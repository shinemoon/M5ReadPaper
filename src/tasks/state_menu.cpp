/**
 * @brief 处理处于菜单状态 (STATE_MENU) 时的系统消息分发与响应。
 *
 * 该函数根据传入的 SystemMessage_t 消息类型执行对应的菜单状态逻辑：
 *  - 计时器超时 (MSG_TIMER_MIN_TIMEOUT)
 *    - 对内部计数器 shutCnt 自增；当达到 READING_IDLE_WAIT_MIN（以分钟为单位）时：
 *      - 重置 shutCnt
 *      - 显示锁屏（show_lockscreen）
 *      - 切换状态到 STATE_IDLE
 *
 *  - 触摸按下 (MSG_TOUCH_PRESSED)
 *    - 重置 shutCnt，更新时间戳 lastActivityTime_
 *    - 通过触摸坐标计算 TouchZone 并调用 handleMenuTouch() 获取 MenuTouchResult
 *    - 根据 MenuTouchResult 做出不同响应：
 *      - button_pressed：圆形按钮被按下 -> 显示锁屏并切换到 STATE_IDLE
 *      - panel_clicked：点击菜单面板（非按钮区域） -> 预留扩展处理点（当前仅记录调试信息）
 *      - outside_clicked：点击菜单外部区域 -> 调用 handleReadingTouch(TouchZone::FAKE_CURRENT) 并切换到 STATE_READING
 *
 *    - 在启用 DBG_STATE_MACHINE_TASK 调试宏时，会打印触摸坐标、处理结果及时间戳等详细调试信息。
 *
 *  - 用户活动 (MSG_USER_ACTIVITY)
 *    - 仅更新时间戳 lastActivityTime_
 *
 *  - 电池/充电/电源相关事件 (MSG_BATTERY_STATUS_CHANGED, MSG_CHARGING_STATUS_CHANGED, MSG_POWER_EVENT)
 *    - 目前仅在启用调试时打印相应信息，未修改状态机行为（可在此处扩展电源相关处理逻辑）
 *
 * 其他注意事项与副作用：
 *  - 会修改类的成员变量：shutCnt、currentState_、lastActivityTime_
 *  - 可能会调用外部界面/控制函数：show_lockscreen(), handleMenuTouch(), handleReadingTouch()
 *  - 假定传入的 msg 指针有效；各消息的 payload 在 msg->data 中按消息类型约定提供
 *  - 调试输出受 DBG_STATE_MACHINE_TASK 宏控制，方便在开发时观察内部决策与输入数据
 *
 * @param msg 指向 SystemMessage_t 的指针，表示接收到的系统消息（应为非空且格式合法）
 * @return 无
 */
#include "readpaper.h"
#include "state_machine_task.h"
#include "state_debug.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "text/text_handle.h"
#include "text/book_handle.h"
#include "ui/ui_control.h"
#include "ui/ui_canvas_utils.h"
#include "ui/ui_canvas_image.h"
#include "ui/ui_lock_screen.h"
#include "test/per_file_debug.h"
#include <cstring>
// for screenshot
#include "ui/screenshot.h"

#include "config/config_manager.h"

#include "current_book.h"
#include "background_index_task.h"
extern float font_size;
extern M5Canvas *g_canvas;
extern int16_t target_page;
extern GlobalConfig g_config;
#include "globals.h"
#include "text/tags_handle.h"
#include "device/safe_fs.h"
#include "ui/index_display.h"

