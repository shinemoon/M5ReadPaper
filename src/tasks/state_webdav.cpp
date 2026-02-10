#include "readpaper.h"
#include "state_machine_task.h"
#include "state_debug.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "ui/ui_canvas_utils.h"
#include "ui/ui_lock_screen.h"
#include "test/per_file_debug.h"
#include "device/wifi_hotspot_manager.h"
#include "current_book.h"
#include "globals.h"
#include "esp_sleep.h"
#include "tasks/display_push_task.h"
// for screenshot
#include "ui/screenshot.h"

extern M5Canvas *g_canvas;
extern float font_size;

void StateMachineTask::handleWebDavState(const SystemMessage_t *msg)
{
    // Single-shot draw control (reset when leaving WEBDAV state)
    static bool webdavShown = false;
    static bool sleepIssued = false;

    if (!g_wifi_sta_connected)
    {
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("WEBDAV状态检测到WiFi断开，返回主菜单\n");
#endif
        if (g_wifi_hotspot)
        {
            g_wifi_hotspot->disconnectWiFi();
        }
        webdavShown = false;
        show_main_menu(g_canvas, false, 0, 0, false);
        currentState_ = STATE_MAIN_MENU;
        return;
    }

    switch (msg->type)
    {
    case MSG_TIMER_MIN_TIMEOUT:
        if (++shutCnt == READING_IDLE_WAIT_MIN)
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("WEBDAV状态收到超时，进入IDLE\n");
#endif
            shutCnt = 0;
            show_lockscreen(PAPER_S3_WIDTH, PAPER_S3_HEIGHT, 30, "双击屏幕解锁");
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
            webdavShown = false;
        }
        break;

    case MSG_BATTERY_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("WEBDAV状态收到电池状态变化: %.2fV, %d%%\n", msg->data.power.voltage, msg->data.power.percentage);
#endif
        if (webdavShown)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "电池: %.2fV %d%%", msg->data.power.voltage, msg->data.power.percentage);
            bin_font_print(buf, 24, TFT_BLACK, 540, 540, 400, false, g_canvas, TEXT_ALIGN_CENTER);
            bin_font_flush_canvas(false, false, true);
        }
        break;

    case MSG_TOUCH_PRESSED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("WEBDAV状态收到触摸, 返回READING: (%d,%d)\n", msg->data.touch.x, msg->data.touch.y);
#endif
        shutCnt = 0;
        lastActivityTime_ = millis();
        webdavShown = false;
        if (g_wifi_hotspot)
        {
            g_wifi_hotspot->disconnectWiFiDeferred();
        }
        currentState_ = STATE_READING;
        if (g_current_book)
        {
            g_current_book->renderCurrentPage(font_size, nullptr, true, false, false, false, HSHUTTER_NORMAL_REV);
        }
        break;

    case MSG_USER_ACTIVITY:
        lastActivityTime_ = millis();
        break;

    case MSG_DOUBLE_TOUCH_PRESSED:
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
        if (!webdavShown)
        {
            webdavShown = true;
            bin_font_clear_canvas();
            bin_font_flush_canvas(false, false, true, HSHUTTER_NORMAL);
            if (!sleepIssued)
            {
                sleepIssued = true;
                if (g_wifi_hotspot)
                {
                    g_wifi_hotspot->disconnectWiFiDeferred(100);
                }
                // Ensure display push queue and panel refresh are done before sleep
                waitDisplayPushIdle(2000);
                // Sleep 10 seconds then wake
                esp_sleep_enable_timer_wakeup(10ULL * 1000000ULL);
                esp_deep_sleep_start();
                return;
            }
        }
        break;
    }
}
