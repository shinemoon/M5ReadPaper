#include <string>
#include "ui_lock_screen.h"
#include <M5Unified.h>
#include "../include/readpaper.h"
#include "../text/bin_font_print.h"
#include "../device/ui_display.h"
#include "../text/text_handle.h"
#include "../text/book_handle.h"
#include "ui_canvas_image.h"
#include "../test/per_file_debug.h"
#include <SPIFFS.h>
#include <SD.h>
#include "../SD/SDWrapper.h"
#include "tasks/state_machine_task.h"
#include "config/config_manager.h"
#include <vector>
#include "device/efficient_file_scanner.h"

#include "current_book.h"
extern M5Canvas *g_canvas;
extern GlobalConfig g_config;

namespace
{
    struct LockImageCache
    {
        bool valid = false;
        bool sd_ready = false;
        std::vector<String> candidates;
    };

    LockImageCache g_lock_image_cache;

    inline void reset_lock_image_cache()
    {
        g_lock_image_cache.candidates.clear();
        g_lock_image_cache.valid = false;
    }

    bool ensure_lock_image_candidates(const char *dirPath)
    {
        if (g_lock_image_cache.valid)
            return true;

        if (!g_lock_image_cache.sd_ready)
        {
            if (!SDW::SD.begin())
                return false;
            g_lock_image_cache.sd_ready = true;
        }

        if (!SDW::SD.exists(dirPath))
        {
            g_lock_image_cache.valid = true;
            return true;
        }

        reset_lock_image_cache();

        std::vector<FileInfo> files = EfficientFileScanner::scanDirectory(std::string(dirPath));
        auto make_sd_prefixed = [](const std::string &p) -> String
        {
            if (p.rfind("/sd", 0) == 0)
                return String(p.c_str());
            if (p.rfind("/", 0) == 0)
                return String((std::string("/sd") + p).c_str());
            return String((std::string("/sd/") + p).c_str());
        };

        for (const auto &fi : files)
        {
            if (fi.isDirectory)
                continue;
            std::string lname = fi.name;
            for (auto &c : lname)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');

            const bool looks_like_image =
                lname.size() >= 4 &&
                (lname.find(".png") != std::string::npos || lname.find(".jpg") != std::string::npos ||
                 lname.find(".jpeg") != std::string::npos || lname.find(".bmp") != std::string::npos);

            if (!looks_like_image)
                continue;

            String fullPath = make_sd_prefixed(fi.path);
            g_lock_image_cache.candidates.push_back(fullPath);
        }

        g_lock_image_cache.valid = true;
        return true;
    }

    inline String extract_basename_no_ext(const String &path)
    {
        String fn = path;
        int ls = fn.lastIndexOf('/');
        if (ls >= 0)
            fn = fn.substring(ls + 1);
        int dot = fn.lastIndexOf('.');
        return (dot >= 0) ? fn.substring(0, dot) : fn;
    }

    inline String extract_filename(const String &path)
    {
        String fn = path;
        int ls = fn.lastIndexOf('/');
        return (ls >= 0) ? fn.substring(ls + 1) : fn;
    }
}

