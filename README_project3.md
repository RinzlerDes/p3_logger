# P3 — AL-24 "AmbuLog" · Ambulatory Vitals Logger — Starter

SWEN 563 / CMPE 663 / EEEE 663 — **FreeRTOS** project (STM32CubeMX HAL + FreeRTOS,
CMake + Ninja + VS Code). Full requirements: [`../RTES_Project3_Spec.md`](../RTES_Project3_Spec.md).

> **This project builds on P2.** The instructor will show you the **P2 solution** (`P2_PatientVitals_Solution`). Start your P3 code from it.
> The sensor drivers, the OLED/console/RGB modules, and the I²C-bus IO layer continue here, unchanged at the silicon level.
> The *new* material in P3 is the **RTOS** on top of them (tasks, a queue, mutexes, an ISR→task handoff, a flash log, and an idle-hook CPU meter).
> The section "What changed from P2" below is your map.

---

## 0. One-time setup: add the P3 peripherals in CubeMX

The `.ioc` shipped here is the **P2 peripheral set, renamed to `P3_Logger`**. You regenerated this set in P2, so it is a known-good start point. P3 adds three items. You add them **in CubeMX** (not by hand), so that the project survives regeneration (spec R32). Open `P3_Logger.ioc` and do the steps below:

**A. FreeRTOS**

1. *Middleware → X-CUBE / Software Packs → FREERTOS* → **Enabled**.

2. **Interface: `CMSIS_V2`**.

3. *Config parameters* → Memory management: **`heap_4`**.

4. *Config parameters → Hook function related definitions*:
   - `USE_IDLE_HOOK` → **Enabled**  (drives the CPU meter — spec R27)
   - `CHECK_FOR_STACK_OVERFLOW` → **`Option2`**  (spec R5)

5. Keep the default task as generated (we replace its body — see §1).

**B. Move the HAL timebase off SysTick** (CubeMX shows a message for this immediately when you enable FreeRTOS, because SysTick becomes the RTOS tick):

6. *System Core → SYS → Timebase Source* → **TIM16**.  *(Record the timer that you select — spec R1 tells you to name it. TIM16 and TIM17 are free here.)*

**C. QUADSPI** (the log store — S25FL128S NOR flash)

7. *Connectivity → QUADSPI* → mode **Bank1 with Quad SPI Lines** (single-bank).

8. Pins (UM2825-verified): **CLK=PA3, BK1_NCS=PD3, BK1_IO0=PB9, BK1_IO1=PD5,
   BK1_IO2=PD6, BK1_IO3=PD7**. CubeMX usually assigns these automatically for the
   WB5MM-DK. Make sure that the result agrees with these pin names.

9. The important parameters: **`Flash Size` = 23** (2^(23+1) = 16 MB). Keep the clock prescaler at its default. Keep Indirect mode (the default). The driver here does not use memory-mapped mode.

Then select **`Generate Code`**. After the generation you have `Middlewares/Third_Party/FreeRTOS/…`,
`Core/Src/freertos.c`, `Core/Inc/FreeRTOSConfig.h`, and `MX_QUADSPI_Init()` with
an `hqspi` handle in `main.c`.

## 0.1 Generated-file touchpoints (3 small edits, all in USER CODE sections)

CubeMX owns `main.c` and `freertos.c`. Put these edits in the `USER CODE` fences, so that regeneration keeps them (spec R32).

1. **`Core/Src/main.c`** — include + POST + task creation:
   - `USER CODE BEGIN Includes`:  `#include "tasks.h"`
   - `USER CODE BEGIN 2` (after all `MX_*_Init()` functions):  `app_post();`

2. **`Core/Src/main.c`** — make the application tasks. 
   - In the `USER CODE BEGIN RTOS_THREADS` fence of `Core/Src/main.c` (it is after `defaultTaskHandle osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes)`) put `app_start();`

3. **`Core/Src/app_freertos.c`** — the idle-hook counter (the CPU meter, spec R27).
   In `vApplicationIdleHook()`:
   - top `USER CODE` include:  `#include "perf_mon.h"`
   - hook body `USER CODE`:  `g_idle_count++;`   *(nothing else — no OS calls)*

*(These three are the only edits to generated files. The tutor or the solution can apply them for you. Tell the tutor when your regenerated tree exists.)*

## 0.2 Build & run