void StateMachineTask::handleMenuState(const SystemMessage_t *msg)
{
#if DBG_STATE_MACHINE_TASK
    sm_dbg_printf("MENU状态处理消息: %d\n", msg->type);
#endif
    // 预留菜单处理逻辑
    switch (msg->type)
    {
    case MSG_TIMER_MIN_TIMEOUT: // 20min go to shut
        if (++shutCnt == READING_IDLE_WAIT_MIN)
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("MENU状态收到5分钟超时信号，j进入IDLE\n");
#endif
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
        }
        break;

    case MSG_TOUCH_PRESSED:
    {
        shutCnt = 0;
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("MENU状态收到触摸: (%d, %d)\n", msg->data.touch.x, msg->data.touch.y);
#endif
        lastActivityTime_ = millis();

        /* Disable the '标记书签', 靠页面的上角
        // Special: if touch is inside the "mark" circle in the reading menu,
        // append a tag for the current book (page start position).
        // Circle center: (280, 370), radius 32 (screen coordinates)
        int16_t tx = msg->data.touch.x;
        int16_t ty = msg->data.touch.y;
        const int16_t mark_cx = 280;
        const int16_t mark_cy = 370;
        const int16_t mark_r = 32;
        int dx = tx - mark_cx;
        int dy = ty - mark_cy;
        if ((dx * dx + dy * dy) <= (mark_r * mark_r))
        {
            // Create/insert tag at current page start position
            if (g_current_book != nullptr)
            {
                TextPageResult tp = g_current_book->currentPage();
                if (tp.success)
                {
                    size_t pos = tp.file_pos;
                    // Compute preview from in-memory page text to avoid extra file I/O
                    auto make_preview_from_utf8 = [](const std::string &s)->std::string
                    {
                        // skip leading whitespace (including U+3000) and collect up to 10 UTF-8 codepoints
                        std::string out;
                        out.reserve(64);
                        size_t len = s.size();
                        size_t idx = 0;

                        auto next_cp = [&](size_t p, uint32_t &cp, size_t &bytes)->bool
                        {
                            if (p >= len) return false;
                            unsigned char b0 = (unsigned char)s[p];
                            if ((b0 & 0x80) == 0)
                            {
                                cp = b0; bytes = 1; return true;
                            }
                            if ((b0 & 0xE0) == 0xC0)
                            {
                                if (p + 1 >= len) return false;
                                cp = ((b0 & 0x1F) << 6) | ((unsigned char)s[p+1] & 0x3F); bytes = 2; return true;
                            }
                            if ((b0 & 0xF0) == 0xE0)
                            {
                                if (p + 2 >= len) return false;
                                cp = ((b0 & 0x0F) << 12) | (((unsigned char)s[p+1] & 0x3F) << 6) | ((unsigned char)s[p+2] & 0x3F); bytes = 3; return true;
                            }
                            if ((b0 & 0xF8) == 0xF0)
                            {
                                if (p + 3 >= len) return false;
                                cp = ((b0 & 0x07) << 18) | (((unsigned char)s[p+1] & 0x3F) << 12) | (((unsigned char)s[p+2] & 0x3F) << 6) | ((unsigned char)s[p+3] & 0x3F); bytes = 4; return true;
                            }
                            // fallback
                            cp = 0; bytes = 1; return true;
                        };

                        auto is_space = [](uint32_t cp)->bool
                        {
                            if (cp <= 0x000D) return (cp >= 0x0009 && cp <= 0x000D);
                            if (cp == 0x0020) return true;
                            if (cp == 0x00A0) return true;
                            if (cp >= 0x2000 && cp <= 0x200A) return true;
                            if (cp == 0x2028 || cp == 0x2029) return true;
                            if (cp == 0x202F || cp == 0x205F) return true;
                            if (cp == 0x3000) return true;
                            return false;
                        };

                        auto is_linebreak = [](uint32_t cp)->bool
                        {
                            if (cp == 0x000A || cp == 0x000D) return true;
                            if (cp == 0x2028 || cp == 0x2029) return true;
                            return false;
                        };

                        // skip leading whitespace (includes newlines)
                        while (idx < len)
                        {
                            uint32_t cp = 0; size_t bytes = 0;
                            if (!next_cp(idx, cp, bytes)) break;
                            if (is_space(cp)) { idx += bytes; continue; }
                            break;
                        }

                        // collect up to 10 codepoints, skipping line breaks (do not count them)
                        size_t collected = 0;
                        size_t pos = idx;
                        while (pos < len && collected < 10)
                        {
                            uint32_t cp = 0; size_t bytes = 0;
                            if (!next_cp(pos, cp, bytes)) break;
                            if (is_linebreak(cp)) { pos += bytes; continue; }
                            size_t start = pos;
                            out.append(s.data() + start, bytes);
                            pos = start + bytes;
                            ++collected;
                        }
                        // trim trailing control
                        while (!out.empty() && ((unsigned char)out.back() < 0x20 || out.back() == '\0')) out.pop_back();
                        return out;
                    };

                    std::string preview = make_preview_from_utf8(tp.page_text);
                    // Insert tag using preview computed from memory (avoids file IO)
                    if (insertTagForFile(g_current_book->filePath(), pos, preview))
                    {
                        // refresh in-memory tags cache for the open book to keep UI in sync
                        if (g_current_book != nullptr)
                            g_current_book->refreshTagsCache();
                    }
                }
            }
        }
        */

        // Special-case: detect the "书签显示" button in reading menu and open index display
        // The button is drawn at draw_button(canvas, x + 330, y + 40, "书签显示", true);
        // In show_reading_menu that corresponds to an absolute rect: left=330, top=327, w=164, h=54
        int16_t tx = msg->data.touch.x;
        int16_t ty = msg->data.touch.y;

        // 检查是否点击了阅读时间区域 (x: 300-540, y: 140-178)
        if (tx >= 300 && tx < 540 && ty >= 140 && ty < 178)
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("MENU状态：点击阅读时间区域，进入 SHOW_TIME_REC\n");
#endif
            // 进入阅读时间记录显示状态
            currentState_ = STATE_SHOW_TIME_REC;
            return;
        }

        const int16_t tag_left = 450;
        const int16_t tag_top = 640 + 40;
        const int16_t tag_w = 90;
        const int16_t tag_h = 54;
        if (tx >= tag_left && tx < (tag_left + tag_w) && ty >= tag_top && ty < (tag_top + tag_h))
        {
            // Show tag UI and switch to index display state
            /*
            show_tag_ui(g_canvas);
            currentState_ = STATE_INDEX_DISPLAY;
            */
            ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
            M5.Display.waitDisplay();
            currentState_ = STATE_HELP;
            return;
        }

        TouchZone zone = getTouchZoneGrid(msg->data.touch.x, msg->data.touch.y);
#if DBG_STATE_MACHINE_TASK
        MenuTouchResult result = handleMenuTouch(zone);
        unsigned long t_res = millis();
        sm_dbg_printf("菜单触摸结果: success=%s, button=%s, panel=%s, outside=%s, msg=%s, ts=%lu\n",
                      result.success ? "是" : "否",
                      result.button_pressed ? "是" : "否",
                      result.panel_clicked ? "是" : "否",
                      result.outside_clicked ? "是" : "否",
                      result.message != nullptr ? result.message : "", t_res);
#endif

        // 处理菜单触摸结果
        MenuTouchResult touch_result = handleMenuTouch(zone);

        if (touch_result.success)
        {
            if (touch_result.button_pressed)
            {
                // 锁屏
                // 圆形按钮被按下 - lock screen
                show_lockscreen(PAPER_S3_WIDTH, PAPER_S3_HEIGHT, 30, "双击屏幕解锁");
                // automatic tag before entering IDLE
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
            else if (touch_result.button_pwr_pressed)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("PWR CLICK- 可扩展功能\n");
#endif
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
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "FBWD 10%") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("FBWD CLICK- 10%\n");
#endif

                if (g_current_book != nullptr)
                {
                    // int current_page = g_current_book->getCurrentPageIndex() + 1;
                    int total_pages = g_current_book->getTotalPages();
                    int jump_pages = total_pages * 0.1; // 10% of total pages
                    if (jump_pages < 1)
                        jump_pages = 1; // At least jump 1 page

                    target_page = target_page - jump_pages;
                    if (target_page < 1)
                        target_page = 1; // Don't go below page 1
                    char name_with_page[128];
                    // 页码
                    // Clean orginal
                    g_canvas->fillRect(160, 770, 220, 80, TFT_WHITE); // Clean

                    snprintf(name_with_page, sizeof(name_with_page), "%zu", target_page);
                    // 页码
                    // bin_font_print(name_with_page, 24, 0, 540, 0, 800, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    bin_font_print(name_with_page, 28, 0, 540, 0, 775, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    snprintf(name_with_page, sizeof(name_with_page), "%zu", total_pages);
                    bin_font_print(name_with_page, 28, 0, 540, 0, 815, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    g_canvas->drawWideLine(PAPER_S3_WIDTH / 2 - 20, 809, PAPER_S3_WIDTH / 2 + 20, 809, 1.8f, TFT_BLACK);

                    bin_font_flush_canvas(false,false,false,NOEFFECT,160,775,230,80); // PapeFWD
                }
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "BWD 1%") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("BWD CLICK- 1%\n");
#endif

                if (g_current_book != nullptr)
                {
                    // int current_page = g_current_book->getCurrentPageIndex() + 1;
                    int total_pages = g_current_book->getTotalPages();
                    // 做个微调，如果页数太多了的话，再缩小,最多10页
                    int jump_pages = (total_pages < 100) ? 1 : (total_pages * 0.01); // 1% of total pages
                    if (jump_pages < 1)
                        jump_pages = 1; // At least jump 1 page

                    target_page = target_page - jump_pages;
                    if (target_page < 1)
                        target_page = 1; // Don't go below page 1
                    char name_with_page[128];
                    // Clean orginal
                    g_canvas->fillRect(160, 770, 220, 80, TFT_WHITE); // Clean

                    snprintf(name_with_page, sizeof(name_with_page), "%zu", target_page);
                    // 页码
                    // bin_font_print(name_with_page, 24, 0, 540, 0, 800, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    bin_font_print(name_with_page, 28, 0, 540, 0, 775, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    snprintf(name_with_page, sizeof(name_with_page), "%zu", total_pages);
                    bin_font_print(name_with_page, 28, 0, 540, 0, 815, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    g_canvas->drawWideLine(PAPER_S3_WIDTH / 2 - 20, 809, PAPER_S3_WIDTH / 2 + 20, 809, 1.8f, TFT_BLACK);

                    bin_font_flush_canvas(false,false,false,NOEFFECT,160,775,230,80); // PapeBWD
                }
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "MBWD 0.1%") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("MBWD CLICK- 0.1%\n");
#endif

                if (g_current_book != nullptr)
                {
                    // int current_page = g_current_book->getCurrentPageIndex() + 1;
                    int total_pages = g_current_book->getTotalPages();
                    // 做个微调，如果页数太多了的话，再缩小,最多10页
                    int jump_pages = (total_pages < 1000) ? 1 : (total_pages * 0.001); // 1% of total pages
                    if (jump_pages < 1)
                        jump_pages = 1; // At least jump 1 page

                    target_page = target_page - jump_pages;
                    if (target_page < 1)
                        target_page = 1; // Don't go below page 1
                    char name_with_page[128];
                    // 页码
                    // Clean orginal
                    g_canvas->fillRect(160, 770, 220, 80, TFT_WHITE); // Clean

                    snprintf(name_with_page, sizeof(name_with_page), "%zu", target_page);
                    // 页码
                    // bin_font_print(name_with_page, 24, 0, 540, 0, 800, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    bin_font_print(name_with_page, 28, 0, 540, 0, 775, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    snprintf(name_with_page, sizeof(name_with_page), "%zu", total_pages);
                    bin_font_print(name_with_page, 28, 0, 540, 0, 815, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    g_canvas->drawWideLine(PAPER_S3_WIDTH / 2 - 20, 809, PAPER_S3_WIDTH / 2 + 20, 809, 1.8f, TFT_BLACK);

                    bin_font_flush_canvas(false,false,false,NOEFFECT,160,775,230,80); //MBWD 
                }
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "FFWD 10%") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("FFWD CLICK- 10%\n");
#endif

                if (g_current_book != nullptr)
                {
                    // int current_page = g_current_book->getCurrentPageIndex() + 1;
                    int total_pages = g_current_book->getTotalPages();
                    int jump_pages = total_pages * 0.1; // 10% of total pages
                    if (jump_pages < 1)
                        jump_pages = 1; // At least jump 1 page

                    target_page = target_page + jump_pages;
                    if (target_page >= total_pages)
                        target_page = total_pages; // Don't go above total page
                    char name_with_page[128];
                    // 页码
                    // Clean orginal
                    g_canvas->fillRect(160, 770, 220, 80, TFT_WHITE); // Clean
                    snprintf(name_with_page, sizeof(name_with_page), "%zu", target_page);
                    // 页码
                    // bin_font_print(name_with_page, 24, 0, 540, 0, 800, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    bin_font_print(name_with_page, 28, 0, 540, 0, 775, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    snprintf(name_with_page, sizeof(name_with_page), "%zu", total_pages);
                    bin_font_print(name_with_page, 28, 0, 540, 0, 815, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    g_canvas->drawWideLine(PAPER_S3_WIDTH / 2 - 20, 809, PAPER_S3_WIDTH / 2 + 20, 809, 1.8f, TFT_BLACK);

                    bin_font_flush_canvas(false,false,false,NOEFFECT,160,775,230,80); //FFWD
                }
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "FWD 1%") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("FWD CLICK- 1%\n");
#endif

                if (g_current_book != nullptr)
                {
                    // int current_page = g_current_book->getCurrentPageIndex() + 1;
                    int total_pages = g_current_book->getTotalPages();
                    int jump_pages = (total_pages < 100) ? 1 : (total_pages * 0.01); // 1% of total pages
                    if (jump_pages < 1)
                        jump_pages = 1; // At least jump 1 page

                    target_page = target_page + jump_pages;
                    if (target_page >= total_pages)
                        target_page = total_pages; // Don't go above total page
                    char name_with_page[128];
                    snprintf(name_with_page, sizeof(name_with_page), "%zu/%zu", target_page, total_pages);
                    // 页码
                    // Clean orginal
                    g_canvas->fillRect(160, 770, 220, 80, TFT_WHITE); // Clean

                    snprintf(name_with_page, sizeof(name_with_page), "%zu", target_page);
                    // 页码
                    // bin_font_print(name_with_page, 24, 0, 540, 0, 800, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    bin_font_print(name_with_page, 28, 0, 540, 0, 775, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    snprintf(name_with_page, sizeof(name_with_page), "%zu", total_pages);
                    bin_font_print(name_with_page, 28, 0, 540, 0, 815, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    g_canvas->drawWideLine(PAPER_S3_WIDTH / 2 - 20, 809, PAPER_S3_WIDTH / 2 + 20, 809, 1.8f, TFT_BLACK);

                    bin_font_flush_canvas(false,false,false,NOEFFECT,160,775,230,80); //FWD
                }
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "MFWD 0.1%") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("MFWD CLICK- 0.1%\n");
#endif
                if (g_current_book != nullptr)
                {
                    // int current_page = g_current_book->getCurrentPageIndex() + 1;
                    int total_pages = g_current_book->getTotalPages();
                    int jump_pages = (total_pages < 1000) ? 1 : (total_pages * 0.001); // 1% of total pages
                    if (jump_pages < 1)
                        jump_pages = 1; // At least jump 1 page

                    target_page = target_page + jump_pages;
                    if (target_page >= total_pages)
                        target_page = total_pages; // Don't go above total page
                    char name_with_page[128];
                    // 页码
                    // Clean orginal
                    g_canvas->fillRect(160, 770, 220, 80, TFT_WHITE); // Clean

                    snprintf(name_with_page, sizeof(name_with_page), "%zu", target_page);
                    // 页码
                    // bin_font_print(name_with_page, 24, 0, 540, 0, 800, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    bin_font_print(name_with_page, 28, 0, 540, 0, 775, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    snprintf(name_with_page, sizeof(name_with_page), "%zu", total_pages);
                    bin_font_print(name_with_page, 28, 0, 540, 0, 815, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
                    g_canvas->drawWideLine(PAPER_S3_WIDTH / 2 - 20, 809, PAPER_S3_WIDTH / 2 + 20, 809, 1.8f, TFT_BLACK);

                    bin_font_flush_canvas(false,false,false,NOEFFECT,160,775,230,80); //MFWD
                }
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "TWO 区域：ReIndex") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("TWO 区域：ReIndex");
#endif
                ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                // Mark re-index flag for current book
                if (g_current_book != nullptr)
                {
                    ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                    // Trigger background force reindex and wait (up to timeout) until
                    // background indexer actually starts handling the request, then
                    // jump to page 0 and render. This prevents jumping before the new
                    // index has gained control.
                    requestForceReindex();
                    // Wait up to 5s for bg indexer to start (acquire lock or begin rebuild)
                    if (waitForForceReindexStart(5000))
                    {
                        // Background has started rebuild -> jump and render
                        g_current_book->jumpToPage(0);
                        g_current_book->renderCurrentPage(font_size);
                        g_current_book->saveBookmark(); // render后保存书签
                    }
                    else
                    {
                        // Timeout: still jump/render to avoid blocking forever
                        g_current_book->jumpToPage(0);
                        g_current_book->renderCurrentPage(font_size);
                        g_current_book->saveBookmark(); // render后保存书签
                    }
                    currentState_ = STATE_READING;
                }
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "Switch Label") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("ONE 区域：Switch Label");
#endif
                // Mark re-index flag for current book
                if (g_current_book != nullptr)
                {
                    g_current_book->setShowLabel(!g_current_book->getShowLabel());
                    // Refresh Menu
                    (void)show_reading_menu(g_canvas, false,LOCKBM);
                }
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "Switch FAST") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("Switch FAST命令收到，切换主题\n");
#endif
                // dark 模式下禁止修改快刷设置
                if (!g_config.dark)
                {
                    // 切换 fastrefresh 配置并保存
                    g_config.fastrefresh = !g_config.fastrefresh;
                    config_save();
                    // 刷新阅读菜单以应用新主题 (参数以现有调用为准)
                    (void)show_reading_menu(g_canvas, false,DARKMODE);
                }
                // dark 模式下忽略此操作
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "Switch DARK") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("Switch DARK 命令收到，切换主题\n");
#endif
                // 切换 dark 配置
                g_config.dark = !g_config.dark;
                // 如果开启 dark 模式，强制启用快刷模式
                if (g_config.dark)
                {
                    g_config.fastrefresh = true;
                }
                config_save();
                // 刷新阅读菜单以应用新主题 (参数以现有调用为准)
                (void)show_reading_menu(g_canvas, false,DARKMODE);
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "Switch KeepOrg") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("THREE 区域：Switch KeepOrg");
#endif
                if (g_current_book != nullptr)
                {
                    g_current_book->setKeepOrg(!g_current_book->getKeepOrg());
                    // Refresh Menu to update checkbox display
                    (void)show_reading_menu(g_canvas, false,SKIPCONV);
                }
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "Switch DrawBottom") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("ONE 区域：Switch DrawBottom");
#endif
                if (g_current_book != nullptr)
                {
                    g_current_book->setDrawBottom(!g_current_book->getDrawBottom());
                    // Refresh Menu to update checkbox display
                    (void)show_reading_menu(g_canvas, false,UNDERLINE);
                }
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "Switch Vertical") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("THREE 区域：Switch Vertical");
#endif
                ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                // Mark re-index flag for current book
                if (g_current_book != nullptr)
                {
                    // 唯一和Index不同是修改vertical配置
                    g_current_book->setVerticalText(!g_current_book->getVerticalText());
                    ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                    // Trigger background force reindex and wait (up to timeout) until
                    // background indexer actually starts handling the request, then
                    // jump to page 0 and render.
                    requestForceReindex();
                    if (waitForForceReindexStart(5000))
                    {
                        g_current_book->jumpToPage(0);
                        g_current_book->renderCurrentPage(font_size);
                        g_current_book->saveBookmark(); // render后保存书签
                    }
                    else
                    {
                        g_current_book->jumpToPage(0);
                        g_current_book->renderCurrentPage(font_size);
                        g_current_book->saveBookmark(); // render后保存书签
                    }
                    currentState_ = STATE_READING;
                }
            }
            // Go to home panel
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "GO HOME") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("NINE 区域：HOME Button");
#endif
                // Refresh whole for Main Menu
                //                M5.Display.clearDisplay();
                (void)show_main_menu(g_canvas, false, 0, 0, true); // 重置到第一页，重新扫描文件
                currentState_ = STATE_MAIN_MENU;
            }
            else if (touch_result.panel_clicked)
            {
                // 点击了菜单面板（非按钮区域）- 可以在这里添加扩展功能
                // 用户可以在这里添加面板点击的处理逻辑
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("菜单面板点击 - 可扩展功能\n");
#endif
            }
            else if (touch_result.outside_clicked)
            {
                ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                // 点击了菜单以外的区域 - 可以在这里添加扩展功能
                // 用户可以在这里添加菜单外点击的处理逻辑
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("菜单外区域点击 - 可扩展功能\n");
#endif
                // 返回阅读
                g_current_book->jumpToPage(target_page - 1);
                g_current_book->renderCurrentPage(font_size);
                g_current_book->saveBookmark(); // render后保存书签
                currentState_ = STATE_READING;
            }
        }
    }
    break;

    case MSG_USER_ACTIVITY:
        lastActivityTime_ = millis();
        break;

    case MSG_BATTERY_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("MENU状态收到电池状态变化: %.2fV, %d%%\n", msg->data.power.voltage, msg->data.power.percentage);
#endif
        break;

    case MSG_CHARGING_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("MENU状态收到充电状态变化: %s, %.2fV, %d%%\n",
                      msg->data.power.isCharging ? "开始充电" : "停止充电",
                      msg->data.power.voltage, msg->data.power.percentage);
#endif
        break;

    case MSG_POWER_EVENT:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("MENU状态收到电源事件\n");
#endif
        break;

    case MSG_DEVICE_ORIENTATION:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("MENU状态收到方向事件: %s\n", DeviceOrientationToString(msg->data.orientation.dir));
#endif
        if (msg->data.orientation.dir == ORIENT_UP)
        {
            display_set_rotation(2);
        }
        else if (msg->data.orientation.dir == ORIENT_DOWN)
        {
            display_set_rotation(0);
        }
        (void)show_reading_menu(g_canvas, true);
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
        Serial.printf("[STATE_MACHINE] MENU状态收到消息: %d\n", msg->type);
#endif
        break;
    }
}
