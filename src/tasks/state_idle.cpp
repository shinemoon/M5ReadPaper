#include "readpaper.h"
#include "state_machine_task.h"
#include "state_debug.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "text/book_handle.h"
#include "test/per_file_debug.h"
#include "ui/ui_lock_screen.h"
#include "ui/ui_control.h"
#include "ui/screenshot.h"
#include <cstring>

#include <esp_heap_caps.h>

#include "current_book.h"
extern float font_size;
#include "globals.h"
#include "text/tags_handle.h"
void StateMachineTask::handleIdleState(const SystemMessage_t *msg)
{
    // IDLE 不需要显示参数
    switch (msg->type)
    {
    case MSG_TIMER_MIN_TIMEOUT:
        // 每分钟触发：在 IDLE 状态记录内存状况（受 DBG_STATE_MACHINE_TASK 控制）
    #if DBG_STATE_MACHINE_TASK
        {
            // 显示内部 DRAM（internal heap）以及 PSRAM 的空闲与总量
            size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            size_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
            size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
            Serial.printf("[STATE_MACHINE] 1MIN IDLE MEM: internal_free=%u internal_total=%u, psram_free=%u psram_total=%u\n",
                          (unsigned)free_internal, (unsigned)total_internal,
                          (unsigned)free_psram, (unsigned)total_psram);
        }
    #endif
        if (++shutCnt == IDLE_PWR_WAIT_MIN)
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("IDLE状态收到5分钟超时信号，准备关机\n");
#endif
            // 如果当前书籍正在建立索引且未完成，则取消这次关机计数，继续保持 IDLE
            if (g_current_book && !g_current_book->isIndexingComplete())
            {
                shutCnt = 0; // 重置计数，给索引更多时间
            }
            else
            {
                shutCnt = 0;
                // automatic tag before shutdown
                if (g_current_book)
                {
                    TextPageResult tp = g_current_book->currentPage();
                    if (tp.success)
                    {
                        insertAutoTagForFile(g_current_book->filePath(), tp.file_pos);
                        g_current_book->refreshTagsCache();
                    }
                }
                currentState_ = STATE_SHUTDOWN;
            }
        }
        /*
        else if (shutCnt == 1)
        {
            // 进入轻休眠
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("IDLE 1分钟进入浅睡眠\n");
#endif

            esp_light_sleep_start();
            delay(50);
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("触摸唤醒\n");
#endif
        }
*/
        break;

    case MSG_USER_ACTIVITY:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("IDLE状态收到用户活动信号\n");
#endif
        lastActivityTime_ = millis();
        break;
    case MSG_TOUCH_PRESSED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("IDLE状态收到触摸按下: (%d, %d)\n", msg->data.touch.x, msg->data.touch.y);
#endif
        lastActivityTime_ = millis();
        break;
        //    case MSG_TOUCH_RELEASED:
    case MSG_DOUBLE_TOUCH_PRESSED: //  双击解锁或截图
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("IDLE状态收到Double Click: (%d, %d)\n", msg->data.touch.x, msg->data.touch.y);
#endif
        lastActivityTime_ = millis();
        // 优先检查是否在截图区域
        if (isInScreenshotArea(msg->data.touch.x, msg->data.touch.y))
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("双击截图区域，开始截图\n");
#endif
            if (screenShot())
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("截图成功\n");
#endif
            }
            break; // 截图后不解锁
        }
        // 不在截图区域，执行解锁
        currentState_ = STATE_READING;
        /*Initial Load book*/
        if (g_current_book == nullptr)
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("没有有效书籍,保持IDLE状态\n");
#endif
            currentState_ = STATE_IDLE;
        }
        else
        {
            // Display current page
            PageTurnResult result = handleReadingTouch(TouchZone::FAKE_CURRENT);
            if (result.success && result.message != nullptr && std::strcmp(result.message, "CURRENTPAGE") == 0)
            {
                g_current_book->renderCurrentPage(font_size,nullptr,true,false,false,false,NOEFFECT);
            }
        }

        break;
    case MSG_TOUCH_EVENT:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("IDLE状态收到触摸事件: (%d, %d)\n", msg->data.touch.x, msg->data.touch.y);
#endif
        lastActivityTime_ = millis();
        break;
    case MSG_DEVICE_ORIENTATION: //仅仅翻转，但不显示
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("IDLE状态收到方向事件: %s\n", DeviceOrientationToString(msg->data.orientation.dir));
#endif
        // 根据方向决定显示旋转：UP -> 2, DOWN -> 0
        if (msg->data.orientation.dir == ORIENT_UP)
        {
            display_set_rotation(2);
        }
        else if (msg->data.orientation.dir == ORIENT_DOWN)
        {
            display_set_rotation(0);
        }
        break;

    case MSG_BATTERY_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("IDLE状态收到电池状态变化: %.2fV, %d%%\n", msg->data.power.voltage, msg->data.power.percentage);
#endif
        if (msg->data.power.percentage < 10)
        {
            Serial.println("[STATE_MACHINE] 电量低警告！");
        }
        break;
    case MSG_CHARGING_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("IDLE状态收到充电状态变化: %s, %.2fV, %d%%\n",
                      msg->data.power.isCharging ? "开始充电" : "停止充电",
                      msg->data.power.voltage, msg->data.power.percentage);
#endif
        break;

    case MSG_POWER_EVENT:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("IDLE状态收到电源事件: 连接=%s, 电量=%d%%\n",
                      msg->data.power.power_connected ? "是" : "否",
                      msg->data.power.battery_level);
#endif
        break;

    default:
#if DBG_STATE_MACHINE_TASK
        Serial.printf("[STATE_MACHINE] IDLE状态收到未知消息: %d\n", msg->type);
#endif
        break;
    }
}
