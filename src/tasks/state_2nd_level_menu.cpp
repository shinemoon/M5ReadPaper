#include "state_machine_task.h"
#include "state_debug.h"
#include "readpaper.h"
#include "ui/ui_canvas_image.h"
#include "ui/ui_control.h"
#include "device/ui_display.h"
#include "ui/ui_canvas_utils.h"
#include "ui/ui_lock_screen.h"
#include "ui/ui_canvas_2nd_utils.h"
#include "config/config_manager.h"
#include <string.h>
#include "globals.h"
#include "SD/SDWrapper.h"
#include "device/efficient_file_scanner.h"
#include "device/wifi_hotspot_manager.h"
#include "device/usb_msc.h"
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <functional>
#include "test/per_file_debug.h"
#include "tasks/background_index_task.h"
// for screenshot
#include "ui/screenshot.h"

extern M5Canvas *g_canvas;
extern int current_file_page; // 当前文件列表页数
extern GlobalConfig g_config;
extern std::vector<FontFileInfo, PSRAMAllocator<FontFileInfo>> g_font_list;
#include "text/tags_handle.h"
#include "text/book_handle.h"

// 定义全局二级菜单类型变量（默认 CLEAN_BOOKMARK）
Main2ndLevelMenuType main_2nd_level_menu_type;

// Option input
int8_t opt = 0;
int16_t opt2 = 0;

