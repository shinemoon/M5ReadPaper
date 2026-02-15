#include "readpaper.h"
#include "state_machine_task.h"
#include "state_debug.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "ui/ui_canvas_utils.h"
#include "ui/ui_lock_screen.h"
#include "ui/trmnl_show.h"
#include "test/per_file_debug.h"
#include "device/wifi_hotspot_manager.h"
#include "current_book.h"
#include "globals.h"
#include "esp_sleep.h"
#include "tasks/display_push_task.h"
#include "tasks/device_interrupt_task.h"
#include "device/powermgt.h"
// for screenshot
#include "ui/screenshot.h"
#define DEBUGWEBDAV 1

extern M5Canvas *g_canvas;
extern float font_size;
// refreshPeriod moved to globals.h/globals.cpp

void StateMachineTask::handleWebDavState(const SystemMessage_t *msg)
{
    // Single-shot draw control (reset when leaving WEBDAV state)
    static bool webdavShown = false;
    static bool sleepIssued = false;

    // if (!g_wifi_sta_connected)
    if (false) // Now there will be always default TRMNL
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
        // Do not timeout in WEBDAV => To protec in case it is stuck in TRMNL
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
            //            bin_font_print(buf, 24, TFT_BLACK, 540, 540, 400, false, g_canvas, TEXT_ALIGN_CENTER);
            //           bin_font_flush_canvas(false, false, true);
        }
        break;

    case MSG_TOUCH_PRESSED:
#if DEBUGWEBDAV
        webdavShown = false;
        show_main_menu(g_canvas, false, 0, 0, false);
        currentState_ = STATE_MAIN_MENU;
#endif
        break;

    case MSG_USER_ACTIVITY:
        break;

    case MSG_DOUBLE_TOUCH_PRESSED:
        break;

    default:
        if (!webdavShown)
        {
            webdavShown = true;
            // 显示 TRMNL 界面（尝试从 WebDAV 读取配置，失败则显示默认）
            trmnl_display(g_canvas);
            // show_default_trmnl(g_canvas);
            bin_font_flush_canvas(false, false, true, RECT);
#if DEBUGWEBDAV
#else
            if (!sleepIssued)
            {
                sleepIssued = true;
                // Ensure display push queue and panel refresh are done before sleep
                waitDisplayPushIdle(2000);
                // Sleep for refreshPeriod minutes then wake to refresh
                int batteryLevel = DeviceInterruptTask::getLastBatteryPercentage(); // Relying on periodly query result

                if (batteryLevel > 15)
                {
                    esp_sleep_enable_timer_wakeup((uint64_t)refreshPeriod * 60ULL * 1000000ULL);
                    esp_deep_sleep_start();
                }
                else
                {
                    show_shutdown_and_sleep();
                }
                return;
            }
#endif
        }
        break;
    }
}