> **§0 is a prerequisite for the first build.** As shipped, this tree does **not** compile. `cmsis_os2.h` (FreeRTOS) and the QSPI HAL come only from the CubeMX generation in §0. Do that generation **in the CubeMX GUI, by hand**. Do not try to script it, and do not hand-copy middleware into the tree (spec R32 — the project must survive regeneration).

```
cmake --preset Debug
cmake --build --preset Debug
```
Flash the `.elf`/`.bin` (VS Code `Run and Debug`, or STM32CubeProgrammer). Open
the VCP at **115200 8N1**. The smoke test (§2) prints a POST report, a
heartbeat, and a 1 Hz UI line with a live **CPU %**.

---

## 1. What is in the box

**Provided — your code calls these, and you do not write them** (the same model as the P2 sensors):

| File | Role |
|---|---|
| `Drivers/Components/stts22h/`, `ism330dhcx/` | Temp + IMU component drivers — **carried from P2 unchanged**. |
| `Drivers/Components/s25fl128s/` | **NEW** thin QSPI-NOR flash driver (JEDEC id, read, page-program, sector/bulk erase, WIP poll). Datasheet-correct, single-line commands. |
| `Drivers/Components/vl53l0x/` | **NEW** minimal ToF driver. ⚠️ **Read the header banner** — the register map is not in the course datasheet. This is a documented minimal single-shot read. **Validate it on hardware.** It is swappable for the official ST API. |
| `App/oled.c`, `console.c`, `rgb_led.c` | Course display/console/LED modules — **carried from P2 unchanged** (silicon logic reused. You call them from a task, and you guard shared peripherals with a mutex). |
| `App/vitals_bus.c` | P2 I²C3 IO layer — **carried and extended** with the VL53L0X binding. |
| `App/perf_mon.c` | **NEW** idle-hook CPU-utilization meter (spec R27/R28). Working. |
| `App/cmd_table.c` | **NEW provided command-table console parser** (name → handler → help): non-blocking line assembly, tokenize, dispatch. **You write the R25 handlers and the ~50 ms tick integration** — the parser is support code, and the integration is the skill. Type `HELP` at the smoke-test prompt to see it work. |
| `App/ui_pages.c` | **NEW provided LIVE + LOG page renderers** (formatting over `oled.c`, B1 page cycling). The **TASKS page is a stub — you write it** (stack high-waters + queue depth, the spec-R5 evidence path). |

**Skeletons — you complete these** (marked `STUDENT TODO`):

| File | You build |
|---|---|
| `App/log_store.c` | The append-only flash log. CRC-16, geometry, the `FORMAT` erase path, status, **and the header-sector struct + rotating-slot layout** are given. **Page-batched append, recovery-on-boot, the slot-rotation update, overflow, and read-back are yours** (spec R15–R21). |
| `App/tasks.c` | Ships a **working smoke test** (2 tasks + a one-verb command table). You replace it with the **4-task logger**, and you extend the command table to the full R25 set. The large comment block at the end of the file gives the target architecture. |
| `App/ui_pages.c` (TASKS page) | The one renderer you write: per-task stack high-waters through `uxTaskGetStackHighWaterMark` and the queue depth/high-water (demo D2). |

`CMakeLists.txt` lists all the `App/` and `Drivers/Components/` files as shipped. When you complete §0, the `.ioc`-driven `cmake/stm32cubemx/CMakeLists.txt` adds FreeRTOS and the QSPI HAL automatically.

---

## 2. The smoke test (what `tasks.c` proves today)

Before you write logger code, the starter shows that the full stack operates:

- **FreeRTOS schedules** two tasks (a heartbeat + a UI task).

- **OLED + console** work from a task (the `AL-24 LIVE` page through the provided renderer, the live temp, and a console that accepts commands — type `HELP`).

- **QSPI flash** answers its JEDEC id at POST (`S25FL128S OK … 16 MB`).

- **The idle-hook CPU meter** shows a live `CPU %` each second.

- **A mutex** guards the I²C bus around the temp read as shipped (spec R7 pattern).

That is your proof that the board, the toolchain, and the RTOS configuration are correct. Then you build the full AL-24.

---

## 3. What you build (the AL-24)

Extend the smoke test into the four-task logger (the spec and the trailing comment in `tasks.c` give the full description):