static bool push_random_sd_image_if_available(const char *dirPath, int x, int y)
{
    if (!ensure_lock_image_candidates(dirPath))
        return false;

    const auto &candidates = g_lock_image_cache.candidates;
    if (candidates.empty())
        return false;

    String book_base;
    if (g_current_book)
    {
        std::string bp = getBookFilePath(g_current_book);
        if (!bp.empty())
        {
            String full(bp.c_str());
            int ls = full.lastIndexOf('/');
            String fname = (ls >= 0) ? full.substring(ls + 1) : full;
            int dot = fname.lastIndexOf('.');
            book_base = (dot >= 0) ? fname.substring(0, dot) : fname;
        }
    }

    if (book_base.length() > 0)
    {
        for (const String &p : candidates)
        {
            if (extract_basename_no_ext(p) == book_base)
            {
                ui_push_image_to_canvas(p.c_str(), x, y, nullptr, true);
                return true;
            }
        }

        // Fuzzy match: if no exact same-name image found, try to find a candidate
        // whose basename is a substring of the book basename (case-insensitive).
        // This supports series like "<书名> 01.txt", "<书名> 02.txt" sharing one cover image.
        String book_base_lower = book_base;
        book_base_lower.toLowerCase();
        for (const String &p : candidates)
        {
            String img_base = extract_basename_no_ext(p);
            img_base.toLowerCase();
            if (img_base.length() > 0 && book_base_lower.indexOf(img_base) >= 0)
            {
                ui_push_image_to_canvas(p.c_str(), x, y, nullptr, true);
                return true;
            }
        }
    }

    if (g_config.defaultlock)
    {
        for (const String &p : candidates)
        {
            String file_with_ext = extract_filename(p);
            file_with_ext.toLowerCase();
            if (file_with_ext == "default.png")
            {
                ui_push_image_to_canvas(p.c_str(), x, y, nullptr, true);
                return true;
            }
        }
        return false;
    }

#if DBG_UI_IMAGE
    Serial.printf("[LOCKSCREEN] FreeHeap before picking image: %u bytes, candidates=%d\n", (unsigned)ESP.getFreeHeap(), (int)candidates.size());
#endif
    randomSeed(millis());
    int idx = random((int)candidates.size());
    String pick = candidates[idx];
    ui_push_image_to_canvas(pick.c_str(), x, y, nullptr, true);
    return true;
}

// 绘制书名与页码的腰封（Name banner）
static void draw_name_banner(M5Canvas *canvas, const char *name_with_page, int32_t basey, bool invert = false, int curp = 0, int totalp = 0)
{
    canvas->fillRect(0, basey + 2, 540, 56, invert ? TFT_BLACK : TFT_LIGHTGREY);
    canvas->fillRect(0, basey + 5, 540, 50, invert ? TFT_BLACK : TFT_WHITE);

    // Protect against divide-by-zero and clamp progress width to [0,540]
    int progress_width = 0;
    if (totalp > 0)
    {
        // use 64-bit intermediate just in case, then clamp
        int64_t pw = (int64_t)540 * (int64_t)curp / (int64_t)totalp;
        if (pw < 0)
            pw = 0;
        if (pw > 540)
            pw = 540;
        progress_width = (int)pw;
    }

    canvas->fillRect(0, basey + 12, progress_width, 36, 0xDDD6);

    canvas->drawLine(0, basey + 10, 540, basey + 10, invert ? TFT_WHITE : TFT_BLACK);
    canvas->drawLine(0, basey + 50, 540, basey + 50, invert ? TFT_WHITE : TFT_BLACK);

    bin_font_print(name_with_page, 24, invert ? TFT_WHITE : TFT_BLACK, 480, 30, basey + 20 - 1, false, canvas, TEXT_ALIGN_CENTER, 440, g_current_book ? g_current_book->getKeepOrg() : false); // 0.7f * 30 = 21
    drawScrew(g_canvas, 20, basey + 30);
    drawScrew(g_canvas, 520, basey + 30);
}

// 绘制左侧垂直条及竖排文摘（vertical banner）
static void draw_vertical_banner(M5Canvas *canvas, const std::string &digest, int basex = 0, int basew = 160, int offsetx = 402, int bg = TFT_BLACK, int fg = TFT_LIGHTGREY)
{
    // Draw a black vertical strip and white separator lines

    // Base
    canvas->fillRect(basex, 0, basew, 940, bg);
    // Side

    canvas->drawLine(basex + 5, 80, basex + 5, 935, fg);
    canvas->drawLine(basew + basex - 5, 80, basew + basex - 5, 935, fg);
    canvas->drawLine(basex + 5, 935, basex + basew - 5, 935, fg);

    // canvas->fillCircle(basex + basew / 2, 820, 15, TFT_WHITE);
    // canvas->drawWideLine(basex, 820, basex+basew, 820, 2.0, fg);
    // canvas->drawCircle(basex + basew / 2, 910, 20, fg);

    // Render digest vertically: area_width=900 (large value), vertical=true (last param true)
    // bin_font_print(digest.c_str(), 28, fg, 900, 120, 960 - basew - offsetx, false, canvas, TEXT_ALIGN_LEFT, 900, false, g_current_book->getKeepOrg(), true);
    bin_font_print(digest.c_str(), 28, fg, 900, 120, 960 - basew - offsetx, false, canvas, TEXT_ALIGN_LEFT, 900, true, true, true);

    // canvas->fillRect(basex, 760, basew, 100, TFT_BLACK);
    canvas->fillRect(basex, 0, basew, 80, TFT_BLACK);
    // RING
    canvas->fillCircle(basex + basew / 2, 40, 15, TFT_WHITE);
    canvas->drawWideLine(basex, 40, basex + basew, 40, 2.0, TFT_WHITE);
    canvas->drawLine(basex, 5, basex + basew, 5, TFT_WHITE);
    canvas->drawCircle(basex + basew / 2, 40, 20, TFT_WHITE);
}

