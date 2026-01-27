#include "../include/readpaper.h"
#include "device_interrupt_task.h"
#include "timer_interrupt_task.h"
#include "test/per_file_debug.h"
#include "device/powermgt.h"
#include "tasks/state_machine_task.h"
#include "tasks/timer_interrupt_task.h"
#include "task_priorities.h"
#include <cmath>
#include "globals.h"

// Helper struct & task for safe power reads with timeout
typedef struct PowerReadParams {
    TaskHandle_t notifyTo;
    float voltage;
    int percentage;
    bool isCharging;
    volatile bool done;
} PowerReadParams;

static void powerReaderTask(void* pvParameters)
{
    PowerReadParams* p = (PowerReadParams*)pvParameters;
    if (p == NULL)
    {
        vTaskDelete(NULL);
        return;
    }
    // perform potentially blocking reads
    p->voltage = M5.Power.getBatteryVoltage();
    p->percentage = M5.Power.getBatteryLevel();
    p->isCharging = M5.Power.isCharging();
    p->done = true;
    // notify the waiting task
    if (p->notifyTo != NULL)
    {
        xTaskNotifyGive(p->notifyTo);
    }
    // give notifier a short window to copy results, then free params and delete self
    vTaskDelay(pdMS_TO_TICKS(50));
    if (p != NULL)
    {
        vPortFree(p);
    }
    vTaskDelete(NULL);
}

// 静态成员初始化
TaskHandle_t DeviceInterruptTask::taskHandle_ = NULL;
volatile bool DeviceInterruptTask::touchPressed_ = false;
volatile int16_t DeviceInterruptTask::lastTouchX_ = -1;
volatile int16_t DeviceInterruptTask::lastTouchY_ = -1;
volatile float DeviceInterruptTask::lastBatteryVoltage_ = -1.0f;
volatile int DeviceInterruptTask::lastBatteryPercentage_ = -1;
volatile bool DeviceInterruptTask::lastChargingState_ = false;
volatile unsigned long DeviceInterruptTask::lastTouchPressTime_ = 0;
volatile int DeviceInterruptTask::lastOrientation_ = ORIENT_UNKNOWN;
volatile int DeviceInterruptTask::lastOrientationCandidate_ = ORIENT_UNKNOWN;
volatile int DeviceInterruptTask::orientationStreak_ = 0;
volatile unsigned long DeviceInterruptTask::lastOrientationCandidateFirstSeen_ = 0;
volatile unsigned long DeviceInterruptTask::lastOrientationSentTime_ = 0;

bool DeviceInterruptTask::initialize()
{
#if DBG_DEVICE_INTERRUPT_TASK
    Serial.printf("[%lu] [DEVICE_INTERRUPT] 初始化设备中断任务 - 简化轮询模式\n", millis());
#endif

    // 创建设备轮询任务
    BaseType_t result = xTaskCreatePinnedToCore(
        taskFunction,
        "DeviceInterruptTask",
        4096, // 栈大小
        NULL,
        PRIO_DEVICE, // 优先级 (中等)
        &taskHandle_,
        0 // 核心1
    );

    if (result != pdPASS)
    {
#if DBG_DEVICE_INTERRUPT_TASK
        Serial.printf("[%lu] [DEVICE_INTERRUPT] 创建设备中断任务失败\n", millis());
#endif
        return false;
    }

    // 初始化设备状态
    lastBatteryVoltage_ = M5.Power.getBatteryVoltage();
    lastBatteryPercentage_ = M5.Power.getBatteryLevel();
    lastChargingState_ = M5.Power.isCharging();

#if DBG_DEVICE_INTERRUPT_TASK
    Serial.printf("[%lu] [DEVICE_INTERRUPT] 设备中断任务初始化成功，轮询间隔: %dms\n", millis(), DEVICE_INTERRUPT_TICK);
#endif

    // 初始化定时器中断（在任务创建后启动）
    TimerInterruptTask::initialize(DEVICE_INTERRUPT_TICK);

    return true;
}

