#include "readpaper.h"
#include "state_machine_task.h"
#include "state_debug.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "ui/ui_canvas_utils.h"
#include "ui/ui_lock_screen.h"
#include "ui/ui_canvas_image.h"
#include "test/per_file_debug.h"
#include <cstring>
// for screenshot
#include "ui/screenshot.h"

#include "current_book.h"
extern M5Canvas *g_canvas;
extern float font_size;
#include "globals.h"

void StateMachineTask::handleHelpState(const SystemMessage_t *msg)
{
    // 单次显示控制（在再次进入 STATE_HELP 时会重新评估并显示）
    static bool helpShown = false;

    switch (msg->type)
    {
    case MSG_TIMER_MIN_TIMEOUT:
        if (++shutCnt == READING_IDLE_WAIT_MIN)
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("HELP状态收到超时，进入IDLE\n");
#endif
            shutCnt = 0;
            show_lockscreen(PAPER_S3_WIDTH, PAPER_S3_HEIGHT, 30, "双击屏幕解锁");
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
            currentState_ = STATE_IDLE;
            helpShown = false; // reset for next time
        }
        break;

    case MSG_BATTERY_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("HELP状态收到电池状态变化: %.2fV, %d%%\n", msg->data.power.voltage, msg->data.power.percentage);
#endif
        // 当收到电池状态变化时，如果帮助页面已显示，更新屏幕上的电量文本
        if (helpShown)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "电池: %.2fV %d%%", msg->data.power.voltage, msg->data.power.percentage);
            bin_font_print(buf, 24, TFT_BLACK, 540, 540, 400, false, g_canvas, TEXT_ALIGN_CENTER);
            bin_font_flush_canvas(false, false, true);
        }
        break;

    case MSG_TOUCH_PRESSED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("HELP状态收到触摸, 返回READING: (%d,%d)\n", msg->data.touch.x, msg->data.touch.y);
#endif
        shutCnt = 0;
        lastActivityTime_ = millis();
        helpShown = false;
        currentState_ = STATE_READING;
        // 返回阅读时刷新当前页
        if (g_current_book)
        {
            g_current_book->renderCurrentPage(font_size);
        }
        break;

    case MSG_USER_ACTIVITY:
        lastActivityTime_ = millis();
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
        // 仅在首次进入或尚未绘制帮助页时绘制一次
        if (!helpShown)
        {
            helpShown = true;
                // 首次进入显示帮助图片（来自 /data/guide.png）
                // 使用 canvas 推图片并立即刷新显示
                bin_font_clear_canvas();
                ui_push_image_to_canvas("/spiffs/guide.png", 0, 0, nullptr, true);
                bin_font_print(ver,28,0,PAPER_S3_WIDTH,0,PAPER_S3_HEIGHT/2+40,false,nullptr,TEXT_ALIGN_CENTER);
                bin_font_flush_canvas(false, false, true, HSHUTTER_NORMAL);
        }
        break;
    }
}
