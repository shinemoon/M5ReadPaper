/**
 * @brief 处理处于主菜单状态 (STATE_MAIN_MENU) 时的系统消息分发与响应。
 *
 * 该函数根据传入的 SystemMessage_t 消息类型执行对应的主菜单状态逻辑：
 *  - 计时器超时 (MSG_TIMER_MIN_TIMEOUT)
 *    - 对内部计数器 shutCnt 自增；当达到 READING_IDLE_WAIT_MIN（以分钟为单位）时：
 *      - 重置 shutCnt
 *      - 显示锁屏（show_lockscreen）
 *      - 切换状态到 STATE_IDLE
 *
 *  - 触摸按下 (MSG_TOUCH_PRESSED)
 *    - 重置 shutCnt，更新时间戳 lastActivityTime_
 *    - 通过触摸坐标计算 TouchZone 并调用 handleMainMenuTouch() 获取 MenuTouchResult
 *    - 目前仅打印触摸信息，不执行具体的业务逻辑
 *
 *  - 用户活动 (MSG_USER_ACTIVITY)
 *    - 仅更新时间戳 lastActivityTime_
 *
 *  - 电池/充电/电源相关事件 (MSG_BATTERY_STATUS_CHANGED, MSG_CHARGING_STATUS_CHANGED, MSG_POWER_EVENT)
 *    - 目前仅在启用调试时打印相应信息，未修改状态机行为（可在此处扩展电源相关处理逻辑）
 *
 * 其他注意事项与副作用：
 *  - 会修改类的成员变量：shutCnt、currentState_、lastActivityTime_
 *  - 可能会调用外部界面/控制函数：show_lockscreen(), handleMainMenuTouch()
 *  - 假定传入的 msg 指针有效；各消息的 payload 在 msg->data 中按消息类型约定提供
 *  - 调试输出受 DBG_STATE_MACHINE_TASK 宏控制，方便在开发时观察内部决策与输入数据
 *
 * @param msg 指向 SystemMessage_t 的指针，表示接收到的系统消息（应为非空且格式合法）
 * @return 无
 */
#include "readpaper.h"
#include "state_machine_task.h"
#include "../ui/ui_canvas_utils.h"
#include "../ui/ui_canvas_2nd_utils.h"
#include "state_debug.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "device/wifi_hotspot_manager.h"
#include "text/text_handle.h"
#include "text/book_handle.h"
#include "ui/ui_control.h"
#include "ui/ui_canvas_utils.h"
#include "ui/ui_canvas_image.h"
#include "ui/ui_lock_screen.h"
#include "ui/screenshot.h"
#include "test/per_file_debug.h"
#include "config/config_manager.h"
#include "readpaper.h"
#include <cstring>
#include "SD/SDWrapper.h"

#include "current_book.h"
#include "globals.h"
extern float font_size;
extern M5Canvas *g_canvas;
extern int16_t target_page;
extern GlobalConfig g_config;
#include "text/tags_handle.h"

// for 2nd level
extern int8_t opt;
extern int16_t opt2;

int8_t mainMenuIndex = 0;  // Main menu selected one
int current_file_page = 0; // 当前文件列表页数

// 获取文件列表总页数的辅助函数
int getTotalPages()
{
    int total_files = get_cached_book_count();
    return (total_files + FILES_PER_PAGE - 1) / FILES_PER_PAGE; // 向上取整
}