void DeviceInterruptTask::destroy()
{
#if DBG_DEVICE_INTERRUPT_TASK
    Serial.printf("[%lu] [DEVICE_INTERRUPT] 销毁设备中断任务\n", millis());
#endif

    if (taskHandle_ != NULL)
    {
        vTaskDelete(taskHandle_);
        taskHandle_ = NULL;
    }

    // 停用定时器
    TimerInterruptTask::destroy();

#if DBG_DEVICE_INTERRUPT_TASK
    Serial.printf("[%lu] [DEVICE_INTERRUPT] 设备中断任务已销毁\n", millis());
#endif
}

void DeviceInterruptTask::taskFunction(void *pvParameters)
{
#if DBG_DEVICE_INTERRUPT_TASK
    Serial.printf("[%lu] [DEVICE_INTERRUPT] 任务启动 - 使用定时器唤醒, 周期=%dms\n", millis(), DEVICE_INTERRUPT_TICK);
#endif

    for (;;)
    {
        // 等待 timer ISR 的通知
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // 收到通知后执行轮询工作
        M5.update();
        checkTouchStatus();
        // 检查 IMU 状态并报告当前设备方向（四方向：UP/DOWN/LEFT/RIGHT）
#if ENABLE_AUTO_ROTATION
        {
            // 如果正在进行显示推送，跳过本次方向判断
            extern volatile bool inDisplayPush;
            if (inDisplayPush)
            {
                continue;
            }
            
            float ax = 0, ay = 0, az = 0;
            M5.Imu.getAccelData(&ax, &ay, &az);

            // 简单的方向判定：以 X/Y 平面投影为主，取绝对值较大的分量决定方向
            float absx = fabsf(ax);
            float absy = fabsf(ay);
            float mag = sqrtf(ax * ax + ay * ay + az * az);

            // 仅在接近重力向量存在时才判定（避免在高速运动/失重时误判）
            const float GRAVITY_MIN = 0.6f; // g
            if (mag >= GRAVITY_MIN)
            {
                int dir = ORIENT_UNKNOWN;
                if (absy >= absx)
                {
                    // Y 主导：负值认为设备顶部朝上（UP），正为 DOWN
                    dir = (ay < 0.0f) ? ORIENT_UP : ORIENT_DOWN;
                }
                else
                {
                    // X 主导：负值为 LEFT，正为 RIGHT
                    dir = (ax < 0.0f) ? ORIENT_LEFT : ORIENT_RIGHT;
                }

                // 用户要求的映射： LEFT->UP, UP->RIGHT, RIGHT->DOWN, DOWN->LEFT
                int mapped_dir = ORIENT_UNKNOWN;
                switch (dir)
                {
                case ORIENT_LEFT:
                    mapped_dir = ORIENT_UP;
                    break;
                case ORIENT_UP:
                    mapped_dir = ORIENT_RIGHT;
                    break;
                case ORIENT_RIGHT:
                    mapped_dir = ORIENT_DOWN;
                    break;
                case ORIENT_DOWN:
                    mapped_dir = ORIENT_LEFT;
                    break;
                default:
                    mapped_dir = ORIENT_UNKNOWN;
                    break;
                }

                // 增强去抖逻辑：要求候选在时间窗内持续稳定且达成计数阈值，且与上次发送不同，且满足最小发送间隔
                const int ORIENT_STREAK_THRESHOLD = 4;           // 连续判定次数阈值（提高以减少抖动）
                const unsigned long ORIENT_STABLE_MS = 600;      // 候选必须持续出现的最小时间（ms）
                const unsigned long ORIENT_MIN_INTERVAL_MS = 1000; // 最小发送间隔（ms），避免频繁切换

                int candidate = mapped_dir;
                if (candidate == DeviceInterruptTask::lastOrientationCandidate_)
                {
                    // 连续计数
                    DeviceInterruptTask::orientationStreak_++;
                }
                else
                {
                    // 新候选，记录首次出现时间并重置计数
                    DeviceInterruptTask::lastOrientationCandidate_ = candidate;
                    DeviceInterruptTask::orientationStreak_ = 1;
                    DeviceInterruptTask::lastOrientationCandidateFirstSeen_ = millis();
                }

                // 确认：达到计数阈值 && 候选持续时间达到 ORIENT_STABLE_MS && 与最后发送方向不同 && 满足最小间隔
                if (DeviceInterruptTask::orientationStreak_ >= ORIENT_STREAK_THRESHOLD &&
                    (millis() - DeviceInterruptTask::lastOrientationCandidateFirstSeen_ >= ORIENT_STABLE_MS) &&
                    candidate != DeviceInterruptTask::lastOrientation_ &&
                    (millis() - DeviceInterruptTask::lastOrientationSentTime_ >= ORIENT_MIN_INTERVAL_MS))
                {
                    DeviceInterruptTask::lastOrientation_ = candidate;
                    DeviceInterruptTask::lastOrientationSentTime_ = millis();

                    SystemMessage_t msg;
                    msg.type = MSG_DEVICE_ORIENTATION;
                    msg.timestamp = millis();
                    msg.data.orientation.dir = (uint8_t)candidate;

#if DBG_DEVICE_INTERRUPT_TASK
                    Serial.printf("[DEVICE_INTERRUPT] ORIENTATION confirmed: %s (raw=%s ax=%.3f ay=%.3f az=%.3f)\n", DeviceOrientationToString(candidate), DeviceOrientationToString(dir), ax, ay, az);
#endif

                    if (!sendStateMachineMessage(msg))
                    {
#if DBG_DEVICE_INTERRUPT_TASK
                        Serial.println("[DEVICE_INTERRUPT] sendStateMachineMessage(MSG_DEVICE_ORIENTATION) failed");
#endif
                    }

                    // 发送后重置 streak 以避免重复立刻发送
                    DeviceInterruptTask::orientationStreak_ = 0;
                }
            }
        }
#else
        // 自动旋转被禁用：不读取 IMU，也不发送方向消息
#endif

        // 方案1：检查由 ISR 设置的 2 分钟到期 flag（ISR 中轻量计数）
        if (TimerInterruptTask::isTwoMinuteExpired())
        {
            // 先清理 flag，再处理较重逻辑
            TimerInterruptTask::resetTwoMinuteFlag();
            // 高频唤醒下的周期性电池检查：保留原有计数逻辑（后备）
            // 同时向状态机任务发送2MIN到时事件
            checkBatteryStatus();
            // Diagnostic log: indicate we're about to inform state machine of timer event
#if DBG_DEVICE_INTERRUPT_TASK
            Serial.println("[DEVICE_INTERRUPT] Two-minute flag set, informing state machine...");
#endif
            TimerInterruptTask::timerInformStatus();
        }
        // Check 1-second (was 5s) flag and inform state machine only when autoread is enabled
        if (TimerInterruptTask::isFiveSecondExpired())
        {
            TimerInterruptTask::resetFiveSecondFlag();
#if DBG_DEVICE_INTERRUPT_TASK
            Serial.println("[DEVICE_INTERRUPT] One-second flag set");
#endif
            // Only notify state machine when autoread is enabled to avoid unnecessary messages
            if (::autoread)
            {
#if DBG_DEVICE_INTERRUPT_TASK
                Serial.println("[DEVICE_INTERRUPT] autoread enabled, informing state machine...");
#endif
                TimerInterruptTask::timerInformStatus5s();
            }
        }
    }
}

