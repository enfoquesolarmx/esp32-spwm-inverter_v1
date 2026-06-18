

<img src="/mcpwm_01.png" width="600" alt="Split-phase a 20 kHz">
<img src="/mcpwm_02.png" width="600" alt="Split-phase a 20 kHz">
<img src="/mcpwm_03.png" width="600" alt="Split-phase a 20 kHz">

# Zero-Crossing Polarity-Transition Delay en un Inversor SPWM de Puente H (ESP32 / MCPWM)

> **Implementación de un *zero-crossing / polarity-transition delay* sub-10 µs vía fault handler del
> MCPWM, con guarda generada por GPTimer, sobre ESP32 clásico.**
>
> Bitácora de ingeniería de la depuración de un inversor monofásico SPWM unipolar. Lo que empezó como
> "ajustar una banda muerta de 500 ns a 10 µs" terminó destapando seis hipótesis falsas, el pipeline
> oculto del MCPWM, y un *jitter* que no era un bug de software sino un **batido de fase entre osciladores
> acoplados**. La cura: enganche armónico de la portadora a 20 kHz.

**Palabras clave / keywords:** ESP32, MCPWM, SPWM, H-bridge inverter, dead-time, blanking time,
zero-crossing distortion, polarity-transition delay, shoot-through prevention, fault handler,
cycle-by-cycle, GPTimer, phase locking, unipolar PWM.

---

## Glosario de partida — tres conceptos que NO son lo mismo

La literatura comercial mezcla estos términos. Este proyecto los trata como lo que son: cosas distintas,
en lugares distintos del puente, a frecuencias distintas.

| Concepto | Dónde actúa | Frecuencia del evento | Magnitud | ¿En este proyecto? |
|----------|-------------|----------------------|----------|--------------------|
| **Blanking time / dead-time** | Dentro de una rama (HO1/LO1), en cada conmutación de portadora | ~20–23 kHz | ns (300 ns) | Sí, por hardware MCPWM. Nunca tocado. |
| **Zero-crossing / polarity-transition delay** | En el relevo de polaridad de la fundamental (D2/D3) | 120 Hz (2× por ciclo 60 Hz) | µs (~8 µs) | **Sí — es el núcleo de este trabajo.** |
| **Shoot-through** | El fallo que ambos previenen | — | — | Evitado en ambos planos. |

> El *blanking time* protege cada pulso de portadora. El *zero-crossing delay* protege la transición de
> polaridad. Los inversores comerciales serios implementan **los dos**; aquí, durante el zero-crossing
> delay se apaga **todo el puente** (los cuatro transistores), más conservador que el relevo típico.

---

## TL;DR

Construir un inversor SPWM funciona "a la primera". Hacer que el **cruce por cero** sea limpio
—sin shoot-through, sin pulso de polaridad equivocada, con *polarity-transition delay* estable—
es donde se concentra toda la dificultad real del proyecto.

Este documento narra el camino completo: desde una banda muerta de 500 ns que queríamos llevar
a 10 µs, hasta el descubrimiento final de que el *jitter* de la guarda no era un error de código
sino un **batido de fase entre la portadora, la fundamental y el temporizador de software**.
Bajar la portadora de 23 kHz a 20 kHz enganchó las fases y estabilizó todo.

**Lección central:** en sistemas con varios osciladores acoplados, lo que parece ruido de software
puede ser física. El "bug" era armonía.

---

## Tabla de contenidos

