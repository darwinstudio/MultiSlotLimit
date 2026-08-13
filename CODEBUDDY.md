# CODEBUDDY.md

This file provides guidance to CodeBuddy Code when working with code in this repository.

## What this repository is

A reusable, header-only-API C library for **limit switch (限位开关) detection on STM32**, built on FreeRTOS. It is intended to be consumed as a **git submodule** by a host firmware project — it is not built or tested standalone inside this repo. There is no Makefile/CMakeLists/test framework here; all build, flash, and test commands come from the host project that includes `slot_limit.c` and this directory on its include path.

## Files

- `slot_limit.h` — public API + default config macros and type definitions (`SL_State_e`, `SL_HwConfig_t`). Host-supplied config: `SL_TIM_HANDLE`, `SL_TIMER_PERIOD_US`, `SL_STABLE_TIME_US`, `SL_TASK_STACK_SIZE`, `SL_TASK_PRIORITY`. Includes `slot_limit_config.h` (host-supplied) and `stm32f4xx_hal.h`.
- `slot_limit.c` — implementation: hardware-timer ISR scan, time-based debounce state machine, FreeRTOS trigger task, `__weak` callbacks.
- `slot_limit_config_template.h` — template the host copies to its include dir as `slot_limit_config.h` (defines `SL_COUNT`, `SL_Id_e`, timer config).
- `README.md` — integration guide (submodule add, config file, hardware table, CMake include, API usage).

## Architecture (the big picture)

**Hardware abstraction via config table, zero direct hardware references.**
The library never references specific GPIO ports/pins. The host defines two things:
1. `slot_limit_config.h` — sets `SL_COUNT` and the `SL_Id_e` id enum (must match in size).
2. `sl_hw_table[]` — a `const SL_HwConfig_t sl_hw_table[SL_COUNT]` array in some host `.c`, mapping each id to `{port, pin, active}` (active = 1 for HIGH-triggered, 0 for LOW-triggered). The library reads pins only through `sl_read_pin` → direct `GPIOx->IDR` read (ISR-safe, no `HAL_GPIO_ReadPin`), so the table is the sole hardware coupling.

**Timer is host-owned, library drives it.** The host defines `SL_TIM_HANDLE` (a CubeMX-generated TIM handle) and configures its prescaler (PSC) so that `ARR = SL_TIMER_PERIOD_US` yields an overflow period of `SL_TIMER_PERIOD_US` microseconds. `SL_Init()` calls `__HAL_TIM_SET_AUTORELOAD(&SL_TIM_HANDLE, SL_TIMER_PERIOD_US)`, registers `SL_TimerCallback` via `HAL_TIM_RegisterCallback(..., HAL_TIM_PERIOD_ELAPSED_CB_ID, ...)`, and starts it with `HAL_TIM_Base_Start_IT`. The library never picks the TIM instance — the handle comes entirely from host config.

**Include-path precedence is load-bearing.** `slot_limit.h` does `#include "slot_limit_config.h"`. The host's `slot_limit_config.h` directory MUST be on the include path *ahead of* this `MultiSlotLimit/` directory, otherwise the template (not the real config) is picked up. This is the single most common integration mistake.

**Hardware-timer ISR scan + FreeRTOS trigger task (no software polling).**
- `SL_Init()` creates a **statically-allocated** task (`xTaskCreateStatic`, no malloc) with a static stack/TCB sized by `SL_TASK_STACK_SIZE`, and starts the scan timer (see above).
- Each limit has `sl_vars[id]` = `{stable, machine}` where `machine` is one of `SL_MACHINE_CLOSE / OPEN / WAIT_TRIGGER`, and `stable` is a `uint16_t` microsecond accumulator (replaces the old `cnt` count).
- The timer overflow ISR `SL_TimerCallback` runs every `SL_TIMER_PERIOD_US`: it scans **all** limits, consumes `SL_Open`/`SL_Close` command bits, and applies time-based debounce. The task `sl_task_entry` stays blocked on `ulTaskNotifyTake(portMAX_DELAY)` and only wakes when the ISR confirms a trigger (via `xTaskNotifyGiveFromISR`), then runs `SL_TriggerExecute(id)` for each pending id (`sl_trig_pending[]` bitmap). So CPU for limit logic is event-driven, not a 1ms busy poll.
- **Time-based debounce (period-decoupled).** In the ISR, the expected level depends on phase (`OPEN` expects idle, `WAIT_TRIGGER` expects active). A matching level does `stable += SL_TIMER_PERIOD_US`; a mismatch resets `stable = 0`. When `stable >= SL_STABLE_TIME_US` the transition is confirmed: `OPEN` → `WAIT_TRIGGER` (idle seen, now watching for active), `WAIT_TRIGGER` → `CLOSE` + sets pending + notifies task. Because the step is `SL_TIMER_PERIOD_US` (the same macro that sets ARR), changing the scan frequency requires editing only that one macro — no recalculation of a count.

**Initial-state decision lives in `SL_Open`.** On open, it reads the current pin level and sets the starting phase. For normally-active limits it starts in `WAIT_TRIGGER`; for limits that need inverted logic (e.g. a half-disc flag) the host overrides the `__weak` `SL_OpenCustomInit(id)` to return 1, which flips the starting phase so the stop position stays consistent.

**Two query paths:**
- `SL_GetStatus(id)` — debounced: reads, `vTaskDelay(5ms)`, reads again; consistent → confirmed state. Returns `SL_STATE_INVALID` if called inside an ISR (`xPortIsInsideInterrupt`) or bad id. Must NOT be called from interrupt context.
- `SL_GetStateNoDelay(id)` — raw GPIO read, safe everywhere, no delay.

**`__weak` extension points** (host strong-defines to override):
- `SL_TriggerExecute(id)` — stop the motor / handle the trip.
- `SL_OpenCustomInit(id)` — invert open-phase logic per id.

## Editing guidance

- Keep hardware fully abstracted: never add a direct `HAL_GPIO_ReadPin`/port reference outside `sl_read_pin`/`sl_is_active`; extend the config table pattern instead. `sl_read_pin` reads `GPIOx->IDR` directly (ISR-safe).
- **No critical sections by design.** `sl_vars` is written only by the timer ISR; `SL_Open`/`SL_Close` only set `sl_cmd` bits (consumed by the ISR); `sl_trig_pending` is set by ISR and cleared by the task. All are single-bit pure writes or ISR-exclusive writes, so `taskENTER_CRITICAL` is intentionally absent. **Do not reintroduce critical sections** — and if you add a new shared variable, preserve the single-writer rule (ISR writes `sl_vars`, task writes nothing concurrent) rather than guarding with locks.
- The TIM interrupt priority must stay at or below `configMAX_SYSCALL_INTERRUPT_PRIORITY` so `xTaskNotifyGiveFromISR` / `portYIELD_FROM_ISR` are legal. The host sets this in `FreeRTOSConfig.h`.
- `SL_GetStatus` blocks (5ms delay); it is intentionally thread/task-only, not ISR-safe. Do not call it from an interrupt.
- `SL_COUNT` and `SL_Id_e` are host-owned; the library only references them via the config and `sl_hw_table`. Don't hardcode limit counts in the library.
- `SL_TIMER_PERIOD_US` is the single source of truth for scan frequency: it sets both ARR and the debounce step. If the host changes PSC/ARR, they must keep this macro in sync with the real overflow period, or debounce timing silently drifts.
- Changes to stack/priority/timer defaults belong in `slot_limit.h`'s default-macro block, overridable from `slot_limit_config.h`.
