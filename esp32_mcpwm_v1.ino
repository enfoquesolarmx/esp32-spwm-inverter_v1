/*==============================================================================
  INVERSOR PUENTE H — ESP32 (MCPWM)
  SINTESIS: FAULT HANDLER (apaga D0/D1 de verdad) + GPTIMER (guarda fina ~7.5us)
  ------------------------------------------------------------------------------
  COMBINA lo mejor de dos scripts previos:

  - Del script FAULT HANDLER: apaga HO1/LO1 (D0/D1) en la ETAPA DE SALIDA,
    DESPUES del dead-time. Es el unico punto del pipeline que apaga D1 sin que
    el dead-time lo regenere:
        generador -> dead-time -> [FAULT HANDLER] -> pin
    (duty 0 actua en el generador y el dead-time regenera el complemento: por eso
     fallaba. out_w1tc sobre el pin no sirve: lo controla el MCPWM.)

  - Del script GPTIMER: guarda FINA y AJUSTABLE (~7.5us via ticks), en vez de la
    guarda gruesa cuantizada a 1 muestra de ISR (~40us) del modelo por conteo.

  SECUENCIA DEL CRUCE (todo coordinado):
    1. ISR detecta cruce -> dispara FALLA (FAULT_BIT alto): HO1/LO1 a LOW por el
       fault handler. Apaga AMBOS brazos lentos por GPIO. Arma el GPTimer (~7.5us).
    2. Durante la guarda: D0,D1,D2,D3 todos en BAJO. La carga no ve tension.
    3. Callback del GPTimer (fin de guarda): SUELTA la falla (FAULT_BIT bajo) ->
       PWM complementario se reanuda, Y enciende el brazo lento entrante (D2 o D3)
       en el MISMO instante. Brazo rapido y lento vuelven coordinados.

  VALIDAR EN SALEAE:
    - Durante D7 (guarda): D0, D1, D2, D3 TODOS en bajo. Sin pulso espurio.
    - Guarda ~7.5us (no 40us). Ajustable con GUARD_TICKS.
    - Al salir: D1 y el brazo lento entrante suben juntos, PWM unipolar intacto.

  RIESGO A VIGILAR (latencia de liberacion del fault CYC):
    El fault en modo cycle-by-cycle puede liberar las salidas en el siguiente
    evento de timer (TEZ/TEP), no instantaneo. Si se "come" una muestra de PWM al
    reanudar, se vera como un pequeno retardo extra en D0/D1 tras la guarda.
    Mide D1 al salir; si hay retardo, es esta latencia (no un bug del codigo).
 =============================================================================*/

#include "soc/gpio_struct.h"
#include "driver/gpio.h"
#include "driver/mcpwm.h"
#include "driver/gptimer.h"
#include "soc/rtc.h"
#include "soc/gpio_sig_map.h"      // PWM0_F0_IN_IDX
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void gpio_matrix_in(uint32_t gpio, uint32_t signal_idx, bool inv);

/*------------------------------------------------------------------------------
  PINES (puente H)
  ----------------------------------------------------------------------------*/
const int HO1 = 23; // MCPWM0A (PWM rapido)
const int LO1 = 22; // MCPWM0B (complemento por dead-time)
const int HO2 = 21; // rama lenta: positivo  (D2)
const int LO2 = 19; // rama lenta: negativo  (D3)

const int DIAG_CRUCE = 18;          // GPIO18 -> D7: refleja la guarda
#define DIAG_BIT (1 << 18)

/* FAULT_DRIVE: GPIO de salida enrutado a la entrada de falla F0 del MCPWM0 por
   la matriz interna (sin cable externo). GPIO5 libre. */
const int FAULT_DRIVE = 5;
#define FAULT_BIT (1 << 5)

#define HO2_BIT (1 << 21)
#define LO2_BIT (1 << 19)
#define HO1_BIT (1 << 23)
#define LO1_BIT (1 << 22)

/* Registros MCPWM (ESP32 clasico) */
#define MCPWM_CMPR0_REG    0x3FF5E040
#define MCPWM_INT_CLR_REG  0x3FF5E11C
#define MCPWM_CLK_CFG      0x3FF5E000
#define MCPWM_TIMER0_CFG0  0x3FF5E004
#define MCPWM_GEN0_STMP    0x3FF5E03C
#define MCPWM_INT_ENA      0x3FF5E110

/*------------------------------------------------------------------------------
  PARAMETROS DE SENAL
  ----------------------------------------------------------------------------*/
const float freqCarr     = 20000.0;
const int   prescaler    = 5;
const float timer_clk_hz = 80e6;
const float tick_ns      = (1.0 / timer_clk_hz) * prescaler * 1e9;