void DeviceInterruptTask::checkTouchStatus()
{
    if (!M5.Touch.isEnabled())
    {
        return;
    }

    auto detail = M5.Touch.getDetail();

    // 检查触摸按下
    if (detail.wasPressed())
    {
        // 如果距离上次按下时间小于间隔，则忽略本次按下（防止过快刷新）
        unsigned long now = millis();

        // 构造完整的触摸消息
        SystemMessage_t msg;
        msg.type = MSG_TOUCH_PRESSED;
        if (now - lastTouchPressTime_ < TOUCH_PRESS_GAP_MS)
        {
#if DBG_DEVICE_INTERRUPT_TASK
            Serial.printf("[%lu] [DEVICE_INTERRUPT] DOUBLE CLICK : %lums\n", millis(), now - lastTouchPressTime_);
#endif
            msg.type = MSG_DOUBLE_TOUCH_PRESSED;
            //    return;
        }
        int16_t x = detail.x;
        int16_t y = detail.y;

        touchPressed_ = true;
        lastTouchX_ = x;
        lastTouchY_ = y;
        lastTouchPressTime_ = now;

        msg.timestamp = millis();
        msg.data.touch.x = x;
        msg.data.touch.y = y;
        msg.data.touch.pressed = true;

        // 发送触摸按下消息给状态机
        if (!sendStateMachineMessage(msg))
        {
#if DBG_DEVICE_INTERRUPT_TASK
            Serial.printf("[%lu] [DEVICE_INTERRUPT] 发送触摸按下消息失败\n", millis());
#endif
        }
        else
        {
#if DBG_DEVICE_INTERRUPT_TASK
            Serial.printf("[%lu] [DEVICE_INTERRUPT] 触摸按下 or Double Click: (%d, %d)\n", millis(), x, y);
#endif
        }
    }

    // 检查触摸释放
    if (detail.wasReleased())
    {

        if (touchPressed_)
        {
            touchPressed_ = false;
            // 构造完整的触摸消息
            SystemMessage_t msg;
            msg.type = MSG_TOUCH_RELEASED;
            msg.timestamp = millis();
            msg.data.touch.x = lastTouchX_;
            msg.data.touch.y = lastTouchY_;
            msg.data.touch.pressed = false;

            // 发送触摸释放消息给状态机
            if (!sendStateMachineMessage(msg))
            {
#if DBG_DEVICE_INTERRUPT_TASK
                Serial.printf("[%lu] [DEVICE_INTERRUPT] 发送触摸释放消息失败\n", millis());
#endif
            }
            else
            {
#if DBG_DEVICE_INTERRUPT_TASK
                Serial.printf("[%lu] [DEVICE_INTERRUPT] 触摸释放: (%d, %d)\n", millis(), lastTouchX_, lastTouchY_);
#endif
            }
        }
    }
}

