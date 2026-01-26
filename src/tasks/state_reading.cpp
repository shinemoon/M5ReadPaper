#include <M5Unified.h>
#include "readpaper.h"
#include "state_machine_task.h"
#include "state_debug.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "text/book_handle.h"
#include "ui/ui_control.h"
#include "ui/ui_canvas_utils.h"
#include "ui/ui_canvas_image.h"
#include "ui/ui_lock_screen.h"
#include "ui/index_display.h"
#include "ui/toc_display.h"
#include "ui/screenshot.h"
#include "test/per_file_debug.h"
#include <cstring>

#include "current_book.h"
extern M5Canvas *g_canvas;
extern GlobalConfig g_config;
extern float font_size;
#include "globals.h"
#include "text/tags_handle.h"

#include <esp_heap_caps.h>

void StateMachineTask::handleReadingState(const SystemMessage_t *msg)
{
        // 持久化的 1s 计数器，用于根据 g_config.autospeed 做自动翻页阈值计算
        static uint32_t s_one_sec_ticks = 0;
        switch (msg->type)
        {
        case MSG_TIMER_MIN_TIMEOUT: // 20min go to shut
                // 每分钟触发：记录内存状况，并增加阅读时间计数（但不立即保存书签）
#if DBG_STATE_MACHINE_TASK
                                {
                                        // 显示内部 DRAM（internal heap）以及 PSRAM 的空闲与总量
                                        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                                        size_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
                                        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                                        size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
                                        Serial.printf("[STATE_MACHINE] 1MIN READING MEM: internal_free=%u internal_total=%u, psram_free=%u psram_total=%u\n",
                                                                  (unsigned)free_internal, (unsigned)total_internal,
                                                                  (unsigned)free_psram, (unsigned)total_psram);
                                }
#endif
                if (g_current_book)
                {
                        g_current_book->incrementReadingMinute();
                }

                if (++shutCnt == READING_IDLE_WAIT_MIN)
                {
#if DBG_STATE_MACHINE_TASK
                        sm_dbg_printf("IDLE状态收到5分钟超时信号，j进入IDLE\n");
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
                }
                break;

        case MSG_TIMER_5S_TIMEOUT:
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("READING 状态收到 1s 定时事件\n");
#endif
                // 该定时器每秒触发一次（已在 timer module 调整），
                // 根据 g_config.autospeed 决定实际翻页间隔：
                // autospeed == 0 -> 12s
                // autospeed == 1 -> 8s
                // autospeed == 2 -> 6s
                // autospeed == 3 -> 4s
                lastActivityTime_ = millis();

                // 仅在 autoread 开启下才增加计数并可能触发翻页（避免无谓计数）
                if (autoread)
                {
                        s_one_sec_ticks++;

                        uint8_t aspeed = g_config.autospeed;
                        uint32_t per_char_ms = 20; // default for fastest gear
                        switch (aspeed)
                        {
                        case 0:
                                per_char_ms = 100;
                                break;
                        case 1:
                                per_char_ms = 80;
                                break;
                        case 2:
                                per_char_ms = 60;
                                break;
                        default:
                                per_char_ms = 40;
                                break;
                        }

                        uint32_t threshold = 6; // fallback seconds
                        if (g_current_book)
                        {
                                size_t char_count = g_current_book->getCurrentPageCharCount();
                                if (char_count > 0)
                                {
                                        uint64_t total_ms = static_cast<uint64_t>(char_count) * per_char_ms;
                                        threshold = static_cast<uint32_t>((total_ms + 999) / 1000); // ceil to seconds
                                        if (threshold == 0)
                                                threshold = 1;
                                }
                        }

                        if (s_one_sec_ticks >= threshold)
                        {
                                s_one_sec_ticks = 0;
                                if (g_current_book)
                                {
                                        TextPageResult tp = g_current_book->nextPage();
                                        if (tp.success)
                                        {
                                                g_current_book->renderCurrentPage(font_size);
                                                g_current_book->saveBookmark(); // render后保存书签
                                        }
                                }
                        }
                }
                else
                {
                        // 如果未开启 autoread，则重置计数
                        s_one_sec_ticks = 0;
                }
                break;

        case MSG_TOUCH_PRESSED:
        {
                shutCnt = 0;
#if DBG_STATE_MACHINE_TASK
                unsigned long touch_start = millis();
                sm_dbg_printf("READING状态收到触摸: (%d, %d) at %lu ms\n",
                              msg->data.touch.x, msg->data.touch.y, touch_start);
#endif
                lastActivityTime_ = millis();

                // High-priority: if touch is inside the top-right 25x25 area, toggle tag for current page
                int16_t tx = msg->data.touch.x;
                int16_t ty = msg->data.touch.y;
                const int16_t corner_w = 80;
                const int16_t corner_h = 80;
                if (tx >= PAPER_S3_WIDTH - corner_w && tx < PAPER_S3_WIDTH && ty >= 0 && ty < corner_h)
                {
                        if (g_current_book != nullptr)
                        {
                                TextPageResult tp = g_current_book->currentPage();
                                if (tp.success)
                                {
                                        size_t page_start = tp.file_pos;
                                        size_t page_end = SIZE_MAX;
                                        
                                        // 计算页面结束位置
                                        if (g_current_book->isPagesLoaded() && g_current_book->getTotalPages() > 0)
                                        {
                                                size_t cur_idx = g_current_book->getCurrentPageIndex();
                                                if (cur_idx + 1 < g_current_book->getTotalPages())
                                                {
                                                        // 获取下一页的开始位置（即当前页的结束位置）
                                                        // 使用nextPage会改变当前页，所以直接计算
                                                        // 假设页面是连续的，下一页开始位置就是当前页的file_pos + page_text.size()
                                                        // 但更准确的方法是使用文件大小或getPagePositions
                                                        // 这里先尝试翻到下一页获取位置，然后翻回来
                                                        size_t saved_idx = cur_idx;
                                                        TextPageResult next = g_current_book->nextPage();
                                                        if (next.success)
                                                        {
                                                                page_end = next.file_pos;
                                                                // 翻回原页面
                                                                g_current_book->jumpToPage(saved_idx);
                                                        }
                                                }
                                                else
                                                {
                                                        // 最后一页，使用文件大小
                                                        page_end = g_current_book->getFileSize();
                                                }
                                        }
                                        
                                        // 查找当前页面范围内的第一个manual tag（不包括auto tag）
                                        size_t tag_to_delete = SIZE_MAX;
                                        bool found = false;
                                        for (const TagEntry &te : g_current_book->getCachedTags())
                                        {
                                                // 只删除manual tags，不删除auto tag
                                                if (!te.is_auto && te.position >= page_start && te.position < page_end)
                                                {
                                                        tag_to_delete = te.position;
                                                        found = true;
                                                        break;
                                                }
                                        }

                                        if (found)
                                        {
                                                // 删除找到的tag
                                                if (deleteTagForFileByPosition(g_current_book->filePath(), tag_to_delete))
                                                {
                                                        g_current_book->refreshTagsCache();
                                                        g_current_book->renderCurrentPage(font_size,nullptr,true,false,false,true);
                                                }
                                        }
                                        else
                                        {
                                                // 当前页没有manual tag，创建新的tag
                                                // compute preview from in-memory page text (same rules as menu)
                                                auto make_preview_from_utf8 = [](const std::string &s) -> std::string
                                                {
                                                        std::string out;
                                                        out.reserve(64);
                                                        size_t len = s.size();
                                                        size_t idx = 0;

                                                        auto next_cp = [&](size_t p, uint32_t &cp, size_t &bytes) -> bool
                                                        {
                                                                if (p >= len)
                                                                        return false;
                                                                unsigned char b0 = (unsigned char)s[p];
                                                                if ((b0 & 0x80) == 0)
                                                                {
                                                                        cp = b0;
                                                                        bytes = 1;
                                                                        return true;
                                                                }
                                                                if ((b0 & 0xE0) == 0xC0)
                                                                {
                                                                        if (p + 1 >= len)
                                                                                return false;
                                                                        cp = ((b0 & 0x1F) << 6) | ((unsigned char)s[p + 1] & 0x3F);
                                                                        bytes = 2;
                                                                        return true;
                                                                }
                                                                if ((b0 & 0xF0) == 0xE0)
                                                                {
                                                                        if (p + 2 >= len)
                                                                                return false;
                                                                        cp = ((b0 & 0x0F) << 12) | (((unsigned char)s[p + 1] & 0x3F) << 6) | ((unsigned char)s[p + 2] & 0x3F);
                                                                        bytes = 3;
                                                                        return true;
                                                                }
                                                                if ((b0 & 0xF8) == 0xF0)
                                                                {
                                                                        if (p + 3 >= len)
                                                                                return false;
                                                                        cp = ((b0 & 0x07) << 18) | (((unsigned char)s[p + 1] & 0x3F) << 12) | (((unsigned char)s[p + 2] & 0x3F) << 6) | ((unsigned char)s[p + 3] & 0x3F);
                                                                        bytes = 4;
                                                                        return true;
                                                                }
                                                                cp = 0;
                                                                bytes = 1;
                                                                return true;
                                                        };

                                                        auto is_space = [](uint32_t cp) -> bool
                                                        {
                                                                if (cp <= 0x000D)
                                                                        return (cp >= 0x0009 && cp <= 0x000D);
                                                                if (cp == 0x0020)
                                                                        return true;
                                                                if (cp == 0x00A0)
                                                                        return true;
                                                                if (cp >= 0x2000 && cp <= 0x200A)
                                                                        return true;
                                                                if (cp == 0x2028 || cp == 0x2029)
                                                                        return true;
                                                                if (cp == 0x202F || cp == 0x205F)
                                                                        return true;
                                                                if (cp == 0x3000)
                                                                        return true;
                                                                return false;
                                                        };

                                                        auto is_linebreak = [](uint32_t cp) -> bool
                                                        {
                                                                if (cp == 0x000A || cp == 0x000D)
                                                                        return true;
                                                                if (cp == 0x2028 || cp == 0x2029)
                                                                        return true;
                                                                return false;
                                                        };

                                                        // skip leading whitespace (includes newlines)
                                                        while (idx < len)
                                                        {
                                                                uint32_t cp = 0;
                                                                size_t bytes = 0;
                                                                if (!next_cp(idx, cp, bytes))
                                                                        break;
                                                                if (is_space(cp))
                                                                {
                                                                        idx += bytes;
                                                                        continue;
                                                                }
                                                                break;
                                                        }

                                                        // collect up to 10 codepoints, skipping line breaks (do not count them)
                                                        size_t collected = 0;
                                                        size_t pos = idx;
                                                        while (pos < len && collected < 10)
                                                        {
                                                                uint32_t cp = 0;
                                                                size_t bytes = 0;
                                                                if (!next_cp(pos, cp, bytes))
                                                                        break;
                                                                if (is_linebreak(cp))
                                                                {
                                                                        pos += bytes;
                                                                        continue;
                                                                }
                                                                size_t start = pos;
                                                                out.append(s.data() + start, bytes);
                                                                pos = start + bytes;
                                                                ++collected;
                                                        }
                                                        while (!out.empty() && ((unsigned char)out.back() < 0x20 || out.back() == '\0'))
                                                                out.pop_back();
                                                        return out;
                                                };

                                                std::string preview = make_preview_from_utf8(tp.page_text);
                                                // 创建新书签时使用页面开头位置
                                                if (insertTagForFile(g_current_book->filePath(), page_start, preview))
                                                {
                                                        g_current_book->refreshTagsCache();
                                                        g_current_book->renderCurrentPage(font_size,nullptr,true,false,false,true);
                                                }
                                        }
                                }
                        }
                        // toggling tag takes priority over other touch handling
                        return;
                }
                else if (tx <= corner_w && ty >= 0 && ty < corner_h)
                {
                        ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                        // 如果当前文本有toc，则显示toc，否则显示tags
                                if (g_current_book && g_current_book->hasToc())
                                {
                                        // Request a TOC refresh when switching from reading
                                        toc_refresh = true;
                                        show_toc_ui(g_canvas);
                                        currentState_ = STATE_TOC_DISPLAY;
                                        return;
                                }
                        show_tag_ui(g_canvas);
                        currentState_ = STATE_INDEX_DISPLAY;
                        return;
                }
                else if (ty >= PAPER_S3_HEIGHT- corner_h  && tx <= PAPER_S3_WIDTH / 2 + corner_w / 2 && tx >= PAPER_S3_WIDTH/ 2 - corner_w / 2)
                {
                        // 随机shuffle章节
                        shutCnt = 0;
                        if (g_current_book && g_current_book->hasToc())
                        {
                                // Try to shuffle
                                ui_push_image_to_display_direct("/spiffs/shuffle.png", 220, 430);
                                g_current_book->goToRandomToC();
                                g_current_book->renderCurrentPage(font_size);
                        } else {
                                ui_push_image_to_display_direct("/spiffs/shuffle.png", 220, 430);
                                // Try to shuffle
                                g_current_book->goToRandomPage();
                                g_current_book->renderCurrentPage(font_size);
                        }
                        return;
                }
                else if (tx <= corner_w && ty >= PAPER_S3_HEIGHT - corner_h)
                {
                        shutCnt = 0;
                        show_lockscreen(PAPER_S3_WIDTH, PAPER_S3_HEIGHT, 30, "双击屏幕解锁");
                        // automatic tag: save current page into slot0 before entering IDLE
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
                        return;
                }
                else if (tx >= PAPER_S3_WIDTH - corner_w && ty >= PAPER_S3_HEIGHT - corner_h)
                {
                        currentState_ = STATE_READING_QUICK_MENU;
                        return;
                }
                TouchZone zone = getTouchZoneGrid(msg->data.touch.x, msg->data.touch.y);
                PageTurnResult result = handleReadingTouch(zone);
#if DBG_STATE_MACHINE_TASK
                unsigned long t_res = millis();
                sm_dbg_printf("翻页结果: success=%s, changed=%s, msg=%s, 处理耗时=%lu ms\n",
                              result.success ? "是" : "否",
                              result.page_changed ? "是" : "否",
                              result.message != nullptr ? result.message : "",
                              t_res - touch_start);
#endif

                // 检查是否需要切换到菜单状态
                if (result.success && result.message != nullptr && std::strcmp(result.message, "MENU") == 0)
                {
#if DBG_STATE_MACHINE_TASK
                        unsigned long menu_switch_start = millis();
                        sm_dbg_printf("开始切换到菜单状态: %lu ms\n", menu_switch_start);
#endif
                        currentState_ = STATE_MENU;
                        // Show menu
                        (void)show_reading_menu(g_canvas, false);
#if DBG_STATE_MACHINE_TASK
                        unsigned long menu_switch_end = millis();
                        sm_dbg_printf("菜单状态切换完成: %lu ms，总耗时: %lu ms\n",
                                      menu_switch_end, menu_switch_end - menu_switch_start);
#endif
                }
                // PREV
                if (result.success && result.message != nullptr && std::strcmp(result.message, "PREVPAGE") == 0)
                {
                        g_current_book->renderCurrentPage(font_size);
                        g_current_book->saveBookmark(); // render后保存书签
                        // 用户触摸导致的翻页，重置自动翻页计时器
                        s_one_sec_ticks = 0;
                }
                // NEXT
                if (result.success && result.message != nullptr && std::strcmp(result.message, "NEXTPAGE") == 0)
                {
                        g_current_book->renderCurrentPage(font_size);
                        g_current_book->saveBookmark(); // render后保存书签
                        // 用户触摸导致的翻页，重置自动翻页计时器
                        s_one_sec_ticks = 0;
                }
        }
        break;

        case MSG_USER_ACTIVITY:
                lastActivityTime_ = millis();
                break;

        case MSG_BATTERY_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("READING状态收到电池状态变化: %.2fV, %d%%\n", msg->data.power.voltage, msg->data.power.percentage);
#endif
                break;

        case MSG_CHARGING_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("READING状态收到充电状态变化: %s, %.2fV, %d%%\n",
                              msg->data.power.isCharging ? "开始充电" : "停止充电",
                              msg->data.power.voltage, msg->data.power.percentage);
#endif
                break;

        case MSG_POWER_EVENT:
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("READING状态收到电源事件\n");
#endif
                break;

        case MSG_DEVICE_ORIENTATION:
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("READING状态收到方向事件: %s\n", DeviceOrientationToString(msg->data.orientation.dir));
#endif
                if (msg->data.orientation.dir == ORIENT_UP)
                {
                        display_set_rotation(2);
                }
                else if (msg->data.orientation.dir == ORIENT_DOWN)
                {
                        display_set_rotation(0);
                }
                g_current_book->renderCurrentPage(font_size);
                break;

        case MSG_DOUBLE_TOUCH_PRESSED:
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("READING状态收到双击触摸: (%d, %d)\n", msg->data.touch.x, msg->data.touch.y);
#endif
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
                Serial.printf("[STATE_MACHINE] READING状态收到消息: %d\n", msg->type);
#endif
                break;
        }
}