void show_start_screen(const char *subtitle)
{
    // Skip
    // 使用 canvas 方式推送图片 - fast
    //    ui_push_image_to_canvas("/spiffs/screen.png", 0, 0);
    //    bin_font_flush_canvas(false,false);

    bin_font_clear_canvas();
    // ui_push_image_to_canvas("/spiffs/start.png", 110, 390);
    ui_push_image_to_canvas("/spiffs/start.png", 0, 0);
    bin_font_flush_canvas(false, false, true);
    delay(500);
    M5.Display.waitDisplay();
    // If subtitle provided, draw centered below image using native drawString
    // (at this early stage fonts may not be loaded, so avoid bin_font_print)
    if (subtitle && subtitle[0] != '\0')
    {
        M5.Display.setTextColor(0x02);
        M5.Display.setTextSize(1.2);
        M5.Display.setTextDatum(MC_DATUM); // center align
        M5.Display.drawString(String(subtitle), PAPER_S3_WIDTH - 80, 920);
        M5.Display.waitDisplay();
    }

    // 在屏幕底部显示 /version 文件第三行（如果存在），使用 TextSize(2)
    std::string ver;
    if (SPIFFS.exists("/version"))
    {
        File vf = SPIFFS.open("/version", "r");
        if (vf)
        {
            std::string curLine;
            std::string lastNonEmpty;
            int lineNo = 0;
            while (vf.available())
            {
                char c = vf.read();
                if (c == '\r')
                    continue;
                if (c == '\n')
                {
                    lineNo++;
                    if (lineNo == 3)
                    {
                        ver = curLine;
                        break;
                    }
                    if (!curLine.empty())
                        lastNonEmpty = curLine;
                    curLine.clear();
                }
                else
                {
                    curLine.push_back(c);
                }
            }

            // handle case where file ends without trailing newline
            if (ver.empty())
            {
                if (!curLine.empty())
                {
                    lineNo++;
                    if (lineNo == 3)
                        ver = curLine;
                    if (ver.empty() && !lastNonEmpty.empty())
                        ver = lastNonEmpty;
                }
                else if (!lastNonEmpty.empty())
                {
                    ver = lastNonEmpty;
                }
            }

            vf.close();

            // trim whitespace
            if (!ver.empty())
            {
                size_t st = 0;
                while (st < ver.size() && isspace((unsigned char)ver[st]))
                    st++;
                size_t ed = ver.size();
                while (ed > st && isspace((unsigned char)ver[ed - 1]))
                    ed--;
                if (ed > st)
                    ver = ver.substr(st, ed - st);
                else
                    ver.clear();
            }
        }
    }

    if (!ver.empty())
    {
        M5.Display.setTextColor(0x02);
        M5.Display.setTextSize(2);
        M5.Display.setTextDatum(MC_DATUM);
        M5.Display.drawString(String(ver.c_str()), PAPER_S3_WIDTH / 2, PAPER_S3_HEIGHT / 2 + 136);
        M5.Display.waitDisplay();
    }
}