```
 PD2 EXTI (IMU DRDY) --give--> imuDrdySem --> ImuSampleTask --+
                                                              +--(i2c3Mutex)--> hi2c3
                        VitalsSampleTask (1 Hz, osDelayUntil) -+
   ImuSampleTask, VitalsSampleTask --PUT--> vitalsQueue --GET--> LoggerTask
                                                              --(flashMutex)--> log_store -> QSPI
   UITask: OLED + console + %CPU + RGB;  DOWNLOAD / FORMAT ----(flashMutex)--> log_store -> QSPI
```

The two showpiece behaviors (spec D5/D7): **DOWNLOAD the log through the UART while the recorder continues to append** (the flash mutex arbitrates the reader/writer race, and you can see that `%CPU` increases), and **FORMAT as the only erase path** (steady-state logging only page-programs). `app_start()` makes the synchronization objects (`flashMutexHandle`, `i2c3MutexHandle`, `vitalsQueueHandle`, `imuDrdySemHandle`) for you.

Console commands (spec R25): the **parser is a provided module** (`cmd_table.c`). You write the **handlers** (`HELP`, `STATUS`, `MODE`, `SET PERIOD`, `SET THRESH`, `FORMAT` — confirmation token necessary — `DOWNLOAD`, `CPU`), and you select **the task that owns the ~50 ms non-blocking console tick**. The device echoes all accepted settings as logged `EVENT` lines. OLED pages (spec §3): the starter supplies the LIVE and LOG renderers (`ui_pages.c`), and the **TASKS page is yours**.

---

## 4. What changed from P2 → P3 (your migration map)

| Area | P2 (super-loop) | P3 (FreeRTOS) |
|---|---|---|
| **Control flow** | one `app_service()` loop calling each “task” function in turn | independent **FreeRTOS tasks** with priorities and periods (`osThreadNew`, `osDelayUntil`) |
| **Timebase** | HAL tick on **SysTick** | SysTick is the **RTOS tick**. The HAL tick moves to **TIM16** (§0.B). `HAL_GetTick()` stays your log timestamp |
| **Sensor bus** | one caller, no contention | temp + IMU + **ToF** on one I²C3, multiple task callers → **`i2c3Mutex`** around all transactions (spec R7) |
| **New hardware** | — | **QUADSPI + S25FL128S** flash log; **VL53L0X** ToF (both drivers provided) |
| **Producer/consumer** | direct calls | samplers → **`vitalsQueue`** → LoggerTask (spec R8) |
| **Shared resource** | — | **`flashMutex`** guards the flash (LoggerTask writes ↔ DOWNLOAD reads) — the priority-inversion lesson (spec R6, L13) |
| **ISR handling** | polled/EXTI flag in loop | IMU DRDY EXTI **gives a semaphore** and yields. The task does the read (spec R4/R10) |
| **Performance** | not measured | **idle-hook %CPU** meter (spec R27/R28) |
| **`app.c`** | the state machine | **discarded** — its control flow does not port. Salvage the *sensor-read + threshold logic* into the new task bodies |
| **Config** | I²C3/SPI1/USART1/TSC/EXTI | + **FREERTOS + QUADSPI + TIM16 timebase** (§0) |
| **Build** | App/ + Components in top-level `CMakeLists.txt` | same, plus CubeMX auto-adds FreeRTOS + QSPI HAL sources |

You know the “carried” rows of §1 from P2. The RTOS column is the new material, and it is exactly what lectures **L11–L15** teach.

---

## 5. Notes and pitfalls

- **No `HAL_Delay()` / busy-wait after `osKernelStart()`** (spec R29). Pre-scheduler init (`oled_init`, POST) can block. Tasks use `osDelay`/`osDelayUntil`.

- **Erase blocks** (up to ~650 ms/sector). Confine it to `FORMAT`. Never hold a mutex across an erase on the logging path (spec R19). The flash driver header gives all the timing numbers.

- **VL53L0X** is the one component to validate on the bench before a graded demo (see its header banner). Its POST check is **non-fatal**, so the smoke test boots even when the ToF is flaky.

- **Two unlocked variables on purpose** — the latest-sample snapshot of the UI and `g_idle_count`. The report must justify why they need no lock while the flash does (spec R11).

- **`osMutexNew` inherits by default** in CMSIS-RTOS2, regardless of the attribute bit. For the GR-A “inheritance OFF” experiment, use a plain binary semaphore (spec §8).