void StateMachineTask::handle2ndLevelMenuState(const SystemMessage_t *msg)
{
    if (!msg)
        return;

#if DBG_STATE_MACHINE_TASK
    sm_dbg_printf("STATE_2ND_LEVEL_MENU 收到消息: %d\n", msg->type);
#endif

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
        sm_dbg_printf("2ND_LEVEL_MENU收到方向事件: %s\n", DeviceOrientationToString(msg->data.orientation.dir));
#endif
        if (msg->data.orientation.dir == ORIENT_UP)
        {
            display_set_rotation(2);
        }
        else if (msg->data.orientation.dir == ORIENT_DOWN)
        {
            display_set_rotation(0);
        }
        show_2nd_level_menu(); // Orient Change
        break;

    case MSG_TOUCH_PRESSED:
    {
        TouchZone zone = getTouchZoneGrid(msg->data.touch.x, msg->data.touch.y);
        // 根据当前二级菜单类型执行相应动作
        if (main_2nd_level_menu_type == MAIN_2ND_MENU_DISPLAY_SETTING)
        {
            int8_t updateInd = 0; // Tell which config changed in this folder
            // 1 - Rotation (手柄上下)
            // 2 - Hand(左右手)
            // 3 - Bookmark Location(书签显示)
            // 4 - BookMark Style(书签主题)
            // 5 - Lockscreen(锁屏)
            int16_t px = msg->data.touch.x;
            int16_t py = msg->data.touch.y;
            const int16_t rectH = 6 * 96;
            const int16_t rectY = (PAPER_S3_HEIGHT - rectH) / 2;
            const int16_t rotationRowY = rectY + 120;
            const int16_t pageStyleRowY = rectY + 2 * 96 - 1 + 25;
            const int16_t labelRowY = rectY + 3 * 96 - 1 + 25;
            const int16_t themeRowY = rectY + 4 * 96 - 1 + 25;
            const int16_t wallpaperRowY = rectY + 5 * 96 - 1 + 25;
            const auto inRow = [&](int16_t rowY)
            { return py >= rowY - 20 && py <= rowY + 20; };
            const auto inBox = [&](int16_t rowY, int16_t left, int16_t width)
            {
                return inRow(rowY) && px >= left && px <= left + width;
            };

            bool updated = false;

            if (inRow(rotationRowY))
            {
                if (inBox(rotationRowY, 210, 160) && g_config.rotation != 2)
                {
                    g_config.rotation = 2;
                    updated = true;
                }
                else if (inBox(rotationRowY, 360, 160) && g_config.rotation != 0)
                {
                    g_config.rotation = 0;
                    updated = true;
                }
                updateInd = 1;
            }

            if (!updated && inRow(pageStyleRowY))
            {
                if (inBox(pageStyleRowY, 210, 160) && strcmp(g_config.pageStyle, "default") != 0)
                {
                    strcpy(g_config.pageStyle, "default");
                    updated = true;
                }
                else if (inBox(pageStyleRowY, 360, 160) && strcmp(g_config.pageStyle, "revert") != 0)
                {
                    strcpy(g_config.pageStyle, "revert");
                    updated = true;
                }
                updateInd = 2;
            }

            if (!updated && inRow(labelRowY))
            {
                if (inBox(labelRowY, 210, 120) && strcmp(g_config.labelposition, "default") != 0)
                {
                    strcpy(g_config.labelposition, "default");
                    updated = true;
                }
                else if (inBox(labelRowY, 330, 120) && strcmp(g_config.labelposition, "middle") != 0)
                {
                    strcpy(g_config.labelposition, "middle");
                    updated = true;
                }
                else if (inBox(labelRowY, 430, 120) && strcmp(g_config.labelposition, "top") != 0)
                {
                    strcpy(g_config.labelposition, "top");
                    updated = true;
                }
                updateInd = 3;
            }

            if (!updated && inRow(themeRowY))
            {
                if (inBox(themeRowY, 210, 120) && strcmp(g_config.marktheme, "dark") != 0)
                {
                    strcpy(g_config.marktheme, "dark");
                    updated = true;
                }
                else if (inBox(themeRowY, 330, 120) && strcmp(g_config.marktheme, "light") != 0)
                {
                    strcpy(g_config.marktheme, "light");
                    updated = true;
                }
                else if (inBox(themeRowY, 430, 120) && strcmp(g_config.marktheme, "random") != 0)
                {
                    strcpy(g_config.marktheme, "random");
                    updated = true;
                }
                updateInd = 4;
            }

            if (!updated && inRow(wallpaperRowY))
            {
                if (inBox(wallpaperRowY, 210, 140) && !g_config.defaultlock)
                {
                    g_config.defaultlock = true;
                    updated = true;
                }
                else if (inBox(wallpaperRowY, 360, 140) && g_config.defaultlock)
                {
                    g_config.defaultlock = false;
                    updated = true;
                }
                updateInd = 5;
            }

            if (updated)
            {
                config_save();
                show_2nd_level_menu(g_canvas, true, updateInd);
                return;
            }
        }

        if (main_2nd_level_menu_type == MAIN_2ND_MENU_CONNECT_METHOD)
        {
            // Two buttons were drawn at center: top = 有线连接, bottom = 无线连接
            int16_t cx = msg->data.touch.x;
            int16_t cy = msg->data.touch.y;
            // New layout: wireless button is centered in the rect; wired UI is hidden
            // (wired touch area moved to center rect top-right 60x60). Compute hit
            // boxes accordingly.
            const int16_t rectH = 4 * 96;                        // same rect height as used by the drawer
            const int16_t rectY = (PAPER_S3_HEIGHT - rectH) / 2; // top y of center rect
            const int16_t w = 164;
            const int16_t h = 54;

            // Wireless button centered: its center Y is the screen center (rect center)
            int16_t btn_cx = PAPER_S3_WIDTH / 2;  // center x
            int16_t btn_cy = PAPER_S3_HEIGHT / 2; // center y (rect center)

            // Wireless hitbox
            if (cx >= btn_cx - w / 2 && cx <= btn_cx + w / 2 && cy >= btn_cy - 16 && cy <= btn_cy - 16 + h)
            {
                // Wireless: start hotspot and go to wire connect state (reuse original logic)
                ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                M5.Display.waitDisplay();
                // 初始化WiFi热点管理器（如果尚未初始化）
                wifi_hotspot_init();

                // 启动WiFi热点
                if (g_wifi_hotspot && g_wifi_hotspot->start())
                {
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("WiFi热点启动成功\n");
#endif
                }
                else
                {
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("WiFi热点启动失败\n");
#endif
                }

                show_wire_connect(g_canvas, true);
                currentState_ = STATE_WIRE_CONNECT;
            }

            // Wired touch region: top-right 60x60 inside the center rect
            const int16_t wired_area_w = 60;
            const int16_t wired_area_h = 60;
            int16_t wired_x0 = PAPER_S3_WIDTH - wired_area_w; // rect x is 0 since rectW == screen width
            int16_t wired_y0 = rectY;                         // top of the center rect

            if (cx >= wired_x0 && cx < (wired_x0 + wired_area_w) && cy >= wired_y0 + 64 && cy < (wired_y0 + wired_area_h + 64))
            {
                // 有线连接 - 启动 USB MSC 并打开页面
                ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                M5.Display.waitDisplay();

                currentState_ = STATE_USB_CONNECT;
                // 初始化并启动 USB MSC
                show_usb_connect(g_canvas, true);
                vTaskDelay(pdMS_TO_TICKS(1000));
                usb_msc_init();
                if (usb_msc_start())
                {
                    Serial.println("[2ND_MENU] USB MSC started successfully");
                }
                else
                {
                    Serial.println("[2ND_MENU] Failed to start USB MSC");
                    // 返回主菜单
                    show_main_menu(g_canvas, false, 0, 0, false);
                    currentState_ = STATE_MAIN_MENU;
                }
            }
        }

        if (main_2nd_level_menu_type == MAIN_2ND_MENU_CLEAN_BOOKMARK)
        {
            //
            if (zone == TouchZone::FIVE_FOUR || zone == TouchZone::FIVE_THREE)
            { // 恢复出厂 (等同于原删除所有书签/历史/配置)
                // 显示等待图片
                ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                M5.Display.waitDisplay();
                // 复用已有的清理逻辑：删除 /bookmarks 和 /screenshot 下所有文件，并删除根目录下 history.list 和 readpaper.cfg
                const char *bmDir = "/bookmarks";
                const char *ssDir = "/screenshot";
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("恢复出厂: 开始清理 %s 和 %s 目录\n", bmDir, ssDir);
#endif
                // 定义通用的清理函数
                auto cleanDirectory = [&](const char *dirPath)
                {
                    if (SDW::SD.exists(dirPath))
                    {
                        // 递归删除所有文件
                        std::function<void(const char *)> removeAllFiles = [&](const char *subDirPath)
                        {
                            File dir = SDW::SD.open(subDirPath);
                            if (!dir)
                            {
#if DBG_STATE_MACHINE_TASK
                                sm_dbg_printf("恢复出厂: 无法打开目录 %s\n", subDirPath);
#endif
                                return;
                            }

                            File entry;
                            int count = 0;
                            while (entry = dir.openNextFile())
                            {
                                const char *entryName = entry.name();
                                if (!entryName)
                                {
                                    entry.close();
                                    continue;
                                }

                                std::string name(entryName);
                                std::string fullPath = name;

                                // 如果名称不包含完整路径，构建它
                                if (fullPath.find(subDirPath) == std::string::npos)
                                {
                                    fullPath = std::string(subDirPath) + "/" + name.substr(name.find_last_of('/') + 1);
                                }

                                bool isDir = entry.isDirectory();
                                entry.close();

                                if (isDir)
                                {
                                    // 递归处理子目录
                                    removeAllFiles(fullPath.c_str());
                                }
                                else
                                {
                                    // 删除文件
                                    bool removed = SDW::SD.remove(fullPath.c_str());
                                    count++;
#if DBG_STATE_MACHINE_TASK
                                    sm_dbg_printf("恢复出厂: [%d] 删除 %s - %s\n", count, fullPath.c_str(), removed ? "成功" : "失败");
#else
                                    (void)removed; // 标记为有意未使用
#endif
                                }
                            }
                            dir.close();
#if DBG_STATE_MACHINE_TASK
                            sm_dbg_printf("恢复出厂: 目录 %s 共删除 %d 个文件\n", subDirPath, count);
#endif
                        };

                        removeAllFiles(dirPath);
                    }
                };

                // 清理 /bookmarks 和 /screenshot 目录
                cleanDirectory(bmDir);
                cleanDirectory(ssDir);

                // 删除根目录下的 history.list 和 readpaper.cfg 相关文件
                const char *hist = "/history.list";
                const char *cfg = "/readpaper.cfg";
                const char *cfgA = "/readpaper.cfg.A";
                const char *cfgB = "/readpaper.cfg.B";
                if (SDW::SD.exists(hist))
                {
                    SDW::SD.remove(hist);
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("恢复出厂: 删除 %s\n", hist);
#endif
                }
                if (SDW::SD.exists(cfg))
                {
                    SDW::SD.remove(cfg);
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("恢复出厂: 删除 %s\n", cfg);
#endif
                }
                if (SDW::SD.exists(cfgA))
                {
                    SDW::SD.remove(cfgA);
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("恢复出厂: 删除 %s\n", cfgA);
#endif
                }
                if (SDW::SD.exists(cfgB))
                {
                    SDW::SD.remove(cfgB);
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("恢复出厂: 删除 %s\n", cfgB);
#endif
                }
                // 重新显示主菜单
                show_main_menu(g_canvas, false, 0, 0, false);
                currentState_ = STATE_MAIN_MENU;
            }
            else if (zone == TouchZone::SIX_FOUR || zone == TouchZone::SIX_THREE)
            { // 清理残存
                ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                M5.Display.waitDisplay();

                // 扫描 /bookmarks/ 下所有文件，删除找不到对应书籍的孤立书签
                const char *bmDir2 = "/bookmarks";
                if (SDW::SD.exists(bmDir2))
                {
                    File bmDir = SDW::SD.open(bmDir2);
                    if (bmDir)
                    {
                        File entry;
                        int deletedCount = 0;
                        while (entry = bmDir.openNextFile())
                        {
                            const char *entryName = entry.name();
                            if (!entryName)
                            {
                                entry.close();
                                continue;
                            }

                            std::string fullPath(entryName);
                            bool isDir = entry.isDirectory();
                            entry.close();

                            if (isDir)
                                continue;

                            // 提取文件名 basename（不含路径和扩展名）
                            size_t pos = fullPath.find_last_of('/');
                            std::string fname = (pos != std::string::npos) ? fullPath.substr(pos + 1) : fullPath;
                            size_t dot = fname.find_last_of('.');
                            std::string base = (dot != std::string::npos) ? fname.substr(0, dot) : fname;

                            bool has_owner = false;

                            // 检查是否有 _sd_book_ 前缀
                            if (base.rfind("_sd_book_", 0) == 0)
                            {
                                // 去掉 _sd_book_ 前缀，还原原始文件名（下划线替换回斜杠）
                                std::string bookName = base.substr(9); // 跳过 "_sd_book_"
                                // 将下划线替换回斜杠以还原路径
                                std::string bookPath = "/book/";
                                for (char c : bookName)
                                {
                                    if (c == '_')
                                        bookPath += '/';
                                    else
                                        bookPath += c;
                                }
                                bookPath += ".txt";

                                // 检查 SD 卡上是否存在该书籍
                                if (SDW::SD.exists(bookPath.c_str()))
                                    has_owner = true;
                            }
                            // 检查是否有 _spiffs_ 前缀
                            else if (base.rfind("_spiffs_", 0) == 0)
                            {
                                // 去掉 _spiffs_ 前缀，还原原始文件名
                                std::string bookName = base.substr(8); // 跳过 "_spiffs_"
                                std::string bookPath = "/" + bookName + ".txt";

                                // 检查 SPIFFS 上是否存在该书籍
                                if (SPIFFS.exists(bookPath.c_str()))
                                    has_owner = true;
                            }

                            if (!has_owner)
                            {
                                bool removed = SDW::SD.remove(fullPath.c_str());
                                deletedCount++;
#if DBG_STATE_MACHINE_TASK
                                sm_dbg_printf("清理残存: 删除孤立书签 %s - %s\n", fullPath.c_str(), removed ? "成功" : "失败");
#else
                                (void)removed;
#endif
                            }
                        }
                        bmDir.close();
#if DBG_STATE_MACHINE_TASK
                        sm_dbg_printf("清理残存: 共删除 %d 个孤立书签文件\n", deletedCount);
#endif
                    }

                    // 清理 /book 目录下的孤立 .idx 文件
                    const char *bookDir = "/book";
                    if (SDW::SD.exists(bookDir))
                    {
                        File bDir = SDW::SD.open(bookDir);
                        if (bDir)
                        {
                            File entry;
                            int idxDeletedCount = 0;
                            while (entry = bDir.openNextFile())
                            {
                                const char *entryName = entry.name();
                                if (!entryName)
                                {
                                    entry.close();
                                    continue;
                                }

                                std::string fullPath(entryName);
                                bool isDir = entry.isDirectory();
                                entry.close();

                                if (isDir)
                                    continue;

                                // 确保 fullPath 是完整路径
                                if (fullPath.find("/book/") == std::string::npos)
                                {
                                    // 只有文件名，需要添加目录前缀
                                    size_t pos = fullPath.find_last_of('/');
                                    std::string fname = (pos != std::string::npos) ? fullPath.substr(pos + 1) : fullPath;
                                    fullPath = std::string(bookDir) + "/" + fname;
                                }

                                // 只处理 .idx 文件
                                if (fullPath.length() < 4 || fullPath.substr(fullPath.length() - 4) != ".idx")
                                    continue;

                                // 检查是否有对应的 .txt 文件（同名不同扩展名）
                                std::string txtPath = fullPath.substr(0, fullPath.length() - 4) + ".txt";
#if DBG_STATE_MACHINE_TASK
                                sm_dbg_printf("清理残存: 检查 .idx 文件 %s，对应 txt 路径: %s\n", fullPath.c_str(), txtPath.c_str());
#endif
                                if (!SDW::SD.exists(txtPath.c_str()))
                                {
                                    // 没有对应的 .txt，删除这个孤立的 .idx
                                    bool removed = SDW::SD.remove(fullPath.c_str());
                                    idxDeletedCount++;
#if DBG_STATE_MACHINE_TASK
                                    sm_dbg_printf("清理残存: 删除孤立 .idx %s - %s\n", fullPath.c_str(), removed ? "成功" : "失败");
#else
                                    (void)removed;
#endif
                                }
                            }
                            bDir.close();
#if DBG_STATE_MACHINE_TASK
                            sm_dbg_printf("清理残存: 共删除 %d 个孤立 .idx 文件\n", idxDeletedCount);
#endif
                        }
                    }
                }

                // Complete - show main menu
                show_main_menu(g_canvas, false, 0, 0, false);
                currentState_ = STATE_MAIN_MENU;
            }
        }

        if (main_2nd_level_menu_type == MAIN_2ND_MENU_FONT_SETTING)
        {
            // 计算矩形高度和位置，与绘制函数一致
            int16_t rectH = 4 * 96;
            int16_t canvasH = PAPER_S3_HEIGHT;
            int16_t y = (canvasH - rectH) / 2;

            // 翻页
            // 翻页：上一项
            // if (zone == TouchZone::SEVEN_ONE)
            int16_t curx = msg->data.touch.x;
            int16_t cury = msg->data.touch.y;

            if (zone == TouchZone::SEVEN_ONE && curx > 10 && curx < 70 && cury > y + rectH - 52 + 5 && cury < y + rectH - 52 + 50)
            {
                int total_fonts = static_cast<int>(g_font_list.size());
                int pages = (total_fonts + 3 - 1) / 3; // 3 items per page
                int last_page = (pages > 0) ? (pages - 1) : 0;
                if (opt2 > 0)
                    --opt2;
                else
                    opt2 = last_page; // wrap to last page
                opt = 0;
                show_2nd_level_menu(g_canvas, true, 1);
            }
            // 翻页：下一项
            // if (zone == TouchZone::SEVEN_SIX)
            else if (zone == TouchZone::SEVEN_SIX && curx > 450 + 20 && curx < 450 + 80 && cury > y + rectH - 52 + 5 && cury < y + rectH - 52 + 50)
            {
                int total_fonts = static_cast<int>(g_font_list.size());
                int pages = (total_fonts + 3 - 1) / 3; // 3 items per page
                int last_page = (pages > 0) ? (pages - 1) : 0;
                if (opt2 < last_page)
                    ++opt2;
                else
                    opt2 = 0; // wrap to first page
                opt = 0;
                show_2nd_level_menu(g_canvas, true, 1);
            }
            else
            {
                // 选中
                //  data.touch.x/y
                // msg->data.touch.x, msg->data.touch.y
                //  只好硬来了
                if (cury > y + 100 + 0 * 80 - 4 && cury < y + 100 + 0 * 80 + 34 && curx > 40 && curx < 500)
                {
                    if (opt != 0)
                    {
                        opt = 0;
                    }
                }
                if (cury > y + 100 + 1 * 80 - 4 && cury < y + 100 + 1 * 80 + 34 && curx > 40 && curx < 500)
                {
                    if (opt != 1)
                    {
                        opt = 1;
                    }
                }
                if (cury > y + 100 + 2 * 80 - 4 && cury < y + 100 + 2 * 80 + 34 && curx > 40 && curx < 500)
                {
                    if (opt != 2)
                    {
                        opt = 2;
                    }
                }
                show_2nd_level_menu(g_canvas, true, 2);
            }

            // 确认字体，重置字体按钮
            // 确认字体
            if (cury > y + rectH - 55 && cury < y + rectH - 1 && curx > 96 && curx < 270)
            {
                int idx = opt2 * 3 + opt;
                if (idx >= 0 && idx < static_cast<int>(g_font_list.size()))
                {
                    // NOTE: replace `filepath` below with the actual member name in FontFileInfo
                    const char *fp = g_font_list[idx].path.c_str();
                    if (fp && strcmp(fp, g_config.fontset) != 0)
                    {
                        strncpy(g_config.fontset, fp, sizeof(g_config.fontset) - 1);
                        g_config.fontset[sizeof(g_config.fontset) - 1] = '\0';
                        config_save();

                        ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                        // rebuild font list and reload fonts
                        font_list_scan();
                        fontLoad();
                        // Trigger a force reindex so indexing uses the new font size
                        //                        requestForceReindex();
                    }
                }

                // 返回主菜单并切换状态
                show_main_menu(g_canvas, false, 0, 0, false);
                currentState_ = STATE_MAIN_MENU;
            }
            // 重置字体
            if (cury > y + rectH - 55 && cury < y + rectH - 1 && curx > 96 * 3 && curx < 96 * 3 + 170)
            {
                const char wanted[] = "/spiffs/lite.bin";
                if (strcmp(g_config.fontset, wanted) != 0)
                {
                    strncpy(g_config.fontset, wanted, sizeof(g_config.fontset) - 1);
                    g_config.fontset[sizeof(g_config.fontset) - 1] = '\0';
                    config_save();

                    ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                    // 根据fontset来刷新和重组font list
                    font_list_scan();
                    // 重新加载字体（请根据工程中实际的字体加载函数替换下面的调用）
                    fontLoad();
                    // Trigger a force reindex so indexing uses the new font size
                    //        requestForceReindex();
                }

                // 返回主菜单并切换状态
                show_main_menu(g_canvas, false, 0, 0, false);
                currentState_ = STATE_MAIN_MENU;
            }
        }

        // 2nd level menu common:
        // 计算当前菜单的矩形位置
        int16_t rectH_common = 4 * 96;
        if (main_2nd_level_menu_type == MAIN_2ND_MENU_DISPLAY_SETTING)
            rectH_common = 6 * 96;
        int16_t canvasH_common = PAPER_S3_HEIGHT;
        int16_t y_common = (canvasH_common - rectH_common) / 2;

        if (static_cast<int>(zone) < static_cast<int>(TouchZone::FOUR_ONE) ||
            static_cast<int>(zone) > static_cast<int>(TouchZone::SEVEN_SIX))
        {

            if (msg->data.touch.y > y_common + rectH_common + 10 || msg->data.touch.y < y_common - 10)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("二级菜单：触摸区域不在菜单矩形内，返回主菜单\n");
#endif
                // Clean those 2nd level input opts
                opt = 0;
                opt2 = 0;
                show_main_menu(g_canvas, false, 0, 0, false);
                currentState_ = STATE_MAIN_MENU;
            }
        }
    }
    break;

    case MSG_TOUCH_RELEASED:
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
        // 其他消息在二级菜单下暂时忽略
        break;
    }
}
