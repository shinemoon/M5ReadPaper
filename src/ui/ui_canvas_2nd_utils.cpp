#include "ui_canvas_2nd_utils.h"
#include "ui_canvas_utils.h"
#include "device/ui_display.h"
#include "device/file_manager.h"
#include "tasks/state_machine_task.h"
#include "ui/ui_canvas_utils.h"
#include "config/config_manager.h"
#include <cstring>

extern M5Canvas *g_canvas;
extern Main2ndLevelMenuType main_2nd_level_menu_type;
extern GlobalConfig g_config;

// Use global font list populated during filesystem init (PSRAM-backed)
extern std::vector<FontFileInfo, PSRAMAllocator<FontFileInfo>> g_font_list;

extern int8_t opt;
extern int16_t opt2;

void show_2nd_level_menu(M5Canvas *canvas, bool partial, int8_t refInd)
{
    M5Canvas *target = canvas ? canvas : g_canvas;
    if (!target)
        return;

    // Backup params for partial refresh
    int16_t p_x = 0;
    int16_t p_y = 0;
    int16_t p_w = 0;
    int16_t p_h = 0;

    // Rectangle dimensions
    const int16_t rectW = PAPER_S3_WIDTH;
    int16_t rectH = 4 * 96;

    switch (main_2nd_level_menu_type)
    {
    case Main2ndLevelMenuType::MAIN_2ND_MENU_DISPLAY_SETTING:
        rectH = 6 * 96;
        break;
    default:
        rectH = 4 * 96;
        break;
    }

    // Canvas origin and size discovery
    int16_t canvasW = target->width();
    int16_t canvasH = target->height();

    int16_t x = (canvasW - rectW) / 2;
    int16_t y = (canvasH - rectH) / 2;
    // Remember the shift on below info -8 y, +16 H
    p_x = x;
    p_y = y - 4;
    p_w = rectW;
    p_h = rectH + 16;

    // Draw white filled rectangle with thin border
    // target->drawRect(x, y, rectW, rectH, TFT_BLACK);
    target->fillRect(x, y - 2, rectW, rectH + 14, TFT_BLACK);
    target->fillRect(x, y, rectW, rectH + 10, TFT_WHITE);
    target->drawLine(x, y + 64, x + rectW, y + 64, TFT_BLACK);
    target->floodFill(x + 10, y + 10, TFT_WHITE);

    // 绘制四个角落的螺丝（使用已解析的 target canvas）
    drawScrew(target, x + 12, y + 12);
    drawScrew(target, x + 12, y + rectH - 4);
    drawScrew(target, x + rectW - 12, y + 12);
    drawScrew(target, x + rectW - 12, y + rectH - 4);

    // Optional title
    switch (main_2nd_level_menu_type)
    {
    case Main2ndLevelMenuType::MAIN_2ND_MENU_FONT_SETTING:
    {
        // Only refresh 'live' items;
        // 1: Paging the fonts
        // 2: Click and set
        if (refInd == 1)
        {
            p_x = 30;
            p_y = y + 16 + 42;
            p_w = 480;
            p_h = rectH - 110;
        }
        else if (refInd == 2)
        {
            p_x = 30;
            p_y = y + 16 + 42;
            p_w = 40;
            p_h = rectH - 110;
        }

        // opt: ind of selectd row
        // opt2: ind of current page
        // Title
        bin_font_print("字体设置选择", 32, 0, 540, 0, y + 16, false, target, TEXT_ALIGN_CENTER, 450);

        // Scan available fonts on SD (uses file_manager font scan API)
        // NOTE: adjust the call below to match your file_manager API if different.
        const int fonts_per_page = 3;

        // Use global font list populated by file_manager
        const auto &fonts = g_font_list;

        if (fonts.empty())
        {
            bin_font_print("未找到字体", 30, 0, 540, 0, y + 80, false, target, TEXT_ALIGN_CENTER, 450);
            break;
        }

        const int total_fonts = (int)fonts.size();
        const int pages = (total_fonts + fonts_per_page - 1) / fonts_per_page;
        if (opt2 < 0)
            opt2 = 0;
        if (opt2 >= pages)
            opt2 = pages - 1;

        // 选中字体做高亮底色
        // Ensure opt refers to a valid item on the current page
        int page_start = opt2 * fonts_per_page;
        int page_count = total_fonts - page_start;
        if (page_count <= 0)
        {
            opt = 0;
        }
        else
        {
            if (opt < 0)
                opt = 0;
            if (opt >= page_count)
                opt = page_count - 1;
        }
        // opt on behalf of the available one
        // target->fillRect(45, y + 100 - 4 + opt * 80, 450, 38, TFT_LIGHTGREY);
        target->fillRect(45, y + 100 - 4 + opt * 80, 10, 38, TFT_BLACK);
        // Show up to 3 font names for the current page, center-aligned under the title.
        for (int i = 0; i < fonts_per_page; ++i)
        {
            int idx = opt2 * fonts_per_page + i;
            if (idx >= total_fonts)
                break;
            int16_t y_pos = y + 100 + i * 80; // spaced to fit inside the white rect area

            // Compose display name: prefer "Family Style" if style is present
            const std::string &fam = fonts[idx].family_name;
            int8_t fontSize = fonts[idx].font_size;
            //            const std::string &sty = fonts[idx].style_name;
            std::string displayName = fam;
            // 系统字体标一个
            // 系统字体标一个
            if (!fonts[idx].path.empty() && strcmp(fonts[idx].path.c_str(), "/spiffs/lite.bin") == 0)
            {
                // 小圆点画在左侧，垂直居中于文字行
                const int16_t circle_x = 30;
                const int16_t circle_r = 6;
                int16_t circle_y = y_pos + 15; // 根据文字布局微调
                target->fillCircle(circle_x, circle_y, circle_r, TFT_BLACK);
                target->fillCircle(circle_x, circle_y, circle_r - 3, TFT_WHITE);
            }

            // if (!sty.empty())
            //{
            //    displayName += " ";
            //                 displayName += sty;
            //}

            bin_font_print(displayName.c_str(), 30, 0, 400, 45, y_pos, false, target, TEXT_ALIGN_CENTER, 360);

#if DBG_FILE_MANAGER
            // Debug: 检查 displayName 中的每个 UTF-8 codepoint 在当前加载字体中是否存在
            const uint8_t *p = (const uint8_t *)displayName.c_str();
            const uint8_t *pend = p + displayName.size();
            Serial.print("[FONT_LIST_DBG] displayName: ");
            Serial.println(displayName.c_str());
            Serial.print("[FONT_LIST_DBG] codepoints: ");
            while (p < pend && *p)
            {
                const uint8_t *prev = p;
                uint32_t cp = utf8_decode(p, pend);
                if (cp == 0)
                    break;
                Serial.printf("U+%04X ", cp);
                bool has = bin_font_has_glyph(cp);
                Serial.printf("->%s  ", has ? "found" : "missing");
                if (p <= prev)
                    break;
            }
            Serial.println();
#endif

            bin_font_print(std::to_string(static_cast<int>(fontSize)).c_str(), 28, 0, 540, 450, y_pos, false, target, TEXT_ALIGN_LEFT, 80);
        }

        // Pagination controls: "上一页" / "下一页"
        // Place buttons near the bottom of the white rect.
        int16_t btn_y = y + rectH - 52;
        //        g_canvas->fillRect(0, btn_y - 10, 540 * (((float)opt2 + 1.0f) / (float)pages), 4, TFT_BLACK);
        //       g_canvas->drawRect(0, btn_y - 11, 540, 6, TFT_BLACK);
        // Page indicator (1-based display)
        {
            int pageNum = opt2 + 1; // opt2 is zero-based
            char pageBuf[32];
            snprintf(pageBuf, sizeof(pageBuf), "%d / %d", pageNum, pages);
            bin_font_print(pageBuf, 20, 0, 540, 0, y + rectH - 78, false, target, TEXT_ALIGN_CENTER, 450);
        }

        // 左快进
        /*
        target->fillRect(0 + 35, btn_y + 8 + 16, 4, 4, TFT_BLACK);
        target->fillRect(0 + 43, btn_y + 4 + 16, 4, 12, TFT_BLACK);
        target->fillRect(0 + 51, btn_y + 0 + 16, 4, 20, TFT_BLACK);
        */
        target->fillTriangle(37, btn_y + 8 + 16 + 2, 51 + 4, btn_y + 0 + 16, 51 + 4, btn_y + 0 + 16 + 20, TFT_BLACK);

        // 中间长横线
        //        canvas->fillRect(0 + 160, btn_y + 40, 220, 4, TFT_BLACK);
        draw_button(target, 96, btn_y + 10, "确 认", true, false);      // SIX_TWO/THREE
        draw_button(target, 96 * 3, btn_y + 10, "重 置", false, false); // SIX_TWO/THREE

        // 右快进
        /*
        target->fillRect(0 + 450 + 35, btn_y + 0 + 16, 4, 20, TFT_BLACK);
        target->fillRect(0 + 450 + 43, btn_y + 4 + 16, 4, 12, TFT_BLACK);
        target->fillRect(0 + 450 + 51, btn_y + 8 + 16, 4, 4, TFT_BLACK);
        */
        target->fillTriangle(485, btn_y + 16, 501 + 4, btn_y + 8 + 18, 485, btn_y + 16 + 20, TFT_BLACK);

        // Page indicator centered
        //       char pageBuf[32];
        //        snprintf(pageBuf, sizeof(pageBuf), "%d / %d", font_page + 1, pages);
        //      bin_font_print(pageBuf, 24, 0, 540, 0, y + rectH - 40, false, g_canvas, TEXT_ALIGN_CENTER, 450);
    }
    break;
    case Main2ndLevelMenuType::MAIN_2ND_MENU_CLEAN_BOOKMARK:
        bin_font_print("确认清理", 32, 0, 540, 0, y + 16, false, target, TEXT_ALIGN_CENTER, 450);

        draw_button(target, 188, y + 80 + 32, "恢复出厂", true, false); // SIX_TWO/THREE

        bin_font_print("恢复出厂设置(书籍和图片不影响)", 28, 0, 540, 0, y + 32 + 80 + 60, true, target, TEXT_ALIGN_CENTER, 480);

        draw_button(target, 188, y + 32 + 80 + 50 + 80, "清理残存", true, false); // SIX_FOUR/FIVE

        bin_font_print("清理无对应书籍的残留缓存内容", 28, 0, 540, 0, y + 32 + 80 + 50 + 60 + 80, true, target, TEXT_ALIGN_CENTER, 480);

        break;
    case Main2ndLevelMenuType::MAIN_2ND_MENU_DISPLAY_SETTING:
    {
        // Got the calculated refresh window
        // refInd ==0 means whole new , don't do such 'small seg'
        if (refInd > 0)
        {
            p_x = 205;
            p_w = 460;
            p_h = 30;
            p_y = p_y + 120 + (refInd - 1) * 96;
        }
        bin_font_print("阅读显示设置", 32, 0, 540, 0, y + 16, false, target, TEXT_ALIGN_CENTER, 450);
        // Button
        draw_label(target, 40, y + 121, "默认方向", true); // FIVE_ONE/TWO
        const int16_t rotationRowY = y + 120;
        if (g_config.rotation == 2)
            // target->fillRect(210, rotationRowY - 4, 160, 36, TFT_LIGHTGREY);
            target->fillTriangle(210, rotationRowY + 4, 210, rotationRowY + 4 + 18, 210 + 12, rotationRowY + 4 + 9, TFT_BLACK);
        else
            // target->fillRect(360, rotationRowY - 4, 160, 36, TFT_LIGHTGREY);
            target->fillTriangle(360, rotationRowY + 4, 360, rotationRowY + 4 + 18, 360 + 12, rotationRowY + 4 + 9, TFT_BLACK);
        bin_font_print("手柄向上", 28, 0, 540, 230, rotationRowY, true, target, TEXT_ALIGN_LEFT, 150);
        bin_font_print("手柄向下", 28, 0, 540, 380, rotationRowY, true, target, TEXT_ALIGN_LEFT, 150);

        // Button
        draw_label(target, 40, y + 2 * 96 + 25, "翻页方式", true); // SIX_ONE/TWO

        const int16_t pageStyleRowY = y + 2 * 96 - 1 + 25;
        if (strcmp(g_config.pageStyle, "default") == 0)
            target->fillTriangle(210, pageStyleRowY + 4, 210, pageStyleRowY + 4 + 18, 210 + 12, pageStyleRowY + 4 + 9, TFT_BLACK);
        else
            target->fillTriangle(360, pageStyleRowY + 4, 360, pageStyleRowY + 4 + 18, 360 + 12, pageStyleRowY + 4 + 9, TFT_BLACK);
        bin_font_print("右手习惯", 28, 0, 540, 230, pageStyleRowY, true, target, TEXT_ALIGN_LEFT, 150);
        bin_font_print("左手习惯", 28, 0, 540, 380, pageStyleRowY, true, target, TEXT_ALIGN_LEFT, 150);
        // Button - 书签显示
        draw_label(target, 40, y + 3 * 96 + 25, "书签显示", true); // SIX_ONE/TWO

        const int16_t labelRowY = y + 3 * 96 - 1 + 25;
        if (strcmp(g_config.labelposition, "top") == 0)
            target->fillTriangle(410, labelRowY + 4, 410, labelRowY + 4 + 18, 410 + 12, labelRowY + 4 + 9, TFT_BLACK);
        else if (strcmp(g_config.labelposition, "middle") == 0)
            target->fillTriangle(310, labelRowY + 4, 310, labelRowY + 4 + 18, 310 + 12, labelRowY + 4 + 9, TFT_BLACK);
        else
            target->fillTriangle(210, labelRowY + 4, 210, labelRowY + 4 + 18, 210 + 12, labelRowY + 4 + 9, TFT_BLACK);

        bin_font_print("底部", 28, 0, 540, 230, labelRowY, true, target, TEXT_ALIGN_LEFT, 100);
        bin_font_print("中部", 28, 0, 540, 330, labelRowY, true, target, TEXT_ALIGN_LEFT, 100);
        bin_font_print("上部", 28, 0, 540, 430, labelRowY, true, target, TEXT_ALIGN_LEFT, 100);

        // Button - 书签主题
        draw_label(target, 40, y + 4 * 96 + 25, "书签主题", true); // SEVEN_ONE/TWO

        const int16_t themeRowY = y + 4 * 96 - 1 + 25;
        if (strcmp(g_config.marktheme, "light") == 0)
            target->fillTriangle(310, themeRowY + 4, 310, themeRowY + 4 + 18, 310 + 12, themeRowY + 4 + 9, TFT_BLACK);
        else if (strcmp(g_config.marktheme, "random") == 0)
            target->fillTriangle(410, themeRowY + 4, 410, themeRowY + 4 + 18, 410 + 12, themeRowY + 4 + 9, TFT_BLACK);
        else
            target->fillTriangle(210, themeRowY + 4, 210, themeRowY + 4 + 18, 210 + 12, themeRowY + 4 + 9, TFT_BLACK);

        bin_font_print("深色", 28, 0, 540, 230, themeRowY, true, target, TEXT_ALIGN_LEFT, 100);
        bin_font_print("浅色", 28, 0, 540, 330, themeRowY, true, target, TEXT_ALIGN_LEFT, 100);
        bin_font_print("随机", 28, 0, 540, 430, themeRowY, true, target, TEXT_ALIGN_LEFT, 100);

        // Button - 通用壁纸
        draw_label(target, 40, y + 5 * 96 + 25, "通用壁纸", true); // EIGHT_ONE/TWO

        const int16_t wallpaperRowY = y + 5 * 96 - 1 + 25;
        if (g_config.defaultlock)
            target->fillTriangle(210, wallpaperRowY + 4, 210, wallpaperRowY + 4 + 18, 210 + 12, wallpaperRowY + 4 + 9, TFT_BLACK);
        else
            target->fillTriangle(360, wallpaperRowY + 4, 360, wallpaperRowY + 4 + 18, 360 + 12, wallpaperRowY + 4 + 9, TFT_BLACK);

        bin_font_print("默认壁纸", 28, 0, 540, 230, wallpaperRowY, true, target, TEXT_ALIGN_LEFT, 150);
        bin_font_print("随机壁纸", 28, 0, 540, 380, wallpaperRowY, true, target, TEXT_ALIGN_LEFT, 150);

        break;
    }
    case Main2ndLevelMenuType::MAIN_2ND_MENU_CONNECT_METHOD:
    {
        bin_font_print("连接方式", 36, 0, 540, 0, y + 16, false, target, TEXT_ALIGN_CENTER, 540);
        // Show only the wireless option centered in the rect.
        // The wired UI is intentionally hidden; its touch region is moved to the
        // top-right 60x60 area of the center rectangle (handled in touch logic).
        int16_t btn_cx = x + rectW / 2 - 82; // left x so button is centered horizontally
        int16_t btn_cy = y + rectH / 2;      // center vertically inside the white rect

        draw_button(target, btn_cx, btn_cy, "无线连接", true);
        //        bin_font_print("请按照说明连接WIFI", 28, 0, 540, 0, btn_cy + 45, false, g_canvas, TEXT_ALIGN_CENTER, 540);

        // Draw MSC indicator
        target->fillTriangle(540 - 60, y + 64, 540, y + 64, 540, y + 64 + 60, TFT_BLACK);
        // Logo
        target->fillRect(540 - 40, y + 64 + 10, 25, 17, TFT_WHITE);
        target->fillRect(540 - 38, y + 64 + 12, 21, 13, TFT_BLACK);
        target->fillCircle(540 - 33, y + 64 + 18, 2, TFT_WHITE);
        target->fillCircle(540 - 23, y + 64 + 18, 2, TFT_WHITE);

        target->fillTriangle(540 - 40, y + 64 + 25, 540 - 7, y + 64 + 25, 540 - 7, y + 64 + 52, TFT_WHITE);
        target->fillArc(540 - 25, y + 64 + 36, 0, 8, 240, 390, TFT_BLACK);

        target->drawWideLine(540 - 60, y + 64, 540, y + 64 + 60, 2, TFT_BLACK);
    }
    break;
    default:
        break;
    }

    // If using global display, push to screen
    if (!canvas && g_canvas)
    {
        M5.Display.powerSaveOff();
        g_canvas->pushSprite(0, 0);
        M5.Display.waitDisplay();
        M5.Display.powerSaveOn();
    }
    else
    {
        // For user-supplied canvas, caller is responsible for pushing
        if (partial)
        {
            bin_font_flush_canvas(false, false, false, NOEFFECT, p_x, p_y, p_w, p_h);
            // Print partial refresh rectangle for debugging
            // Serial.printf("p_x=%d p_y=%d p_w=%d p_h=%d\n", p_x, p_y, p_w, p_h);
        }
        else
            bin_font_flush_canvas();
    }
}
