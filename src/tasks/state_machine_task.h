#pragma once

#include <M5Unified.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "ui/ui_canvas_utils.h"

// 状态机状态定义
typedef enum {
    STATE_IDLE = 0,
    STATE_DEBUG,
    STATE_READING,
    STATE_READING_QUICK_MENU,
    STATE_HELP,
    STATE_INDEX_DISPLAY,
    STATE_TOC_DISPLAY,
    STATE_MENU,
    STATE_MAIN_MENU,
    STATE_2ND_LEVEL_MENU,
    STATE_WIRE_CONNECT,
    STATE_USB_CONNECT,
    STATE_SHUTDOWN,
    STATE_SHOW_TIME_REC
} SystemState_t;

// 二级菜单类型枚举（用于 main 菜单触发的二级菜单场景）
typedef enum {
    MAIN_2ND_MENU_CLEAN_BOOKMARK = 0,
    MAIN_2ND_MENU_DISPLAY_SETTING = 1,
    MAIN_2ND_MENU_FONT_SETTING = 2,
    MAIN_2ND_MENU_CONNECT_METHOD = 3,
    // future types can be added here
} Main2ndLevelMenuType;

// 当前二级菜单类型（全局可见）
extern Main2ndLevelMenuType main_2nd_level_menu_type;

// 任务间通信消息类型
typedef enum {
    MSG_TIMER_MIN_TIMEOUT = 0,
    MSG_TIMER_5S_TIMEOUT,
    MSG_USER_ACTIVITY,
    MSG_TOUCH_PRESSED,              // 触摸按下
    MSG_TOUCH_RELEASED,             // 触摸释放
    MSG_TOUCH_EVENT,                // 通用触摸事件 (向后兼容)
    MSG_DOUBLE_TOUCH_PRESSED,              // 触摸按下
    MSG_BATTERY_STATUS_CHANGED,     // 电池状态变化
    MSG_CHARGING_STATUS_CHANGED,    // 充电状态变化
    MSG_POWER_EVENT                 // 通用电源事件 (向后兼容)
    ,MSG_DEVICE_ORIENTATION         // 设备方向事件（UP/DOWN/LEFT/RIGHT）
} MessageType_t;

typedef struct {
    MessageType_t type;
    uint32_t timestamp;
    union {
        struct {
            int16_t x, y;
            bool pressed;           // true=按下, false=释放
        } touch;
        struct {
            float voltage;          // 电池电压 (V)
            int percentage;         // 电池电量百分比
            bool isCharging;        // 是否正在充电
            bool power_connected;   // 电源连接状态 (向后兼容)
            int battery_level;      // 电池等级 (向后兼容)
        } power;
        struct {
            uint32_t reserved;      // 预留给其他事件类型
        } generic;
        struct {
            uint8_t dir;         // DeviceOrientation (globals.h)
        } orientation;
    } data;
} SystemMessage_t;

// 状态机处理任务类
class StateMachineTask {
private:
    static TaskHandle_t taskHandle_;
    static QueueHandle_t messageQueue_;
    static SystemState_t currentState_;
    static unsigned long lastActivityTime_;
    static int shutCnt;
    
    // 状态处理函数
    static void handleIdleState(const SystemMessage_t* msg);
    static void handleReadingState(const SystemMessage_t* msg);
    static void handleDebugState(const SystemMessage_t* msg);
    static void handleReadingQuickMenuState(const SystemMessage_t* msg);
    static void handleHelpState(const SystemMessage_t* msg);
    static void handleIndexDisplayState(const SystemMessage_t* msg);
    static void handleTocDisplayState(const SystemMessage_t* msg);
    static void handleMenuState(const SystemMessage_t* msg);
    static void handleMainMenuState(const SystemMessage_t* msg);
    static void handle2ndLevelMenuState(const SystemMessage_t* msg);
    static void handleWireConnectState(const SystemMessage_t* msg);
    static void handleUsbConnectState(const SystemMessage_t* msg);
    static void handleShutdownState(const SystemMessage_t* msg);
    static void handleShowTimeRecState(const SystemMessage_t* msg);
    
    // 内部任务函数
    static void taskFunction(void* pvParameters);
    
public:
    static bool initialize();
    static void destroy();
    static bool sendMessage(const SystemMessage_t& msg);
    static SystemState_t getCurrentState() { return currentState_; }
    static TaskHandle_t getTaskHandle() { return taskHandle_; }
    static QueueHandle_t getMessageQueue() { return messageQueue_; }
};

// 便捷的全局访问函数
bool initializeStateMachine();
void destroyStateMachine();
bool sendStateMachineMessage(MessageType_t type, uint32_t timestamp = 0);
bool sendStateMachineMessage(const SystemMessage_t& message);  // 发送完整消息
SystemState_t getCurrentSystemState();
