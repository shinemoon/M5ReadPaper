#pragma once

#include <M5Unified.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "tasks/state_machine_task.h"

// 设备中断类型
typedef enum {
    DEVICE_INT_TOUCH_CLICK,     // 触摸屏点击
    DEVICE_INT_BATTERY_CHANGE,  // 电池状态变化
    DEVICE_INT_CHARGING_CHANGE  // 充电状态变化
    ,DEVICE_INT_IMU_MOTION       // 陀螺仪/IMU 运动中断
} DeviceInterruptType_t;

// 设备中断数据结构
typedef struct {
    DeviceInterruptType_t type;
    uint32_t timestamp;
    union {
        struct {
            int16_t x;
            int16_t y;
            bool pressed;
        } touch;
        struct {
            float voltage;
            int percentage;
            bool isCharging;
        } battery;
        struct {
            float ax, ay, az;
            float gx, gy, gz;
        } imu;
    } data;
} DeviceInterrupt_t;

// 设备中断处理任务类 - 简化的定时轮询架构
class DeviceInterruptTask {
private:
    static TaskHandle_t taskHandle_;
    static volatile bool touchPressed_;
    static volatile int16_t lastTouchX_;
    static volatile int16_t lastTouchY_;
    static volatile float lastBatteryVoltage_;
    static volatile int lastBatteryPercentage_;
    static volatile bool lastChargingState_;
    static volatile unsigned long lastTouchPressTime_;
    // 上次发送的方向（用于去抖/抑制重复发送）
    static volatile int lastOrientation_;
    // 去抖候选方向和计数：需要连续 N 次相同判定才真正发送
    static volatile int lastOrientationCandidate_;
    static volatile int orientationStreak_;
    // 候选方向首次被看到的时间（ms），用于确认稳定持续时间
    static volatile unsigned long lastOrientationCandidateFirstSeen_;
    // 上次成功发送方向的时间（ms），用于最小间隔保护
    static volatile unsigned long lastOrientationSentTime_;
    
    // 内部任务函数
    static void taskFunction(void* pvParameters);
    
    // 设备状态检查函数
    static void checkTouchStatus();
    static void checkBatteryStatus();
    
public:
    static bool initialize();
    static void destroy();
    static TaskHandle_t getTaskHandle() { return taskHandle_; }
    static int getLastBatteryPercentage();
};

// 便捷的全局访问函数
bool initializeDeviceInterrupt();
void destroyDeviceInterrupt();
