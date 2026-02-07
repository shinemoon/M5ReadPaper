#include "readpaper.h"
#include "state_machine_task.h"
#include "state_debug.h"
#include "device/ui_display.h"
#include "ui/ui_lock_screen.h"
#include "test/per_file_debug.h"
// for screenshot
#include "ui/screenshot.h"

#include "current_book.h"
extern M5Canvas *g_canvas;
extern float font_size;
#include "globals.h"
#include "text/tags_handle.h"
#include <cstring>
#include "ui/ui_reading_quick_menu.h"
#include "../config/config_manager.h"

extern GlobalConfig g_config;

void StateMachineTask::handleReadingQuickMenuState(const SystemMessage_t *msg)
{
#if DBG_STATE_MACHINE_TASK
    sm_dbg_printf("READING_QUICK_MENU 状态处理消息: %d\n", msg->type);
#endif
    static bool quickMenuShown = false;

    // 首次进入时绘制一次初始界面
    if (!quickMenuShown)
    {
        quickMenuShown = true;
        draw_reading_quick_menu(g_canvas);
        bin_font_flush_canvas(false, false, false);
    }
    switch (msg->type)
    {
    case MSG_TIMER_MIN_TIMEOUT:
        // 超时计数，达到阈值转到 IDLE
        if (++shutCnt == READING_IDLE_WAIT_MIN)
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("READING_QUICK_MENU 收到超时，进入 IDLE\n");
#endif
            shutCnt = 0;
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
        sm_dbg_printf("READING_QUICK_MENU 收到触摸 (entry)\n");
#endif
        // 仅作为进入/活动记录：重置计时与活动时间
        shutCnt = 0;
        lastActivityTime_ = millis();
        // 点击事件：如果在快速菜单矩形之外则直接返回 READING
        {
            int16_t tx = msg->data.touch.x;
            int16_t ty = msg->data.touch.y;
            if (!is_point_in_reading_quick_menu(tx, ty))
            {
                if (ty >= 100) // For Screening
                {
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("READING_QUICK_MENU: 点击在矩形之外，返回 READING\n");
#endif
                    quickMenuShown = false;
                    currentState_ = STATE_READING;
                    if (g_current_book)
                    {
                        g_current_book->renderCurrentPage(font_size);
                    }
                }
            }
            else
            {
                // 点击在快速菜单内部：优先处理 autospeed 区域 (y in 890..960, x in 50..452)
                if (ty >= 890 && ty <= 960 && tx >= 50 && tx <= 452)
                {
                    uint8_t newSpeed = ::autospeed;
                    if (tx >= 52 && tx <= 150)
                        newSpeed = 0;
                    else if (tx >= 152 && tx <= 250)
                        newSpeed = 1;
                    else if (tx >= 252 && tx <= 350)
                        newSpeed = 2;
                    else if (tx >= 352 && tx <= 450)
                        newSpeed = 3;

                    if (newSpeed != ::autospeed)
                    {
                        ::autospeed = newSpeed;
                        g_config.autospeed = ::autospeed;
                        // persist to storage
                        config_save();
#if DBG_STATE_MACHINE_TASK
                        sm_dbg_printf("READING_QUICK_MENU: 设置 autospeed -> %u\n", ::autospeed);
#endif
                        // 重新绘制快速菜单以反馈变化
                        draw_reading_quick_menu(g_canvas);
                        bin_font_flush_canvas(false, false, true);
                    }
                }
                else if (tx > 460 && ty > 880) // 点击在快速菜单内部：当坐标 x>460 && y>880 时切换 autoread
                {
                    autoread = !autoread;
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("READING_QUICK_MENU: 切换 autoread -> %d\n", autoread);
#endif
                    // 重新绘制快速菜单并显示状态提示
                    draw_reading_quick_menu(g_canvas);
                    bin_font_flush_canvas(false, false, true);
                }
                else if (tx > 249 && tx < 460 && ty > 780 && ty < 860)
                { // 点击在快速菜单内部：手动全刷
                    //bin_font_flush_canvas(false, false, true);
                    quickMenuShown = false;
                    currentState_ = STATE_READING;
                    if (g_current_book)
                    {
                        g_current_book->renderCurrentPage(font_size,nullptr,true,false,false,4,display_type::RANDOM);
                    }
                }
                else
                {
                    quickMenuShown = false;
                    currentState_ = STATE_READING;
                    if (g_current_book)
                    {
                        g_current_book->renderCurrentPage(font_size);
                    }
                }
            }
        }
        break;

    case MSG_USER_ACTIVITY:
        lastActivityTime_ = millis();
        break;

    case MSG_DEVICE_ORIENTATION:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("READING_QUICK_MENU 收到方向事件: %d\n", msg->data.orientation.dir);
#endif
        if (msg->data.orientation.dir == ORIENT_UP)
        {
            display_set_rotation(2);
        }
        else if (msg->data.orientation.dir == ORIENT_DOWN)
        {
            display_set_rotation(0);
        }
        // 切换方向后，若处于阅读相关页面可刷新当前页
        if (g_current_book)
        {
            g_current_book->renderCurrentPage(font_size);
            // 重新绘制快速菜单以适配旋转
            if (quickMenuShown)
            {
                draw_reading_quick_menu(g_canvas);
                bin_font_flush_canvas(false, false, false);
            }
        }
        break;

    case MSG_BATTERY_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("READING_QUICK_MENU 电池变化: %.2fV %d%%\n", msg->data.power.voltage, msg->data.power.percentage);
#endif
        // 可在此处更新快速菜单上的电池显示（目前仅刷新日志）
        break;

    case MSG_CHARGING_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("READING_QUICK_MENU 充电状态变化: %d\n", msg->data.power.isCharging);
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
        Serial.printf("[STATE_MACHINE] READING_QUICK_MENU 收到未知消息: %d\n", msg->type);
#endif
        break;
    }
}