void DeviceInterruptTask::checkBatteryStatus()
{
    // Read power values in a helper task and wait up to 500ms
    const TickType_t POWER_READ_TIMEOUT = pdMS_TO_TICKS(500);
    PowerReadParams* params = (PowerReadParams*)pvPortMalloc(sizeof(PowerReadParams));
    if (params == NULL)
    {
        // allocation failed, fall back to direct read (best-effort)
        float voltage = M5.Power.getBatteryVoltage();
        int percentage = M5.Power.getBatteryLevel();
        bool isCharging = M5.Power.isCharging();
        // existing logic continues with these values
        
        bool batteryChanged = (lastBatteryVoltage_ < 0 || abs(voltage - lastBatteryVoltage_) > 100.0f);
        if (batteryChanged)
        {
            lastBatteryVoltage_ = voltage;
            lastBatteryPercentage_ = percentage;

            // 构造完整的电池消息
            SystemMessage_t msg;
            msg.type = MSG_BATTERY_STATUS_CHANGED;
            msg.timestamp = millis();
            msg.data.power.voltage = voltage;
            msg.data.power.percentage = percentage;
            msg.data.power.isCharging = isCharging;
            msg.data.power.power_connected = isCharging; // 向后兼容
            msg.data.power.battery_level = percentage;   // 向后兼容

            // 发送电池状态变化消息给状态机
            if (!sendStateMachineMessage(msg))
            {
#if DBG_DEVICE_INTERRUPT_TASK
                Serial.printf("[%lu] [DEVICE_INTERRUPT] 发送电池状态消息失败\n", millis());
#endif
            }
            else
            {
#if DBG_DEVICE_INTERRUPT_TASK
                Serial.printf("[%lu] [DEVICE_INTERRUPT] 电池状态变化: %.2fV, %d%%\n", millis(), voltage, percentage);
#endif
            }
        }

        // 检查充电状态变化
        if (lastChargingState_ != isCharging)
        {
            lastChargingState_ = isCharging;

            // 构造完整的充电状态消息
            SystemMessage_t msg;
            msg.type = MSG_CHARGING_STATUS_CHANGED;
            msg.timestamp = millis();
            msg.data.power.voltage = voltage;
            msg.data.power.percentage = percentage;
            msg.data.power.isCharging = isCharging;
            msg.data.power.power_connected = isCharging; // 向后兼容
            msg.data.power.battery_level = percentage;   // 向后兼容

            // 发送充电状态变化消息给状态机
            if (!sendStateMachineMessage(msg))
            {
#if DBG_DEVICE_INTERRUPT_TASK
                Serial.printf("[%lu] [DEVICE_INTERRUPT] 发送充电状态消息失败\n", millis());
#endif
            }
            else
            {
#if DBG_DEVICE_INTERRUPT_TASK
                Serial.printf("[%lu] [DEVICE_INTERRUPT] 充电状态变化: %s\n", millis(), isCharging ? "开始充电" : "停止充电");
#endif
            }
        }

        return;
    }

    // initialize params
    params->notifyTo = xTaskGetCurrentTaskHandle();
    params->done = false;

    // create helper task
    BaseType_t r = xTaskCreatePinnedToCore(
        powerReaderTask,
        "PowerReader",
        2048,
        params,
        PRIO_DEVICE,
        NULL,
        0);

    float voltage = 0.0f;
    int percentage = -1;
    bool isCharging = false;

    if (r == pdPASS)
    {
        // wait for notification or timeout
        if (ulTaskNotifyTake(pdTRUE, POWER_READ_TIMEOUT) > 0)
        {
            // copy results from params
            voltage = params->voltage;
            percentage = params->percentage;
            isCharging = params->isCharging;
        }
        else
        {
            // timeout: helper may still be running; skip update
#if DBG_DEVICE_INTERRUPT_TASK
            Serial.println("[DEVICE_INTERRUPT] Power read timed out (>=500ms), skipping update");
#endif
            // let helper finish and free params; do not access params after this point
            return;
        }
    }
    else
    {
        // task creation failed, fall back to direct read
        voltage = M5.Power.getBatteryVoltage();
        percentage = M5.Power.getBatteryLevel();
        isCharging = M5.Power.isCharging();
    }

    // params will be freed by helper task after a short delay

    // 检查电池电压变化 (变化超过0.1V) 实际上的单位是1mV！
    bool batteryChanged = (lastBatteryVoltage_ < 0 || abs(voltage - lastBatteryVoltage_) > 100.0f);
    if (batteryChanged)
    {
        lastBatteryVoltage_ = voltage;
        lastBatteryPercentage_ = percentage;

        // 构造完整的电池消息
        SystemMessage_t msg;
        msg.type = MSG_BATTERY_STATUS_CHANGED;
        msg.timestamp = millis();
        msg.data.power.voltage = voltage;
        msg.data.power.percentage = percentage;
        msg.data.power.isCharging = isCharging;
        msg.data.power.power_connected = isCharging; // 向后兼容
        msg.data.power.battery_level = percentage;   // 向后兼容

        // 发送电池状态变化消息给状态机
        if (!sendStateMachineMessage(msg))
        {
#if DBG_DEVICE_INTERRUPT_TASK
            Serial.printf("[%lu] [DEVICE_INTERRUPT] 发送电池状态消息失败\n", millis());
#endif
        }
        else
        {
#if DBG_DEVICE_INTERRUPT_TASK
            Serial.printf("[%lu] [DEVICE_INTERRUPT] 电池状态变化: %.2fV, %d%%\n", millis(), voltage, percentage);
#endif
        }
    }

    // 检查充电状态变化
    if (lastChargingState_ != isCharging)
    {
        lastChargingState_ = isCharging;

        // 构造完整的充电状态消息
        SystemMessage_t msg;
        msg.type = MSG_CHARGING_STATUS_CHANGED;
        msg.timestamp = millis();
        msg.data.power.voltage = voltage;
        msg.data.power.percentage = percentage;
        msg.data.power.isCharging = isCharging;
        msg.data.power.power_connected = isCharging; // 向后兼容
        msg.data.power.battery_level = percentage;   // 向后兼容

        // 发送充电状态变化消息给状态机
        if (!sendStateMachineMessage(msg))
        {
#if DBG_DEVICE_INTERRUPT_TASK
            Serial.printf("[%lu] [DEVICE_INTERRUPT] 发送充电状态消息失败\n", millis());
#endif
        }
        else
        {
#if DBG_DEVICE_INTERRUPT_TASK
            Serial.printf("[%lu] [DEVICE_INTERRUPT] 充电状态变化: %s\n", millis(), isCharging ? "开始充电" : "停止充电");
#endif
        }
    }
}

// ============================================================================
// 便捷的全局访问函数实现
// ============================================================================

int DeviceInterruptTask::getLastBatteryPercentage()
{
    return lastBatteryPercentage_;
}


bool initializeDeviceInterrupt()
{
    return DeviceInterruptTask::initialize();
}

void destroyDeviceInterrupt()
{
    DeviceInterruptTask::destroy();
}