void show_lockscreen(int16_t area_width, int16_t area_height, float font_size, const char *text, bool isshutdown, const char *labelpos)
{
#if DBG_POWERMGT
    Serial.println("[POWER] 10分钟无操作，自动关机");
#endif

    // Lazy overrideing from global now..
    // Override labelpos from global config when configured
    // Use a local copy to ensure lifetime if you prefer safety
    static std::string labelpos_copy; // or non-static if you prefer per-call storage
    if (g_config.labelposition != nullptr && g_config.labelposition[0] != '\0')
    {
        labelpos_copy = g_config.labelposition;
        labelpos = labelpos_copy.c_str();
    }

    // Background - 始终显示screen.png
    /* To support transparent pic
    //    g_canvas->clear(); // skip clearn back
    // For transparent lockscreen
    //    if (g_current_book)
    //       g_current_book->renderCurrentPage(0, g_canvas);
    // 锁屏图标
    */
    if (getCurrentSystemState() != STATE_IDLE)
    {
        ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
        M5.Display.waitDisplay();
    }

    // Try to show a random SD image from /image; fallback to /spiffs/screen.png
    if (!push_random_sd_image_if_available("/image", 0, 0))
    {
        if (g_current_book && g_current_book->getVerticalText())
            ui_push_image_to_canvas("/spiffs/screen.png", 0, 0, nullptr, true);
        else
            ui_push_image_to_canvas("/spiffs/screenH.png", 0, 0, nullptr, true);
    }

    g_canvas->drawRect(0, 0, 540, 960, TFT_WHITE);
    //    delay(100);
    // Use provided text (or default) instead of reading shutdown.txt
    if (text == nullptr)
    {
        // In failulre , even can't write font
        //     ui_push_image_to_canvas("/spiffs/wait.png", 240, 450);
    }
    else
    {
        // 如果当前书籍的showlabel为false，只显示screen.png，跳过其他内容
        if (g_current_book == nullptr || !g_current_book->getShowLabel())
        {
            // put on top-right conner
            g_canvas->fillTriangle(480, 0, 540, 0, 540, 60, 0x0005);
            g_canvas->drawWideLine(480, 0, 540, 60, 0.5, TFT_WHITE);

            if (isshutdown)
            {
                ui_push_image_to_canvas("/spiffs/power-icon.png", 508, 0);
            }
            else
            {
                ui_push_image_to_canvas("/spiffs/lock-icon.png", 508, 0);
            }

            bin_font_flush_canvas(false, false, true);
            M5.Display.waitDisplay();

            return; // 提前返回，跳过后续的图片和文字
        }

        //        std::string display_text = text ? std::string(text) : std::string("坐看云起时");
        //       bin_font_print(display_text.c_str(), 30, 2, 540, 60, 900, false); // 1.0f * 30 = 30
        // 腰封 - 只有在showlabel为true时才显示完整内容
        {
            // 主题颜色变量（集中定义，便于主题替换）
            // 三角形/装饰色
            uint16_t theme_tri_color = 0x0005;
            // 垂直条（vertical banner）背景/前景
            uint16_t theme_vertical_bg = 0x2222;
            uint16_t theme_vertical_fg = TFT_WHITE;
            // 腰封/条带背景与文字颜色
            uint16_t theme_strip_bg = 0x2222;
            uint16_t theme_strip_fg = TFT_WHITE;

            std::string curtheme = g_config.marktheme; // 书签主题: 'dark' / 'light' / 'random'
            bool darktheme;
            if (curtheme == "dark")
            {
                darktheme = true;
            }
            else if (curtheme == "light")
            {
                darktheme = false;
            }
            else if (curtheme == "random")
            {
                randomSeed(millis());
                darktheme = (random(2) == 0);
            }
            else
            {
                // Default to light if unknown
                darktheme = false;
            }

            if (darktheme)
            {
                // DARK
                //  三角形/装饰色
                theme_tri_color = 0x0005;
                // 垂直条（vertical banner）背景/前景
                theme_vertical_bg = 0x2222;
                theme_vertical_fg = TFT_WHITE;
                // 腰封/条带背景与文字颜色
                theme_strip_bg = 0x2222;
                theme_strip_fg = TFT_WHITE;
            }
            else
            {
                //  三角形/装饰色
                theme_tri_color = 0x00aa;
                // 垂直条（vertical banner）背景/前景
                theme_vertical_bg = TFT_WHITE;
                theme_vertical_fg = TFT_BLACK;
                // 腰封/条带背景与文字颜色
                theme_strip_bg = TFT_WHITE;
                theme_strip_fg = TFT_BLACK;
            }

            // 根据 labelpos 调整整体腰封 Y 偏移
            int deltaY = 0; // middle 默认
            if (labelpos != nullptr)
            {
                // For horizon
                if (strcmp(labelpos, "top") == 0)
                {
                    deltaY = -450;
                }
                else if (strcmp(labelpos, "default") == 0)
                {
                    deltaY = 220;
                }
                else if (strcmp(labelpos, "middle") == 0)
                {
                    deltaY = 0;
                }

                // decide the triangle
                if (g_current_book && g_current_book->getVerticalText() && strcmp(labelpos, "default") == 0)
                {
                    // put on bottom-right conner
                    g_canvas->fillTriangle(0, 0, 60, 0, 0, 60, theme_tri_color);
                    g_canvas->drawWideLine(60, 0, 0, 60, 0.5, TFT_WHITE);

                    if (isshutdown)
                    {
                        ui_push_image_to_canvas("/spiffs/power-icon.png", 1, 4);
                    }
                    else
                    {
                        ui_push_image_to_canvas("/spiffs/lock-icon.png", 1, 4);
                    }
                }
                else if (strcmp(labelpos, "top") == 0 && (g_current_book && !g_current_book->getVerticalText()))
                {
                    // put on bottom-right conner
                    g_canvas->fillTriangle(480, 960, 540, 960, 540, 900, theme_tri_color);
                    g_canvas->drawWideLine(480, 960, 540, 900, 0.5, TFT_WHITE);

                    if (isshutdown)
                    {
                        ui_push_image_to_canvas("/spiffs/power-icon.png", 508, 960 - 35);
                    }
                    else
                    {
                        ui_push_image_to_canvas("/spiffs/lock-icon.png", 508, 960 - 35);
                    }
                }
                else
                {
                    // put on top-right conner
                    g_canvas->fillTriangle(480, 0, 540, 0, 540, 60, theme_tri_color);
                    g_canvas->drawWideLine(480, 0, 540, 60, 0.5, TFT_WHITE);

                    if (isshutdown)
                    {
                        ui_push_image_to_canvas("/spiffs/power-icon.png", 508, 0);
                    }
                    else
                    {
                        ui_push_image_to_canvas("/spiffs/lock-icon.png", 508, 0);
                    }
                }
            }
            // 当前书名
            std::string path = getBookFilePath(g_current_book);
            // 去掉路径，只保留文件名
            size_t pos = path.find_last_of("/\\");
            std::string name = (pos == std::string::npos) ? path : path.substr(pos + 1);
            // 去掉扩展名
            size_t dot = name.find_last_of('.');
            if (dot != std::string::npos)
                name = name.substr(0, dot);
            // 拼接页码信息
            size_t cur_page = 1, total_page = 1;
            if (g_current_book)
            {
                cur_page = g_current_book->getCurrentPageIndex() + 1;
                total_page = g_current_book->getTotalPages();
            }
            char name_with_page[128];
            {
                std::string shortname = name;
                // UTF-8 safe truncate by Unicode codepoints (won't cut a multi-byte char)
                auto utf8_truncate = [](const std::string &s, size_t max_chars) -> std::string
                {
                    std::string out;
                    out.reserve(s.size());
                    const unsigned char *data = (const unsigned char *)s.data();
                    size_t i = 0, n = s.size(), chars = 0;
                    while (i < n && chars < max_chars)
                    {
                        unsigned char c = data[i];
                        size_t clen;
                        if (c < 0x80)
                            clen = 1;
                        else if ((c & 0xE0) == 0xC0)
                            clen = 2;
                        else if ((c & 0xF0) == 0xE0)
                            clen = 3;
                        else if ((c & 0xF8) == 0xF0)
                            clen = 4;
                        else
                            break; // invalid start byte -> stop
                        if (i + clen > n)
                            break; // truncated sequence at end -> stop
                        out.append((const char *)&data[i], clen);
                        i += clen;
                        ++chars;
                    }
                    return out;
                };

                // Limit to 18 characters (Unicode-aware)
                std::string display_name = utf8_truncate(shortname, 22);
                // snprintf(name_with_page, sizeof(name_with_page), "%s | %zu/%zu", display_name.c_str(), cur_page, total_page);
                snprintf(name_with_page, sizeof(name_with_page), "%s", display_name.c_str());
            }

            // If vertical text mode, render a left-side vertical label area instead of the frame image
            if (g_current_book && g_current_book->getVerticalText())
            {
                // Draw vertical banner (left strip and vertical digest)
                {
                    int vb_basex = 200; // default
                    if (labelpos)
                    {
                        if (strcmp(labelpos, "top") == 0)
                            vb_basex = 10;
                        else if (strcmp(labelpos, "middle") == 0)
                            vb_basex = 200;
                        else if (strcmp(labelpos, "default") == 0)
                            vb_basex = 370;
                    }
                    draw_vertical_banner(g_canvas, g_current_book->getCurrentDigest(), vb_basex, 160, 402 + vb_basex, theme_vertical_bg, theme_vertical_fg);
                }

                // Name banner
                int32_t basey = 820;
                draw_name_banner(g_canvas, name_with_page, basey, !darktheme, cur_page, total_page);
            }
            else
            {
                /*
                //                ui_push_image_to_canvas("/spiffs/frame.png", 0, 380 + BOOKMARKOFFSET + deltaY);
                bin_font_print(name_with_page, 21, 4, 540, 10, 590 + BOOKMARKOFFSET + deltaY, false); // 0.7f * 30 = 21
                // 文摘刷新
                bin_font_print(g_current_book->getCurrentDigest().c_str(), 26, 2, 540, 50, 430 + BOOKMARKOFFSET + deltaY, false); // 0.85f * 30 = 25.5 ≈ 26
                */
                int basey = 382 + BOOKMARKOFFSET + deltaY;
                int baseh = 160;

                // Draw a themed vertical strip and separator lines
                g_canvas->fillRect(0, basey, 60, baseh, TFT_BLACK);
                g_canvas->fillRect(0, basey, 540, baseh, theme_strip_bg);
                g_canvas->drawRect(0, basey, 540, baseh, TFT_BLACK);

                bin_font_print(g_current_book->getCurrentDigest().c_str(), 26, theme_strip_fg, 540, 120, basey + 20, false, g_canvas, TEXT_ALIGN_LEFT, 0, false, true);

                // head
                g_canvas->fillRect(0, basey, 60, baseh, TFT_BLACK);
                g_canvas->drawLine(60, basey + 5, 540, basey + 5, theme_strip_fg);
                g_canvas->drawLine(60, basey + baseh - 5, 540, baseh + basey - 5, theme_strip_fg);

                g_canvas->drawLine(0, basey + 5, 60, basey + 5, TFT_WHITE);
                g_canvas->drawLine(0, basey + baseh - 5, 60, baseh + basey - 5, TFT_WHITE);

                g_canvas->drawCircle(30, basey + baseh / 2, 20, TFT_WHITE);
                g_canvas->fillCircle(30, basey + baseh / 2, 15, TFT_WHITE);
                g_canvas->drawWideLine(0, basey + baseh / 2, 60, basey + baseh / 2, 1.5, TFT_WHITE);

                draw_name_banner(g_canvas, name_with_page, basey + 162, false, cur_page, total_page);
            }
        }
    }
    bin_font_flush_canvas(false, false, true); // quality will be reset by the bin flush way!
};

void lockscreen_image_cache_invalidate()
{
    reset_lock_image_cache();
    g_lock_image_cache.sd_ready = false;
}