const int   tmrRegVal  = int(((((1/freqCarr)/2)/(tick_ns*1e-9)) - 1) + 0.5);
float real_freqCarr    = 1.0 / ((tmrRegVal + 1) * tick_ns * 1e-9 * 2);

const float freqMod   = 60.0;
const int   sampleNum = int(real_freqCarr / 60.0);
const float radVal    = 2 * PI / sampleNum;
const int   amplitude = int(0.90 * tmrRegVal);

/*------------------------------------------------------------------------------
  GUARDA (GPTimer, metodo libre). AJUSTE MANUAL DIRECTO.
  guard_ticks = ticks que se suman a la latencia natural del GPTimer (~7.65us).
    0 -> ~7.65us   1 -> ~8.65us   2 -> ~9.66us   3 -> ~10.66us
  Cambia GUARD_TICKS y mide D7.
  ----------------------------------------------------------------------------*/
  //20 = 23.87US
  //10 = 13.25US

#define GUARD_TICKS   10
#define GUARD_CORE    1            // mismo core que la ISR de portadora

/*------------------------------------------------------------------------------
  ESTADO COMPARTIDO ISR <-> CALLBACK
  ----------------------------------------------------------------------------*/
volatile int      i           = 0;
volatile int      prevSign    = 0;
volatile int      pendingSign = 0;
volatile uint32_t guard_ticks = GUARD_TICKS;
volatile int      guard_ready = 0;

gptimer_handle_t gt = NULL;
void initGuardTimer(void* arg);

/*==============================================================================
  CALLBACK DEL GPTIMER (fin de guarda): SUELTA la falla y enciende el brazo
  lento entrante, COORDINADOS. El PWM complementario se reanuda al soltar.
==============================================================================*/
static bool IRAM_ATTR onGuardElapsed(gptimer_handle_t timer,
                                     const gptimer_alarm_event_data_t *edata,
                                     void *user_ctx) {
  /* Soltar la falla -> HO1/LO1 (D0/D1) vuelven al PWM complementario normal */
  GPIO.out_w1tc = FAULT_BIT;
  /* Encender el brazo lento entrante en el MISMO instante */
  if (pendingSign > 0) GPIO.out_w1ts = HO2_BIT;   // semiciclo positivo (D2)
  else                 GPIO.out_w1ts = LO2_BIT;   // semiciclo negativo (D3)
  GPIO.out_w1tc = DIAG_BIT;                         // D7 abajo = fin de guarda
  return false;
}

/*==============================================================================
  ISR DEL MCPWM (~23 kHz). En el cruce: dispara la falla (apaga D0/D1 por la
  etapa de salida), apaga los brazos lentos, arma el GPTimer. NO bloquea.
==============================================================================*/
void IRAM_ATTR MCPWM_ISR(void*) {
  WRITE_PERI_REG(MCPWM_INT_CLR_REG, BIT(3));

  int sineVal = int(amplitude * sin(radVal * i));
  int sign    = (sineVal > 0) ? 1 : -1;

  if (sign != prevSign) {
    /* --- CRUCE: congelar TODO el puente --- */
    GPIO.out_w1ts = FAULT_BIT;                     // falla ON -> D0/D1 a LOW
    GPIO.out_w1tc = HO2_BIT | LO2_BIT;             // D2/D3 a LOW
    GPIO.out_w1ts = DIAG_BIT;                      // D7 arriba = inicio guarda
    WRITE_PERI_REG(MCPWM_CMPR0_REG, 0);            // duty 0 (limpieza, por si acaso)
    pendingSign   = sign;

    /* Armar la guarda (GPTimer libre: alarma = cuenta_actual + guard_ticks) */
    uint64_t now = 0;
    gptimer_get_raw_count(gt, &now);
    gptimer_alarm_config_t al = { .alarm_count = now + guard_ticks,
                                  .reload_count = 0,
                                  .flags = { .auto_reload_on_alarm = false } };
    gptimer_set_alarm_action(gt, &al);
    prevSign = sign;
  }

  /* --- Duty de la portadora (fuera del cruce) ---
     Durante la guarda la falla mantiene D0/D1 en LOW, asi que el valor del duty
     aqui es irrelevante hasta que el callback suelta la falla. Escribimos el
     duty normal: cuando la falla se suelta, el PWM ya tiene el valor correcto. */
  if (sineVal > 0) {
    WRITE_PERI_REG(MCPWM_CMPR0_REG, sineVal);
  } else {
    WRITE_PERI_REG(MCPWM_CMPR0_REG, tmrRegVal + sineVal);
  }

  i++;
  if (i >= sampleNum) i = 0;
}

