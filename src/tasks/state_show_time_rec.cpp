#include "readpaper.h"
#include "state_machine_task.h"
#include "device/ui_display.h"
#include "ui/ui_lock_screen.h"
#include "test/per_file_debug.h"
#include "current_book.h"
#include "globals.h"
#include "text/tags_handle.h"
#include "ui/ui_time_rec.h"
#include "text/bin_font_print.h"
// for screenshot
#include "ui/screenshot.h"

extern M5Canvas *g_canvas;
extern float font_size;

void StateMachineTask::handleShowTimeRecState(const SystemMessage_t *msg)
{
#if DBG_STATE_MACHINE_TASK
    sm_dbg_printf("SHOW_TIME_REC 状态处理消息: %d\n", msg->type);
#endif
    static bool screen_shown = false;

    // 首次进入时绘制界面
    if (!screen_shown)
    {
        screen_shown = true;
        draw_time_rec_screen(g_canvas);
        bin_font_flush_canvas(false, false, true,VSHUTTER_NORMAL);
    }

    switch (msg->type)
    {
    case MSG_TIMER_MIN_TIMEOUT:
        // 超时计数，达到阈值转到 IDLE
        if (++shutCnt == READING_IDLE_WAIT_MIN)
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("SHOW_TIME_REC 收到超时，进入 IDLE\n");
#endif
            shutCnt = 0;
            screen_shown = false;
            
            // 自动保存书签
            if (g_current_book)
            {
                TextPageResult tp = g_current_book->currentPage();
                if (tp.success)
                {
                    insertAutoTagForFile(g_current_book->filePath(), tp.file_pos);
                    g_current_book->refreshTagsCache();
                }
            }
            
            show_lockscreen(PAPER_S3_WIDTH, PAPER_S3_HEIGHT, 30, "双击屏幕解锁");
            currentState_ = STATE_IDLE;
        }
        break;

    case MSG_TOUCH_PRESSED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("SHOW_TIME_REC 收到触摸 (entry)\n");
#endif
        // 重置计时与活动时间
        shutCnt = 0;
        lastActivityTime_ = millis();
        
        {
            int16_t tx = msg->data.touch.x;
            int16_t ty = msg->data.touch.y;
            
            // 检查是否点击了返回按钮
            if (is_point_in_time_rec_back_button(tx, ty))
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("SHOW_TIME_REC: 点击返回按钮，返回 READING\n");
#endif
                screen_shown = false;
                currentState_ = STATE_READING;
                // 重新绘制阅读页面
                if (g_current_book)
                {
                    g_current_book->renderCurrentPage(font_size);
                }
            }
        }
        break;

    case MSG_USER_ACTIVITY:
        lastActivityTime_ = millis();
        break;

    case MSG_DEVICE_ORIENTATION:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("SHOW_TIME_REC 收到方向事件: %d\n", msg->data.orientation.dir);
#endif
        if (msg->data.orientation.dir == ORIENT_UP)
        {
            display_set_rotation(2);
        }
        else if (msg->data.orientation.dir == ORIENT_DOWN)
        {
            display_set_rotation(0);
        }
        // 重新绘制界面
        draw_time_rec_screen(g_canvas);
        bin_font_flush_canvas(false, false, true);
        break;

    case MSG_BATTERY_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("SHOW_TIME_REC 电池变化: %.2fV %d%%\n", msg->data.power.voltage, msg->data.power.percentage);
#endif
        break;

    case MSG_CHARGING_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("SHOW_TIME_REC 充电状态变化: %d\n", msg->data.power.isCharging);
#endif
        break;

    case MSG_DOUBLE_TOUCH_PRESSED:
        // 检查是否在截图区域
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
        }
        break;

    default:
#if DBG_STATE_MACHINE_TASK
        Serial.printf("[STATE_MACHINE] SHOW_TIME_REC 收到未知消息: %d\n", msg->type);
#endif
        break;
    }
}
