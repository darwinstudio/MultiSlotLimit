# MultiSlotLimit

可复用的限位开关检测库，基于 FreeRTOS，适用于 STM32 平台。

## 特性

- 基于**硬件定时器溢出中断（ISR）扫描** + 时间去抖检测（非任务轮询）
- 通过配置表抽象硬件，零直接硬件引用（GPIO 经 `sl_hw_table`，定时器经 `SL_TIM_HANDLE`）
- 静态内存分配（无 malloc）
- `__weak` 回调，方便宿主项目处理触发事件
- 支持自定义初始化逻辑（`SL_OpenCustomInit`）
- 去抖采用"连续稳定时间"模型，与定时器周期解耦（`SL_TIMER_PERIOD_US` 单一真相源）

## 文件结构

```
slot_limit.h                   # 公共 API
slot_limit.c                   # 实现
slot_limit_config_template.h   # 配置模板
```

## 集成步骤

### 1. 添加为 git submodule

```bash
git submodule add <repo_url> MultiSlotLimit
```

### 2. 创建项目配置文件

复制 `slot_limit_config_template.h` 到宿主项目的 include 目录，重命名为 `slot_limit_config.h`：

```c
#ifndef __SLOT_LIMIT_CONFIG_H_
#define __SLOT_LIMIT_CONFIG_H_

#define SL_COUNT  7  // 限位开关数量

// 如果项目已有枚举，直接 #define SL_Id_e 并 typedef
typedef enum {
    LIMIT_ID_0,
    LIMIT_ID_1,
    // ...
    LIMIT_ID_NUMS
} SL_Id_e;

// 扫描定时器：CubeMX 配好 PSC，使 ARR=SL_TIMER_PERIOD_US 时溢出周期为对应微秒
// htim2 为 CubeMX 生成的全局 TIM 句柄（宿主工程已可见，无需 extern）
#define SL_TIM_HANDLE       htim2
#define SL_TIMER_PERIOD_US  1000  // 扫描周期(us)，同时作 ARR 值与去抖步长
#define SL_STABLE_TIME_US   3000  // 去抖稳定时间(us)：电平连续一致累计达到此值即确认

// 可选覆盖
// #define SL_TASK_STACK_SIZE 512
// #define SL_TASK_PRIORITY   6

#endif
```

**重要**: 确保 `slot_limit_config.h` 所在目录在 include 路径中优先于 `MultiSlotLimit/` 目录。

### 运行前提（否则编译失败或定时器静默失效）

1. **开启 HAL 定时器回调注册**：在宿主的 `stm32f4xx_hal_conf.h` 中
   `#define USE_HAL_TIM_REGISTER_CALLBACKS 1`。本库通过
   `HAL_TIM_RegisterCallback` 注册溢出回调，未开启时该宏为空，定时器不会触发扫描
   （库已在 `slot_limit.h` 中加编译期 `#error` 检查）。
2. **定时器中断优先级**：`SL_TIM_HANDLE` 对应的 TIM 中断优先级须 **≤ `configMAX_SYSCALL_INTERRUPT_PRIORITY`**，
   否则 `xTaskNotifyGiveFromISR` 等 FromISR API 非法。该值在 `FreeRTOSConfig.h` 中设置。
3. **PSC 配置**：用 CubeMX 配好 TIM 预分频，使 `ARR = SL_TIMER_PERIOD_US` 时溢出周期恰为
   `SL_TIMER_PERIOD_US` 微秒（如 1000 → 1ms）。库只设 ARR，不碰 PSC。

### 3. 定义硬件配置表

在宿主项目的某个 `.c` 文件中定义 `sl_hw_table[]`：

```c
#include "slot_limit.h"

const SL_HwConfig_t sl_hw_table[SL_COUNT] = {
    [LIMIT_ID_0] = { LIMIT_1_GPIO_Port, LIMIT_1_Pin, 1 },  // active=HIGH
    [LIMIT_ID_1] = { LIMIT_2_GPIO_Port, LIMIT_2_Pin, 1 },
    // ...
};
```

### 4. CMake 集成

在 CMakeLists.txt 中添加源文件和 include 路径：

```cmake
# 源文件
${CMAKE_CURRENT_SOURCE_DIR}/../../MultiSlotLimit/slot_limit.c

# Include 路径（放在项目 include 目录之后）
${CMAKE_CURRENT_SOURCE_DIR}/../../MultiSlotLimit
```

### 5. 初始化和使用

```c
#include "slot_limit.h"

// 系统启动时
SL_Init();

// 电机运动前，开启限位检测
SL_Open(LIMIT_ID_0);

// 查询限位状态
SL_State_e state = SL_GetStatus(LIMIT_ID_0);
if (state == SL_STATE_TRIGGER) {
    // 已在限位位置
}

// 电机停止后，关闭限位检测
SL_Close(LIMIT_ID_0);

// 强覆盖触发回调
void SL_TriggerExecute(SL_Id_e id) {
    // 停止对应电机
    Motor_StopByLimit(id);
}
```

## 自定义初始化逻辑

默认情况下，`SL_Open` 根据当前 GPIO 电平设置初始状态：
- active 电平 → 直接进入 WAIT_TRIGGER
- idle 电平 → 进入 OPEN（先等 idle 再等 active）

对于需要反转逻辑的限位（如半圆形挡片），覆盖 `SL_OpenCustomInit`：

```c
uint8_t SL_OpenCustomInit(SL_Id_e id) {
    if (id == LIMIT_ID_MICRO_UP_DOWN) {
        return 1;  // 反转：先找 idle 边沿，保证停止位置一致
    }
    return 0;
}
```

## API 参考

| 函数 | 说明 |
|------|------|
| `SL_Init()` | 创建 FreeRTOS 触发任务，并配置/启动扫描定时器（设 ARR、注册溢出回调） |
| `SL_Open(id)` | 开启限位检测 |
| `SL_Close(id)` | 关闭限位检测 |
| `SL_GetStatus(id)` | 去抖读取（5ms 延时） |
| `SL_GetStateNoDelay(id)` | 原始 GPIO 读取 |
| `SL_HardStop(id)` | `__weak` 硬停回调（ISR 中立即调用，停电机） |
| `SL_TriggerExecute(id)` | `__weak` 触发回调 |
| `SL_OpenCustomInit(id)` | `__weak` 初始化自定义 |