/*==============================================================================
  INIT DEL GPTIMER (tarea anclada a GUARD_CORE). Metodo libre: arranca una vez
  y nunca se detiene; en cada cruce solo se mueve la alarma.
==============================================================================*/
void initGuardTimer(void* arg) {
  gptimer_config_t tcfg = {
    .clk_src       = GPTIMER_CLK_SRC_APB,
    .direction     = GPTIMER_COUNT_UP,
    .resolution_hz = 1000000,
    .flags = { .intr_shared = false },
  };
  esp_err_t err = gptimer_new_timer(&tcfg, &gt);
  if (err != ESP_OK) {
    Serial.printf(">>> FALLO gptimer_new_timer: %s\n", esp_err_to_name(err));
  }

  /* Calibracion de resolucion real (solo informativa) */
  gptimer_enable(gt);
  gptimer_set_raw_count(gt, 0);
  int64_t t0 = esp_timer_get_time();
  gptimer_start(gt);
  while (esp_timer_get_time() - t0 < 2000) { }
  uint64_t tk = 0; gptimer_get_raw_count(gt, &tk);
  int64_t t1 = esp_timer_get_time();
  gptimer_stop(gt);
  float res = (float)tk / ((float)(t1 - t0) * 1e-6f);
  Serial.printf("GPTimer res: %.0f Hz (%.4f us/tick)  GUARD_TICKS=%u\n",
                res, 1e6f / res, guard_ticks);
  gptimer_disable(gt);

  gptimer_event_callbacks_t cbs = { .on_alarm = onGuardElapsed };
  gptimer_register_event_callbacks(gt, &cbs, NULL);
  gptimer_enable(gt);

  /* Metodo libre: arrancar y dejar corriendo para siempre */
  gptimer_set_raw_count(gt, 0);
  gptimer_start(gt);

  guard_ready = 1;
  vTaskDelete(NULL);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("==== SINTESIS: FAULT HANDLER + GPTIMER ====");

  pinMode(LO2, OUTPUT);
  pinMode(HO2, OUTPUT);
  pinMode(DIAG_CRUCE, OUTPUT);
  pinMode(FAULT_DRIVE, OUTPUT);
  GPIO.out_w1tc = FAULT_BIT | HO2_BIT | LO2_BIT | DIAG_BIT;  // todo en bajo

  mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0A, HO1);
  mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0B, LO1);

  mcpwm_config_t pwm_config;
  pwm_config.frequency    = real_freqCarr * 2;
  pwm_config.cmpr_a       = 0;
  pwm_config.cmpr_b       = 0;
  pwm_config.counter_mode = MCPWM_UP_DOWN_COUNTER;
  pwm_config.duty_mode    = MCPWM_DUTY_MODE_0;
  mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &pwm_config);

  WRITE_PERI_REG(MCPWM_CLK_CFG, 0);
  uint32_t reg_val = ((prescaler - 1) & 0xFF) | ((tmrRegVal & 0xFFFF) << 8);
  WRITE_PERI_REG(MCPWM_TIMER0_CFG0, reg_val);
  WRITE_PERI_REG(MCPWM_GEN0_STMP, 2);

  esp_intr_alloc(ETS_PWM0_INTR_SOURCE, ESP_INTR_FLAG_IRAM, MCPWM_ISR, NULL, NULL);

  /* Dead-time de la portadora: 80 ticks = 300 ns. NO TOCAR. */
  mcpwm_deadtime_enable(MCPWM_UNIT_0, MCPWM_TIMER_0,
                        MCPWM_ACTIVE_HIGH_COMPLIMENT_MODE, 80, 80);

  /*--------------------------------------------------------------------------
    FAULT HANDLER: enruta FAULT_DRIVE (GPIO5) a la entrada de falla F0 por la
    matriz interna; falla en NIVEL ALTO; modo CYC con A->LOW y B->LOW.
    Mientras FAULT_BIT este alto, HO1/LO1 se fuerzan a LOW en la salida.
  --------------------------------------------------------------------------*/
  gpio_matrix_in(FAULT_DRIVE, PWM0_F0_IN_IDX, false);
  mcpwm_fault_init(MCPWM_UNIT_0, MCPWM_HIGH_LEVEL_TGR, MCPWM_SELECT_F0);
  mcpwm_fault_set_cyc_mode(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_SELECT_F0,
                           MCPWM_FORCE_MCPWMXA_LOW,
                           MCPWM_FORCE_MCPWMXB_LOW);

  /* GPTimer de la guarda, en GUARD_CORE (mismo core que la ISR) */
  TaskHandle_t h = NULL;
  xTaskCreatePinnedToCore(initGuardTimer, "guardInit", 4096, NULL, 5, &h, GUARD_CORE);
  while (guard_ready == 0) { delay(1); }

  WRITE_PERI_REG(MCPWM_INT_ENA, BIT(3));

  Serial.println("Listo. Cruce: fault apaga D0/D1, GPTimer da la guarda fina,");
  Serial.println("callback suelta fault y enciende brazo lento, coordinados.");
}

void loop() {
  delay(1000);
}
