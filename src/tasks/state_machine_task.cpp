#include "state_machine_task.h"
#include "device/powermgt.h"
#include "device/file_manager.h"
#include "test/per_file_debug.h"
#include "readpaper.h"
#include "ui/ui_control.h"
#include "text/book_handle.h"
#include "ui/ui_lock_screen.h"
#include "task_priorities.h"

// 定义全局变量
bool enterDebug = false;

// 静态成员初始化
TaskHandle_t StateMachineTask::taskHandle_ = NULL;
QueueHandle_t StateMachineTask::messageQueue_ = NULL;
SystemState_t StateMachineTask::currentState_ = STATE_IDLE;
unsigned long StateMachineTask::lastActivityTime_ = 0;

// 使用统一的 debug helper
#include "state_debug.h"
#include "ui/show_debug.h"

int StateMachineTask::shutCnt = 0;

bool StateMachineTask::initialize()
{
    // 创建消息队列
    messageQueue_ = xQueueCreate(10, sizeof(SystemMessage_t));
    if (messageQueue_ == NULL)
    {
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("创建消息队列失败\n");
#endif
        return false;
    }

    // 创建任务
    BaseType_t result = xTaskCreatePinnedToCore(
        taskFunction,
        "StateMachineTask",
        16384, // 栈大小 (16KB，从4KB增加)
        NULL,
        PRIO_STATE, // 优先级 (中等)
        &taskHandle_,
        1 //
    );

    if (result != pdPASS)
    {
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("创建任务失败\n");
#endif
        vQueueDelete(messageQueue_);
        messageQueue_ = NULL;
        return false;
    }

    // 初始化状态
    if (enterDebug)
    {
        currentState_ = STATE_DEBUG;
        show_debug(nullptr,true);
    }
    else
    {
        currentState_ = STATE_IDLE;
        // currentState_ = STATE_READING;
        show_lockscreen(PAPER_S3_WIDTH, PAPER_S3_HEIGHT, 30, "双击屏幕解锁");
    }
    lastActivityTime_ = millis();

#if DBG_STATE_MACHINE_TASK
    sm_dbg_printf("状态机任务初始化成功\n");
#endif

    return true;
}

void StateMachineTask::destroy()
{
    if (taskHandle_ != NULL)
    {
        vTaskDelete(taskHandle_);
        taskHandle_ = NULL;
    }

    if (messageQueue_ != NULL)
    {
        vQueueDelete(messageQueue_);
        messageQueue_ = NULL;
    }

#if DBG_STATE_MACHINE_TASK
    Serial.println("[STATE_MACHINE] 状态机任务已销毁");
#endif
}

bool StateMachineTask::sendMessage(const SystemMessage_t &msg)
{
    if (messageQueue_ == NULL)
    {
        return false;
    }

    return xQueueSend(messageQueue_, &msg, pdMS_TO_TICKS(10)) == pdPASS;
}

void StateMachineTask::taskFunction(void *pvParameters)
{
    SystemMessage_t msg;

#if DBG_STATE_MACHINE_TASK
    Serial.println("[STATE_MACHINE] 任务启动");
#endif
    // Initial lock
    //    show_lockscreen(PAPER_S3_WIDTH, PAPER_S3_HEIGHT, 30, "双击屏幕解锁");

    // 执行IDLE状态的初始化工作
    for (;;)
    {
        // 阻塞等待消息，无超时
        if (xQueueReceive(messageQueue_, &msg, portMAX_DELAY) == pdTRUE)
        {
#if DBG_STATE_MACHINE_TASK
            unsigned long t_msg = millis();
            sm_dbg_printf("收到消息类型: %d, 当前状态: %d, ts=%lu\n", msg.type, currentState_, t_msg);
#endif

            // 根据当前状态处理消息，具体实现分散在各 state_*.cpp 文件中
            switch (currentState_)
            {
            case STATE_DEBUG:
                handleDebugState(&msg);
                break;
            case STATE_IDLE:
                handleIdleState(&msg);
                break;
            case STATE_READING:
                handleReadingState(&msg);
                break;
            case STATE_READING_QUICK_MENU:
                handleReadingQuickMenuState(&msg);
                break;
            case STATE_INDEX_DISPLAY:
                handleIndexDisplayState(&msg);
                break;
            case STATE_HELP:
                handleHelpState(&msg);
                break;
            case STATE_WEBDAV:
                handleWebDavState(&msg);
                break;
            case STATE_TOC_DISPLAY:
                handleTocDisplayState(&msg);
                break;
            case STATE_MENU:
                handleMenuState(&msg);
                break;
            case STATE_MAIN_MENU:
                handleMainMenuState(&msg);
                break;
            case STATE_2ND_LEVEL_MENU:
                handle2ndLevelMenuState(&msg);
                break;
            case STATE_WIRE_CONNECT:
                handleWireConnectState(&msg);
                break;
            case STATE_USB_CONNECT:
                handleUsbConnectState(&msg);
                break;
            case STATE_SHUTDOWN:
                handleShutdownState(&msg);
                break;
            case STATE_SHOW_TIME_REC:
                handleShowTimeRecState(&msg);
                break;
            default:
                sm_dbg_printf("未知状态: %d\n", currentState_);
                break;
            }
        }
    }
}

// State handlers moved to separate source files (state_idle.cpp, state_reading.cpp, etc.)

// 全局便捷函数实现
bool initializeStateMachine()
{
    return StateMachineTask::initialize();
}

void destroyStateMachine()
{
    StateMachineTask::destroy();
}

bool sendStateMachineMessage(MessageType_t type, uint32_t timestamp)
{
    SystemMessage_t msg;
    msg.type = type;
    msg.timestamp = timestamp == 0 ? millis() : timestamp;

    bool ok = StateMachineTask::sendMessage(msg);
    if (!ok)
    {
        Serial.printf("[STATE_MACHINE] sendStateMachineMessage(type=%d) failed - queue full or not initialized\n", (int)type);
    }
    else
    {
        // light debug when sending timer messages
        if (type == MSG_TIMER_MIN_TIMEOUT)
        {
            Serial.println("[STATE_MACHINE] sendStateMachineMessage: MSG_TIMER_MIN_TIMEOUT sent");
        }
    }
    return ok;
}

bool sendStateMachineMessage(const SystemMessage_t &message)
{
    bool ok = StateMachineTask::sendMessage(message);
    if (!ok)
    {
        Serial.printf("[STATE_MACHINE] sendStateMachineMessage(message.type=%d) failed\n", (int)message.type);
    }
    else
    {
#if DBG_STATE_MACHINE_TASK
        if (message.type == MSG_TIMER_MIN_TIMEOUT)
        {
            Serial.println("[STATE_MACHINE] sendStateMachineMessage: MSG_TIMER_MIN_TIMEOUT (full message) sent");
        }
#endif
    }
    return ok;
}

SystemState_t getCurrentSystemState()
{
    return StateMachineTask::getCurrentState();
}
