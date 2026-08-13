# CODEBUDDY.md

This file provides guidance to CodeBuddy Code when working with code in this repository.

## 项目是什么

MultiSlotLimit 是一个可复用的限位开关（限位开关）检测库，面向 STM32（F4 HAL）+ FreeRTOS。
它是一个**以 git submodule 形式嵌入宿主 STM32CubeMX 项目的头文件/源文件库**——自身没有
构建系统、没有测试、没有 lint 配置，也无法独立编译（见下文"构建"）。

三个源文件：

- `slot_limit.h` — 公共 API、默认配置宏、编译期检查。
- `slot_limit.c` — 实现（定时器 ISR、去抖状态机、FreeRTOS 任务）。
- `slot_limit_config_template.h` — 供宿主复制为 `slot_limit_config.h` 的模板。

## 构建

本仓库没有构建命令。库只能作为宿主 STM32 项目的一部分进行编译。

集成步骤（来自 `README.md`）：

1. 添加为 submodule：`git submodule add <repo_url> MultiSlotLimit`
2. 将 `slot_limit_config_template.h` 复制到宿主项目的 include 目录，重命名为
   `slot_limit_config.h`，并填入 `SL_COUNT`、`SL_Id_e`、`SL_TIM_HANDLE`、
   `SL_TIMER_PERIOD_US`、`SL_STABLE_TIME_US`。该配置目录必须在 include 路径中
   **优先于** `MultiSlotLimit/` 目录。
3. 加入 CMake：源文件 `.../MultiSlotLimit/slot_limit.c`，include 目录 `.../MultiSlotLimit`。
4. 在宿主的某个 `.c` 文件中定义 `const SL_HwConfig_t sl_hw_table[SL_COUNT]`。
5. 启动时调用 `SL_Init()`；运动期间调用 `SL_Open(id)` / `SL_Close(id)`。

宿主侧前置条件（缺少以下任一项，要么编译失败，要么定时器静默失效）：

- 在 `stm32f4xx_hal_conf.h` 中 `#define USE_HAL_TIM_REGISTER_CALLBACKS 1`
  （由 `slot_limit.h:10` 的 `#error` 强制检查）。
- TIM 中断优先级必须 ≤ `configMAX_SYSCALL_INTERRUPT_PRIORITY`（否则 FromISR API 非法）。
- CubeMX 必须配置好 TIM 预分频（PSC），使 `ARR = SL_TIMER_PERIOD_US` 时溢出周期恰为该微秒数。
  库只设置 ARR（`SL_Init` → `__HAL_TIM_SET_AUTORELOAD`），从不改动 PSC。

## 架构

**检测模型** — 单个硬件定时器每 `SL_TIMER_PERIOD_US` 微秒溢出一次；其 ISR 扫描所有限位开关，
以"连续稳定时间"模型去抖：电平一致则 `stable += SL_TIMER_PERIOD_US`，不一致则清零；
`stable >= SL_STABLE_TIME_US` 即确认一次状态切换（`slot_limit.c:137-148`）。去抖与定时器周期
解耦——ARR 与去抖步长都源自同一个宏 `SL_TIMER_PERIOD_US`。

**每个限位的状态机**（`SL_Machine_e`，`slot_limit.c:27`）：

```
CLOSE → OPEN（等待 idle）→ WAIT_TRIGGER（等待 active）→ CLOSE + 触发已上报
```

**并发模型 — 无临界区**（`slot_limit.c:11-16`，关键设计决策）：
- `sl_vars`（`stable`、`machine`）的**唯一写者是定时器 ISR**。
- `SL_Open`/`SL_Close` 从不触碰 `sl_vars`；它们只置位单 bit 的 `sl_cmd` 命令标志，由 ISR 在
  下一轮扫描时消费并应用。
- `sl_trig_pending` 由 ISR 置位、由任务清零。

以上字段均为单 bit 纯写 / 单写者字段，在 F407 上无需 `taskENTER_CRITICAL` 即安全。
**切勿为这些字段引入第二个写者**，除非重新加回临界区——整个设计都建立在单写者纪律之上。

**触发路径** — ISR 确认触发 → 立即在 ISR 中调用 `SL_HardStop(id)`（硬停，早于任务）→ 置位
`sl_trig_pending[id]` → `vTaskNotifyGiveFromISR` → `sl_task_entry` 被唤醒并调用
`SL_TriggerExecute(id)`。`SL_HardStop` 运行在 ISR 上下文，`SL_TriggerExecute` 运行在任务上下文。

**配置抽象** — 库中零直接硬件引用。GPIO 来自 `sl_hw_table[]`（宿主定义的
`SL_HwConfig_t`：端口/引脚/触发电平）；定时器来自 `SL_TIM_HANDLE` 宏。读取直接使用
`port->IDR`，以保证 ISR 安全。

**弱回调** — `SL_HardStop`、`SL_TriggerExecute` 与 `SL_OpenCustomInit` 均为 `__weak`
（`slot_limit.c:260`），宿主无需额外配置标志即可覆盖。

## API 说明

- `SL_GetStatus(id)` 阻塞 5 ms（`vTaskDelay`）做两次采样去抖读取；若在 ISR 中调用或 id 越界
  则返回 `SL_STATE_INVALID`。需要原始 GPIO 读取时用 `SL_GetStateNoDelay(id)`。
- `SL_HardStop(id)` 为 `__weak` 硬停回调，在 ISR 确认触发时立即调用（早于任务上下文的
  `SL_TriggerExecute`），用于紧急停电机；运行在 ISR 上下文，禁止阻塞。
- `SL_OpenCustomInit(id)` 返回 1 表示反转开合时的初始状态逻辑（例如半圆形挡片需要先寻找
  idle 边沿）。
- `slot_limit.c` include 了 CubeMX 生成的 `main.h` 与 `tim.h` —— 它假定宿主项目的生成目录结构。

## 代码风格

- 注释使用 doxygen 风格（`/** */`，`@brief`/`@param`/`@return`/`@note`）。
- 函数注释写在 `.c` 实现文件，`.h` 头文件不写函数级注释（仅保留类型/枚举/结构体/宏/文件级注释）。

## 许可证

MIT（见 `LICENSE`）。
