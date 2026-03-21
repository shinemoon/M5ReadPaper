#include "readpaper.h"
#include "state_machine_task.h"
#include "state_debug.h"
#include "device/ui_display.h"
#include "ui/ui_lock_screen.h"
#include "device/wifi_hotspot_manager.h"
#include "ui/ui_canvas_image.h"
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
#include "tasks/background_index_task.h"
#include "text/bin_font_print.h"

extern GlobalConfig g_config;

namespace
{
constexpr int16_t QUICK_AUTOSPEED_X = 49;
constexpr int16_t QUICK_AUTOSPEED_Y = 889;
constexpr int16_t QUICK_AUTOSPEED_W = 492;
constexpr int16_t QUICK_AUTOSPEED_H = 62;

// 字体比例条 + 预览字符 联合局部刷新区域（覆盖scale bar正上方72px预览区 + 整条scale bar）
constexpr int16_t QUICK_SCALE_VALUE_X = 0;
constexpr int16_t QUICK_SCALE_VALUE_Y = 552;
constexpr int16_t QUICK_SCALE_VALUE_W = PAPER_S3_WIDTH;
constexpr int16_t QUICK_SCALE_VALUE_H = 135; // 72px (预览区) + 63px (scale bar)
}

void StateMachineTask::handleReadingQuickMenuState(const SystemMessage_t *msg)
{
#if DBG_STATE_MACHINE_TASK
    sm_dbg_printf("READING_QUICK_MENU 状态处理消息: %d\n", msg->type);
#endif
    static bool quickMenuShown = false;
    static uint8_t pending_font_scale_pct = FONT_SCALE_DEFAULT_PCT;
    static bool pending_font_scale_dirty = false;

    auto apply_pending_font_scale_if_needed = [&]() {
        if (!pending_font_scale_dirty)
            return;

        uint8_t new_scale = clamp_font_scale_pct((int)pending_font_scale_pct);
        if (g_config.font_scale_pct != new_scale)
        {
            g_config.font_scale_pct = new_scale;
            extern float font_size;
            font_size = get_configured_reading_font_size(get_font_size_from_file());
            if (g_current_book)
            {
                g_current_book->setFontSize(font_size);
                // 同步更新 area_w：根据当前字号动态设置左右对称边距，确保分页与渲染一致
                g_current_book->setAreaWidth(PAPER_S3_WIDTH - get_reading_effective_margin_left() - get_reading_effective_margin_right());
            }
            config_save();
            if (g_current_book)
            {
                requestForceReindex();
            }
        }
        pending_font_scale_dirty = false;
    };

    // 首次进入时绘制一次初始界面
    if (!quickMenuShown)
    {
        quickMenuShown = true;
        pending_font_scale_pct = clamp_font_scale_pct((int)g_config.font_scale_pct);
        pending_font_scale_dirty = false;
        draw_reading_quick_menu(g_canvas, pending_font_scale_pct, pending_font_scale_dirty);
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
            apply_pending_font_scale_if_needed();
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
            StateMachineTask::activateLockScreen();
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
                    apply_pending_font_scale_if_needed();
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
                        draw_reading_quick_menu(g_canvas, pending_font_scale_pct, pending_font_scale_dirty);
                        bin_font_flush_canvas(false, false, false, NOEFFECT,
                                              QUICK_AUTOSPEED_X, QUICK_AUTOSPEED_Y,
                                              QUICK_AUTOSPEED_W, QUICK_AUTOSPEED_H);
                    }
                }
                else if (ty >= 628 && ty <= 682 && tx >= 51 && tx <= 551)
                {
                    const uint8_t scale_values[15] = {80, 85, 90, 95, 100, 105, 110, 115, 120, 125, 130, 135, 140, 145, 150};
                    int current_idx = 4;
                    for (int i = 0; i < 15; ++i)
                    {
                        if (pending_font_scale_pct == scale_values[i])
                        {
                            current_idx = i;
                            break;
                        }
                    }

                    int target_idx = current_idx;
                    // 500宽字体比例条: 标题190 | 减号70 | 当前值110 | 加号70 | 螺丝60
                    // 实际可点击区域主要是减号和加号两段
                    if (tx >= 243 && tx < 313)
                    {
                        target_idx = current_idx > 0 ? current_idx - 1 : current_idx;
                    }
                    else if (tx >= 423 && tx < 493)
                    {
                        target_idx = current_idx < 14 ? current_idx + 1 : current_idx;
                    }

                    uint8_t new_scale = scale_values[target_idx];
                    if (pending_font_scale_pct != new_scale)
                    {
                        pending_font_scale_pct = new_scale;
                        pending_font_scale_dirty = true;
                        draw_reading_quick_menu(g_canvas, pending_font_scale_pct, pending_font_scale_dirty);
                        bin_font_flush_canvas(false, false, false, VSHUTTER_REV,
                                              QUICK_SCALE_VALUE_X, QUICK_SCALE_VALUE_Y,
                                              QUICK_SCALE_VALUE_W, QUICK_SCALE_VALUE_H);
                    }
                }
                else if (tx > 460 && ty > 880) // 点击在快速菜单内部：当坐标 x>460 && y>880 时切换 autoread
                {
                    autoread = !autoread;
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("READING_QUICK_MENU: 切换 autoread -> %d\n", autoread);
#endif
                    // 重新绘制快速菜单并显示状态提示
                    draw_reading_quick_menu(g_canvas, pending_font_scale_pct, pending_font_scale_dirty);
                    bin_font_flush_canvas(false, false, true);
                }
                else if (tx > 249 && tx < 460 && ty > 700 && ty < 775)
                { // 点击在快速菜单内部：联线待机
                    apply_pending_font_scale_if_needed();
                    quickMenuShown = false;
                    ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                    M5.Display.waitDisplay();
                    wifi_hotspot_init();
                    if (g_wifi_hotspot)
                    {
                        bool connected = g_wifi_hotspot->connectToWiFiFromToken();
                        if (!connected)
                            g_wifi_hotspot->disconnectWiFi();
                    }
                    currentState_ = STATE_WEBDAV;
                }
                else if (tx > 249 && tx < 460 && ty > 780 && ty < 860)
                { // 点击在快速菜单内部：手动全刷
                    //bin_font_flush_canvas(false, false, true);
                    apply_pending_font_scale_if_needed();
                    quickMenuShown = false;
                    currentState_ = STATE_READING;
                    if (g_current_book)
                    {
                        g_current_book->renderCurrentPage(font_size,nullptr,true,false,false,4,display_type::RANDOM);
                    }
                }
                else
                {
                    apply_pending_font_scale_if_needed();
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
                draw_reading_quick_menu(g_canvas, pending_font_scale_pct, pending_font_scale_dirty);
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