void StateMachineTask::handleMainMenuState(const SystemMessage_t *msg)
{
#if DBG_STATE_MACHINE_TASK
    sm_dbg_printf("主菜单状态处理消息: %d\n", msg->type);
#endif
    // 主菜单处理逻辑
    switch (msg->type)
    {
    case MSG_TIMER_MIN_TIMEOUT: // 超时进入idle
        if (++shutCnt == READING_IDLE_WAIT_MIN)
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("主菜单状态收到超时信号，进入IDLE\n");
#endif
            shutCnt = 0;
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
        break;
    case MSG_DEVICE_ORIENTATION:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("Main Menu 状态收到方向事件: %s\n", DeviceOrientationToString(msg->data.orientation.dir));
#endif
        if (msg->data.orientation.dir == ORIENT_UP)
        {
            display_set_rotation(2);
        }
        else if (msg->data.orientation.dir == ORIENT_DOWN)
        {
            display_set_rotation(0);
        }
        show_main_menu(g_canvas, true, mainMenuIndex , current_file_page, false);
        break;

    case MSG_TOUCH_PRESSED:
    {
        shutCnt = 0;
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("主菜单状态收到触摸: (%d, %d)\n", msg->data.touch.x, msg->data.touch.y);
#endif
        lastActivityTime_ = millis();

        TouchZone zone = getTouchZoneGrid(msg->data.touch.x, msg->data.touch.y);
        // 处理主菜单触摸结果（目前仅打印，不做具体处理）
        MenuTouchResult touch_result = handleMainMenuTouch(zone);

        if (touch_result.success)
        {
            // TODO: 在这里添加具体的主菜单触摸处理逻辑
            if (touch_result.message != nullptr && std::strcmp(touch_result.message, "RETURN READ") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("主菜单状态返回阅读\n");
#endif

                // Ensure bookmark/font checks are performed before entering reading.
                // This will trigger forceReindex() inside loadBookmarkAndJump() if
                // the saved bookmark's font metadata doesn't match the current font.
                if (g_current_book)
                {
                    g_current_book->loadBookmarkAndJump();
                    g_current_book->renderCurrentPage(0, g_canvas);
                }
                currentState_ = STATE_READING;
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "DISPLAY SETTING") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("主菜单收到显示设置\n");
#endif
               // 改为先显示二级菜单并进入 STATE_2ND_LEVEL_MENU。
                // 如果需要恢复原功能，可在此处取消注释原代码。

                // 设定二级菜单类型为清理书签
                main_2nd_level_menu_type = MAIN_2ND_MENU_DISPLAY_SETTING;
                // 显示二级菜单（中心白色矩形）
                show_2nd_level_menu(g_canvas,true);
                // 切换状态到二级菜单
                currentState_ = STATE_2ND_LEVEL_MENU;
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "OPEN BOOK") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("主菜单收到打开书籍信号\n");
#endif
                ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                M5.Display.waitDisplay();
                // 获取当前选中的文件名
                std::string selected_book_name = get_cached_book_name(current_file_page, mainMenuIndex);

                if (!selected_book_name.empty())
                {
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("打开书籍: %s (页面%d, 索引%d)\n", selected_book_name.c_str(), current_file_page, mainMenuIndex);
#endif

                    // 构造完整的文件路径（支持 history.list 的原始路径或默认 /sd/book/<name>.txt）
                    std::string book_path = get_selected_book_fullpath(current_file_page, mainMenuIndex);
                    if (book_path.empty())
                    {
                        // fallback to old behavior
                        book_path = std::string("/sd/book/") + selected_book_name + ".txt";
                    }

                    // 使用 config_update_current_book 来创建新的 BookHandle
                    // 这会自动处理配置更新和书签加载
                    int16_t area_w = PAPER_S3_WIDTH - MARGIN_LEFT - MARGIN_RIGHT;
                    int16_t area_h = PAPER_S3_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM;
                    float fsize = (float)get_font_size_from_file();

                    BookHandle *new_book = config_update_current_book(book_path.c_str(), area_w, area_h, fsize);
#include "current_book.h"

                    if (new_book != nullptr)
                    {
                        // Obtain the shared_ptr snapshot that was published by
                        // config_update_current_book. DO NOT construct a new
                        // shared_ptr from the raw pointer (would create a separate
                        // control block and lead to double-delete).
                        auto new_sp = current_book_shared();
                        if (new_sp)
                        {
#if DBG_STATE_MACHINE_TASK
                            sm_dbg_printf("成功加载书籍: %s, 总页数: %zu\n", book_path.c_str(), new_sp->getTotalPages());
#endif
                            new_sp->renderCurrentPage(0, g_canvas);
                            currentState_ = STATE_READING;
                        }
                        else
                        {
#if DBG_STATE_MACHINE_TASK
                            sm_dbg_printf("警告: new_book 返回但 global shared_ptr 未发布，跳过渲染\n");
#endif
                        }
                    }
                    else
                    {
#if DBG_STATE_MACHINE_TASK
                        sm_dbg_printf("创建 BookHandle 失败\n");
#endif
                        // 打开失败，可能是文件已被删除，从 history.list 中移除
                        extern bool removeBookFromHistory(const std::string &book_path);
                        removeBookFromHistory(book_path);
                    }
                }
                else
                {
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("无效的书籍选择，页面%d索引%d\n", current_file_page, mainMenuIndex);
#endif
                }
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "PREV PAGE") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("主菜单收到上一页信号\n");
#endif
                // 循环翻页：第一页向前跳到最后一页
                {
                    int total_pages = getTotalPages();
                    if (total_pages <= 0)
                    {
                        // 没有文件或页数不可用，保持为0
                        current_file_page = 0;
                    }
                    else if (current_file_page > 0)
                    {
                        current_file_page--;
                    }
                    else
                    {
                        current_file_page = total_pages - 1; // wrap to last page
                    }
                    show_main_menu(g_canvas, false, 0, current_file_page, false, true, 1);
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("循环切换到第 %d 页 (共%d页)\n", current_file_page + 1, total_pages);
#endif
                }
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "NEXT PAGE") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("主菜单收到下一页信号\n");
#endif
                {
                    int total_pages = getTotalPages();
                    if (total_pages <= 0)
                    {
                        current_file_page = 0;
                    }
                    else if (current_file_page < total_pages - 1)
                    {
                        current_file_page++;
                    }
                    else
                    {
                        current_file_page = 0; // wrap to first page
                    }
                    show_main_menu(g_canvas, false, 0, current_file_page, false, true, 1);
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("循环切换到第 %d 页 (共%d页)\n", current_file_page + 1, total_pages);
#endif
                }
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "FONT TOGGLE") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("主菜单收到字体切换信号\n");
#endif

                opt = 0;
                opt2 = 0;
                main_2nd_level_menu_type = MAIN_2ND_MENU_FONT_SETTING;
                // 准备字体列表并传入二级菜单以避免在UI函数内扫描（更灵活，也便于测试）
                {
                    // 使用 init_filesystem() 时已完成一次字体列表刷新，直接显示二级菜单
                    show_2nd_level_menu(g_canvas,true);
                }
                // 切换状态到二级菜单
                currentState_ = STATE_2ND_LEVEL_MENU;
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "TOGGLE_RECENT") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("主菜单收到切换最近文件来源信号\n");
#endif
                // Only allow toggling to "最近打开" when /history.list exists and contains at least
                // one non-empty line. If history.list is missing or empty, keep show_recent == false
                // and simply refresh the menu showing "按文件名".
                const char *HPATH = "/history.list";
                bool allow_toggle = false;
                if (SDW::SD.exists(HPATH))
                {
                    File hf = SDW::SD.open(HPATH, "r");
                    if (hf)
                    {
                        // scan for at least one non-empty line
                        while (hf.available())
                        {
                            String line = hf.readStringUntil('\n');
                            line.trim();
                            if (line.length() > 0)
                            {
                                allow_toggle = true;
                                break;
                            }
                        }
                        hf.close();
                    }
                }

                if (allow_toggle)
                {
                    show_recent = !show_recent;
                }
                else
                {
                    // ensure we display file-by-name when history is not usable
                    show_recent = false;
                }
                // force reload menu (do not rescan files when toggling)
                //show_main_menu(g_canvas, true, 0, current_file_page, false,true,2);
                show_main_menu(g_canvas, false, 0, current_file_page, false,true,2);
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "TOGGLE_ZH_CONV") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("主菜单切换 繁简模式\n");
#endif
                // Toggle between 简体 (1) and 繁体 (2). If currently 0 (no conv), set to 1.
                if (g_config.zh_conv_mode == 2)
                {
                    g_config.zh_conv_mode = 1;
                }
                else
                {
                    g_config.zh_conv_mode = 2;
                }
                // Persist and refresh menu
                config_save();
                show_main_menu(g_canvas, true, 0, current_file_page, false);
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "CLEAN BOOKMARK") == 0)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("主菜单收到书签清理 (已切换为二级菜单显示)\n");
#endif

                // 注：原有清理逻辑（遍历 /bookmarks 并删除孤立文件）已注释掉，
                // 改为先显示二级菜单并进入 STATE_2ND_LEVEL_MENU。
                // 如果需要恢复原功能，可在此处取消注释原代码。

                // 设定二级菜单类型为清理书签
                main_2nd_level_menu_type = MAIN_2ND_MENU_CLEAN_BOOKMARK;
                // 显示二级菜单（中心白色矩形）
                show_2nd_level_menu(g_canvas,true);
                // 切换状态到二级菜单
                currentState_ = STATE_2ND_LEVEL_MENU;
            }
            else if (touch_result.message != nullptr && std::strcmp(touch_result.message, "WIRE CONNECT") == 0)
            {
                // 打开“连接方式”二级菜单，让用户选择“有线/无线”
                main_2nd_level_menu_type = MAIN_2ND_MENU_CONNECT_METHOD;
                show_2nd_level_menu(g_canvas,true);
                currentState_ = STATE_2ND_LEVEL_MENU;
            }
            else if (touch_result.message != nullptr && std::strncmp(touch_result.message, "SELECT BOOK:", std::strlen("SELECT BOOK:")) == 0)
            {
                const char *book_spec = touch_result.message + std::strlen("SELECT BOOK:");
                while (*book_spec == ' ')
                    ++book_spec; // skip spaces after colon
                if (*book_spec != '\0')
                {
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("主菜单选择书籍: %s\n", book_spec);
#endif
                    // 仅支持单字符 '0'-'9' 转换成 0-9
                    if (book_spec[0] >= '0' && book_spec[0] <= '9' && book_spec[1] == '\0')
                    {
#if DBG_STATE_MACHINE_TASK
                        sm_dbg_printf("解析书籍索引: %d\n", book_spec[0] - '0');
#endif
                        bool invertColor = false;
                        int clicked_index = book_spec[0] - '0';

                        // Validate against number of files on current page. 如果点击的是空槽，忽略之。
                        int total_files = get_cached_book_count();
                        int page_start = current_file_page * FILES_PER_PAGE;
                        int page_end = std::min(page_start + FILES_PER_PAGE, total_files);
                        int files_to_show = page_end - page_start;
                        if (clicked_index >= files_to_show)
                        {
#if DBG_STATE_MACHINE_TASK
                            sm_dbg_printf("主菜单点击空槽，忽略 (index=%d, files_to_show=%d)\n", clicked_index, files_to_show);
#endif
                            break; // ignore this selection
                        }
                        // 选择书籍
                        mainMenuIndex = clicked_index;
                        // 如果是选中的项，绘制侧边
                        /*
                        g_canvas->fillRect(355, 0, 5, 960, TFT_WHITE);
                        g_canvas->fillTriangle(360, 96 * mainMenuIndex, 360, 96 * mainMenuIndex + 96, 355, 96 * mainMenuIndex + 48, TFT_BLACK);
                        */
                        g_canvas->fillRect(360, 0, 2, 960, invertColor ? TFT_WHITE : TFT_BLACK);
                        g_canvas->fillRect(360, 96 * mainMenuIndex + 2, 2, 94, invertColor ? TFT_BLACK : TFT_WHITE);
                        g_canvas->fillRect(0, 96 * mainMenuIndex, (mainMenuIndex == 0) ? 540 : 360, 2, invertColor ? TFT_WHITE : TFT_BLACK);
                        g_canvas->fillRect(0, 96 * mainMenuIndex + 96, (mainMenuIndex == 9) ? 540 : 360, 2, invertColor ? TFT_WHITE : TFT_BLACK);
                        //Special Part for click handling
                        bin_font_flush_canvas(false,false,false,NOEFFECT,359,0,3,960);
                    }
                }
                else
                {
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("无效的书籍索引: '%s'\n", book_spec);
#endif
                }
                // TODO: 使用 BookHandle 的接口打开指定书籍（按路径或名称）
                // 示例： g_current_book->openBookByPath(book_spec);
                // 暂时将目标页置为 0 并进入阅读状态，渲染当前书的第一页（如果已加载）
                //                  target_page = 0;
                //                    g_current_book->renderCurrentPage(0, g_canvas);
                //                   currentState_ = STATE_READING;
            }
        }
        break;
    }
    case MSG_TOUCH_RELEASED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("主菜单状态收到触摸释放\n");
#endif
        break;

    case MSG_TOUCH_EVENT:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("主菜单状态收到触摸事件\n");
#endif
        break;

    case MSG_DOUBLE_TOUCH_PRESSED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("主菜单状态收到双击触摸: (%d, %d)\n", msg->data.touch.x, msg->data.touch.y);
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

    case MSG_USER_ACTIVITY:
        lastActivityTime_ = millis();
        break;

    case MSG_BATTERY_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("主菜单状态收到电池状态变化: %.2fV, %d%%\n", msg->data.power.voltage, msg->data.power.percentage);
#endif
        break;

    case MSG_CHARGING_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("主菜单状态收到充电状态变化: %s, %.2fV, %d%%\n",
                      msg->data.power.isCharging ? "开始充电" : "停止充电",
                      msg->data.power.voltage, msg->data.power.percentage);
#endif
        break;

    case MSG_POWER_EVENT:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("主菜单状态收到电源事件\n");
#endif
        break;

    default:
#if DBG_STATE_MACHINE_TASK
        Serial.printf("[STATE_MACHINE] 主菜单状态收到消息: %d\n", msg->type);
#endif
        break;
    }
}