1. [Arquitectura del sistema](#1-arquitectura-del-sistema)
2. [El problema del zero-crossing](#2-el-problema-del-zero-crossing)
3. [Diario de depuración: hipótesis y su caída](#3-diario-de-depuración-hipótesis-y-su-caída)
4. [El hallazgo del fault handler](#4-el-hallazgo-del-fault-handler)
5. [Zero-crossing distortion: el pulso espurio de polaridad](#5-zero-crossing-distortion-el-pulso-espurio-de-polaridad)
6. [La resonancia de frecuencia (phase locking)](#6-la-resonancia-de-frecuencia-phase-locking)
7. [Lecciones de ingeniería](#7-lecciones-de-ingeniería)
8. [Estado final y pendientes](#8-estado-final-y-pendientes)

---

## 1. Arquitectura del sistema

Inversor monofásico de puente H que sintetiza una onda senoidal de 60 Hz mediante modulación
SPWM unipolar sobre un ESP32 clásico (WROOM/WROVER, doble núcleo, APB a 80 MHz).

| Señal | Pin | Rol | Conmutación |
|-------|-----|-----|-------------|
| HO1 (D0) | GPIO23 | Alto izquierdo | PWM rápido (portadora ~20–23 kHz) vía MCPWM0A |
| LO1 (D1) | GPIO22 | Bajo izquierdo | Complemento por dead-time (MCPWM0B) |
| HO2 (D2) | GPIO21 | Alto derecho | Brazo lento (60 Hz), por GPIO |
| LO2 (D3) | GPIO19 | Bajo derecho | Brazo lento (60 Hz), por GPIO |

El brazo izquierdo modula la portadora; el brazo derecho releva la polaridad de cada semiciclo.
La onda sobre la carga es la diferencia entre los dos puntos medios del puente.

**Dead-time de la portadora:** 80 ticks ≈ 300 ns por hardware (MCPWM), nunca tocado.
Protege HO1/LO1 contra cortocircuito de rama en cada conmutación de alta frecuencia.

---

## 2. El problema del zero-crossing

El objetivo aparente era simple: los brazos de retorno D2/D3 conmutaban con solo ~500 ns de
separación en el cruce por cero, y queríamos un *polarity-transition delay* de ~10 µs para dar margen
seguro al relevo de polaridad y evitar conducción cruzada durante el cambio de estado.

Lo que parecía un ajuste de un parámetro se convirtió en una cadena de problemas anidados, cada uno
escondido detrás del anterior. La regla que sostuvo todo el trabajo fue una sola:

> **No creer ninguna hipótesis hasta medirla en el analizador lógico.**
> Incluidas las hipótesis "obvias". Especialmente esas.

---

## 3. Diario de depuración: hipótesis y su caída

La banda de guarda se generó con un `GPTimer` (temporizador de propósito general) en modo *one-shot*,
disparado desde la ISR del MCPWM en cada cruce. La guarda medida no coincidía con la configurada, y
cada explicación que parecía correcta caía al medirla.

| # | Hipótesis | Predicción | Medición | Veredicto |
|---|-----------|-----------|----------|-----------|
| 1 | Residuo del contador del GPTimer | Añadir `gptimer_stop()` lo arregla | Sin cambio | **Falsa** |
| 2 | Callback fuera de IRAM | Forzar IRAM baja la latencia | Sin cambio | **Falsa** |
| 3 | Reloj del GPTimer a 500 kHz (no 1 MHz) | Daría escala ×2 | Serial: 994 kHz real | **Falsa** |
| 4 | `gptimer_get_resolution()` miente | Autocalibrar vs `esp_timer` | Resolución correcta | **Falsa** |
| 5 | Contención de núcleo (callback vs ISR) | Anclar a core 0 lo separa | Auditoría: cores separados, sin mejora | **Falsa** |
| 6 | Latencia de arranque del patrón `stop→start` | Timer libre (sin rearmar) la elimina | **De 21 µs a 11 µs** | **CONFIRMADA** |

### El método que sí funcionó

El offset venía de **rearrancar** un GPTimer detenido en cada cruce: el patrón
`stop → set_raw_count → set_alarm_action → start` cargaba ~10 µs fijos de latencia de arranque.

**Solución:** método *libre*. El GPTimer arranca una sola vez en `setup()` y **nunca se detiene**.
En cada cruce, la ISR solo lee la cuenta actual y mueve la alarma:

```c
uint64_t now = 0;
gptimer_get_raw_count(gt, &now);
gptimer_alarm_config_t al = { .alarm_count = now + guard_ticks, /* ... */ };
gptimer_set_alarm_action(gt, &al);
```

Test aislado (sin ISR de portadora, sin nada más):

```
ONE-SHOT con stop/reconfig/start : 21.0  µs   (offset +11 µs)
LIBRE (solo mueve la alarma)     : 11.25 µs   (offset +1.25 µs)
```

La guarda quedó controlable. Modelo medido en el montaje:
`guarda ≈ 7.6 µs + GUARD_TICKS × 0.56 µs`.

---

## 4. El hallazgo del fault handler

Con la guarda fina lista, apareció el problema de fondo: **el brazo rápido D0/D1 no se apagaba
durante la guarda.** Probamos lo evidente y todo falló, hasta entender el pipeline del MCPWM:

```
generador  →  dead-time  →  [FAULT HANDLER]  →  pin
```

| Intento | Dónde actúa | Por qué falla |
|---------|-------------|---------------|
| `duty = 0` | En el **generador** (antes del dead-time) | El dead-time regenera el complemento aguas abajo. D1 sigue saliendo. |
| `GPIO.out_w1tc` sobre el pin | En el **pin** | El pin lo controla el MCPWM vía la matriz GPIO, no el registro GPIO. Sin efecto. |
| **Fault handler (CYC)** | En la **etapa de salida** (después del dead-time) | Fuerza A y B a LOW de forma independiente. **Funciona.** |

El fault handler en modo *cycle-by-cycle* es el **único punto del pipeline** donde forzar la salida a
LOW apaga D1 de verdad sin que el dead-time lo reconstruya. Se dispara enrutando un GPIO de salida a
la entrada de falla F0 por la **matriz interna** (sin cable externo), de modo que `GPIO.out_w1ts`
dispara la falla y `out_w1tc` la suelta — operaciones seguras desde IRAM.

```c
gpio_matrix_in(FAULT_DRIVE, PWM0_F0_IN_IDX, false);
mcpwm_fault_init(MCPWM_UNIT_0, MCPWM_HIGH_LEVEL_TGR, MCPWM_SELECT_F0);
mcpwm_fault_set_cyc_mode(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_SELECT_F0,
                         MCPWM_FORCE_MCPWMXA_LOW,
                         MCPWM_FORCE_MCPWMXB_LOW);
```

---

## 5. Zero-crossing distortion: el pulso espurio de polaridad

Una observación afinada en el analizador reveló un artefacto sutil — un caso de libro de
**zero-crossing distortion**: en el cruce aparecía un pulso de **polaridad equivocada**. La causa era
una desincronización de timing:

- El brazo **rápido** cambiaba su régimen de modulación **al instante** del cruce de signo del seno.
- El brazo **lento** completaba su relevo **~8 µs después**, al final de la guarda.

Durante esa ventana, el brazo rápido ya modulaba "como el semiciclo nuevo" mientras el lento seguía en
transición → la carga veía un diente de tensión de signo incorrecto.

**Solución:** coordinar la liberación. El callback del GPTimer suelta la falla **y** enciende el brazo
lento entrante en el mismo instante. Los dos brazos vuelven juntos. D1 reaparece con el primer ciclo
de D0 (es su complemento por dead-time: llega *con* D0, no antes — comportamiento correcto, no bug).

La banda de guarda introduce una muesca de "todo apagado" en cada cruce. Sobre un semiperiodo de
8.33 ms, ~8 µs representan el 0.1% del tiempo: despreciable para forma de onda y armónicos, y
elimina el artefacto por completo.

---

## 6. La resonancia de frecuencia (phase locking)

El último misterio: la guarda medía estable en un banco de pruebas mínimo, pero en la versión completa
(con rampa de arranque y rampa de frecuencia activas) **saltaba entre dos valores** — un patrón
bimodal, p. ej. 14-12-14-12 µs, no ruido aleatorio.

### Mecanismo

El callback del GPTimer vence en un instante fijo, pero **se ejecuta cuando el CPU está libre**.
Compite con la ISR de portadora, que corre cada ~43 µs:

- Si el vencimiento cae **mientras la ISR corre** → el callback espera → guarda larga.
- Si cae en un **hueco** → ejecuta enseguida → guarda corta.

El que caiga en uno u otro depende de la **fase relativa** entre tres periodos:
la portadora, la fundamental de 60 Hz, y el temporizador de guarda. A 23 kHz esos periodos eran
**inconmensurables**: la fase deslizaba de cruce a cruce, y el callback alternaba entre "alcanzo la
ISR" y "no la alcanzo" → bimodal.

### La cura accidental

Bajar la portadora a **20 kHz** (periodo exacto de 50 µs) llevó la relación de fases a una **zona de
enganche**: el vencimiento de la guarda aterriza siempre en el mismo punto relativo del ciclo de la
ISR. La fase dejó de deslizar. Resultado:

```
23 kHz : 8 – 11 – 7 – 10 µs   (batido de fase, bimodal)
20 kHz : 8.75 µs constante    (fases enganchadas)
```

Es el mismo fenómeno que un estroboscopio "congelando" una rueda: cuando dos frecuencias entran en
relación armónica, el batido entre ellas desaparece.

> No fue suerte ni parche. Fue **estabilidad por diseño de frecuencias**.
> Y 20 kHz tiene un bono: queda por encima de la audición humana → inversor inaudible.

### Advertencia honesta

El enganche es específico de la combinación **20 kHz + 60 Hz**. Si se usa la rampa de frecuencia
(p. ej. bajar a 50 Hz), `sampleNum` cambia y la fase puede salir de la zona de enganche → el jitter
podría reaparecer. No es grave: en el cruce por cero, ±2 µs de guarda son irrelevantes (es paso por
cero, no conmutación de potencia). Pero conviene saber *por qué* pasa, no solo *que* pasa.

---

## 7. Lecciones de ingeniería

**Por qué casi no hay documentación de esto en la web.** Los inversores SPWM con cruce limpio cruzan
tres dominios que rara vez domina la misma persona: electrónica de potencia (brazos, dead-time,
freewheeling), sistemas de tiempo real embebidos (ISRs, contención de núcleo, jitter), y teoría de
señal (modulación, armónicos, fase). El conocimiento está fragmentado y mucho del que existe es
"funciona en mi placa" sin el porqué.

**El jitter era información, no error.** Quien no hubiera experimentado con la frecuencia portadora
podría haber descartado el proyecto como un bug irresoluble de software. Era física: tres osciladores
acoplados con relaciones de fase que deslizan o se enganchan según sus proporciones.

**Medir vence a teorizar.** Seis hipótesis razonables sobre el offset del GPTimer cayeron una tras
otra al medirlas. La séptima, confirmada por medición, fue la correcta. Cada vez que se aceptó una
explicación sin medirla, costó iteraciones.

**Conocer el pipeline del hardware es decisivo.** `duty=0` y `out_w1tc` parecían soluciones obvias y
ambas fallan porque actúan en el punto equivocado del pipeline `generador→dead-time→fault→pin`. Sin
ese modelo mental, el fault handler nunca habría aparecido como respuesta.

**La diferencia entre depurar y entender.** Depurar hace que el síntoma desaparezca. Entender explica
por qué desaparece — y por eso ahora se sabe que a 50 Hz el jitter podría volver, sin que ello asuste,
porque el fenómeno está identificado.

---

## 8. Estado final y pendientes

### Resuelto

- [x] Banda de guarda fina y ajustable en el cruce (GPTimer, método libre)
- [x] Apagado real del brazo rápido D0/D1 en el cruce (fault handler en etapa de salida)
- [x] Eliminación del pulso espurio de polaridad (liberación coordinada con el brazo lento)
- [x] Guarda estable sin jitter (portadora a 20 kHz, fases enganchadas)
- [x] Arranque suave (rampa de amplitud 30→90% en 3 s) contra inrush de magnetización
- [x] Rampa de frecuencia en tiempo real (50↔60 Hz)
- [x] Shadow del comparador en TEP (causa raíz del "pulso ancho" resuelta)
- [x] Autotest de registros por serial contra silicio
- [x] **Escalado de `tmrRegVal` — verificado y descartado como falsa alarma.** El diagnóstico de
  silicio original sugería una base de 16 MHz asumida (tick 62.5 ns), pero la verificación numérica
  (cálculo vs registro vs APB real) confirmó que `tmrRegVal` calculado = periodo en registro = **347**,
  frecuencia 22.989 kHz por ambas vías, y `amplitude` = 312 = **89.9%** del periodo real. El 62.5 ns
  proviene de 80 MHz ÷ prescaler 5, **no** de una base de 16 MHz — fue una coincidencia numérica (ese
  tick puede salir de dos caminos). `tmrRegVal`, `amplitude` y `sampleNum` están sobre la base correcta.
  No había factor de escala. *Lección: medir cerró en un minuto lo que la sospecha habría dejado abierto
  indefinidamente.*

### Pendiente — roadmap por fases

El proyecto está en marcha. Los pendientes se ordenan de lo inmediato a lo ambicioso: la regla que
sostuvo el trabajo de hoy (medir antes de creer, aislar una variable) aplica con más fuerza a medida
que se sube de fase, porque el costo del error crece — de un pulso espurio en el analizador a corrientes
circulantes que funden MOSFET en microsegundos.

#### Fase A — Cierre del núcleo actual (caminar firme)

- [ ] Verificar el enganche de fase a 50 Hz tras usar la rampa de frecuencia.
- [ ] Validar el comportamiento del cruce con carga inductiva real (ruta de freewheeling durante la
  guarda).

#### Fase B — Robustez de potencia (trotar)

- [ ] **Dead-time adaptativo por temperatura del MOSFET.** Los tiempos de conmutación del MOSFET
  (retardo de apagado por la carga de Miller / Qgd) varían con la temperatura de unión: un dead-time
  fijo seguro en frío puede quedar ajustado en caliente, y uno holgado en caliente desperdicia
  eficiencia en frío. Plan: leer temperatura (NTC en disipador o sensor del módulo) y modular los
  80 ticks del dead-time según una curva, reconfigurando `mcpwm_deadtime_enable` desde el loop sin
  introducir glitch en la conmutación.
- [ ] **Protección de sobrecorriente por hardware reutilizando el fault handler.** El fault handler ya
  está montado y enrutado por software; conectarle una entrada de falla real (sensor de corriente +
  comparador) daría apagado en nanosegundos ante shoot-through o sobrecarga, sin pasar por software.
  Reutiliza la infraestructura ya construida para seguridad de potencia real.
- [ ] **Realimentación de tensión de salida (lazo cerrado).** Hoy el inversor es lazo abierto (amplitud
  fija). Medir la salida y ajustar la amplitud regularía ante variaciones de carga y de bus DC.

#### Fase C — Detección de cruce sincronizada y operación en paralelo (correr)

> La culminación del proyecto. Es un proyecto en sí mismo, no una función más: el reparto de corriente
> entre inversores es un problema de estabilidad de control y de hardware donde el error no es cosmético.
> Las piezas dominadas en las fases anteriores (detección de cruce determinista, control fino de
> amplitud, control del índice de tabla `i`) son **prerrequisitos directos** de esta fase.

- [ ] **PLL por software** que enganche la tabla de seno interna a una referencia externa: detector de
  fase + filtro de lazo + ajuste fino de `freqMod` e índice `i` para llevar el error de fase a cero.
  Es el componente común a todo lo demás de esta fase.
- [ ] **Paralelo en isla (microred sin red) con droop control.** Varios inversores alimentando una carga
  común sin referencia de red. El reto central es el reparto de carga: diferencias mínimas de fase o
  amplitud entre inversores generan corriente circulante destructiva. El *droop control* (reducir
  frecuencia y amplitud según la potencia entregada, imitando generadores síncronos) reparte carga
  **sin comunicación entre inversores** — sincronización por física, no por cable.
- [ ] **Grid-tie (atado a red), opcional y regulado.** Sincronización con la red existente vía PLL +
  protección anti-islanding (requisito legal de interconexión en la mayoría de jurisdicciones). Solo
  si el objetivo evoluciona a inyección a red; entra en territorio normativo y de certificación.
- [ ] **(por definir)** — espacio reservado para el siguiente hallazgo que esta arquitectura destape.

#### Transversal

- [ ] **Compensación de zero-crossing distortion residual.** Aunque la guarda eliminó el pulso espurio,
  toda banda muerta introduce una pequeña distorsión armónica en el cruce; los inversores de calidad la
  compensan inyectando una corrección en el ciclo de duty adyacente al cruce.

> **Disciplina de avance:** cada salto hacia "correr" se da con la misma metodología de hoy — medir
> antes de creer, aislar una variable, validar en banco con carga controlada (resistiva → inductiva →
> real) antes de cualquier conexión seria. No acelerar: encadenar caminatas firmes.

---

## Contexto: esto tiene nombre propio en la industria

Tras resolverlo midiendo, encontramos que la técnica implementada aquí corresponde a lo que los
inversores comerciales llaman **zero-crossing delay** o **polarity-transition delay**: una banda muerta
aplicada específicamente en la transición de polaridad de la fundamental, distinta del *blanking
time / dead-time* que protege cada conmutación de la portadora.

No es una rareza de este montaje: es **práctica estándar de inversores serios**. El cruce por cero es
el punto más propenso a conducción cruzada porque es donde la corriente de la carga inductiva cambia de
dirección y la corriente de freewheeling busca camino mientras los transistores conmutan. Por eso la
industria le da un delay con nombre propio, separado del dead-time genérico.

Para profundizar con fuentes rigurosas (no divulgación, que tiende a mezclar los términos):
notas de aplicación de fabricantes de gate drivers (Infineon, Texas Instruments, STMicroelectronics)
sobre *dead-time insertion*; y, por separado, literatura sobre *zero-crossing distortion in unipolar
PWM inverters*. En material serio, "dead-time/blanking" y "zero-crossing/polarity-transition delay"
aparecen como conceptos distintos con valores distintos — no como sinónimos.

La diferencia de este trabajo no es haber inventado la técnica, sino haber llegado a ella **entendiendo
cada capa**: por qué `duty=0` no apaga el complemento, por qué el fault handler sí, por qué el GPTimer
libre elimina la latencia de arranque, y por qué 20 kHz engancha la fase. La mayoría de implementaciones
funcionan sin que su autor pueda explicar el porqué. Esta está documentada hasta el mecanismo.

---

## Notas de seguridad

Este proyecto controla una etapa de potencia de puente H. Antes de conectar carga real:

- Verificar que los MOSFET/IGBT tengan diodos de freewheeling (integrados o externos) para dar ruta a
  la corriente inductiva durante la banda de guarda.
- El dead-time de 300 ns de la portadora **nunca** debe deshabilitarse: es la protección contra
  cortocircuito de rama en HO1/LO1.
- Los 8 µs de guarda en el cruce protegen el relevo de D2/D3, pero la seguridad última depende de la
  topología y de los tiempos de apagado reales de los transistores empleados.

---

*Bitácora de un proyecto donde el cruce por cero — el punto donde la onda no es nada — resultó ser
donde se escondía toda la complejidad.*
