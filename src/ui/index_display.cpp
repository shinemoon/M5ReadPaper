#include "index_display.h"
#include "ui_canvas_utils.h"
#include "text/tags_handle.h"
#include "text/font_buffer.h"
#include "current_book.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "globals.h"
#include <vector>

extern M5Canvas *g_canvas;

// Draw a left-side 360x800 tag list, 10 rows (each 80px high).
// Each row shows: <摘要> <百分比>
void show_tag_ui(M5Canvas *canvas)
{
    M5Canvas *target = canvas ? canvas : g_canvas;
    if (!target)
        return;

    // 清理TOC和书名缓存，避免索引期间缓存与字体文件状态不一致导致乱码
    // 这些缓存在下次需要时会自动重建
    clearTocCache();
    clearBookNameCache();

    // Left area
    const int16_t x = 0;
    const int16_t y = 0;
    const int16_t deltay = 32; // leave small top margin
    const int16_t w = 450;
    const int16_t h = 960;
    const int rows = 10;
    const int row_h = h * 0.9 / rows; // 80

    // background
    target->fillRect(x, y, w, h, TFT_WHITE);
    target->fillRect(x + w, y, 540 - w, h, TFT_BLACK);

    // Load tags for current book
    std::vector<TagEntry> tags;
    if (g_current_book)
    {
        tags = loadTagsForFile(g_current_book->filePath());
    }

    // Draw up to 10 entries
    for (int i = 0; i < rows; ++i)
    {
        int16_t ry = y + i * row_h + deltay; // inner padding
        if (i < (int)tags.size())
        {
            const TagEntry &te = tags[i];
            // determine if this tag's position is already indexed
            bool available = true;
            if (g_current_book)
            {
                available = (g_current_book->isIndexingComplete() || te.position <= g_current_book->getIndexingCurrentPos());
            }

            // preview left, percentage right
            const char *preview = te.preview.c_str();
            char pctbuf[32];
            int pct = (int)(te.percentage + 0.5f);
            snprintf(pctbuf, sizeof(pctbuf), " %d%%", pct);

            ry = ry + 50;
            // choose color index: 0 for normal, 5 for not-yet-indexed
            // int text_color = available ? 0 : 3;
            int text_color = 0;

            // preview area: allow up to ~260px width
            bin_font_print(preview, 28, 0, 350, x + 48, ry, true, target, TEXT_ALIGN_LEFT, 350);
            // percentage aligned to right column
            bin_font_print(pctbuf, 24, text_color, 120, x + 350, ry, true, target, TEXT_ALIGN_LEFT, 120);

            if (i == 0)
                bin_font_print("Auto", 14, 0, 60, 17, ry - 5, true, target, TEXT_ALIGN_LEFT, 60);
            else
            {
                if (available)
                    canvas->drawCircle(x + 20, ry + 12, 3, TFT_BLACK);
            }
            //            canvas->drawLine(20, ry + 35, 350, ry + 35, TFT_BLACK);
            //           canvas->drawLine(20, ry + 38, 350, ry + 38, TFT_BLACK);
        }
        else
        {
            // empty row — leave blank
        }
    }

    // Screw
    drawScrew(canvas, 20, 20);
    drawScrew(canvas, 520, 20);
    drawScrew(canvas, 20, 940);
    drawScrew(canvas, 520, 940);

    // Line
    canvas->drawLine(450, 40, 540, 40, TFT_WHITE);
    canvas->drawLine(450, 920, 540, 920, TFT_WHITE);
    canvas->drawLine(0, 40, 450, 40, TFT_BLACK);
    canvas->drawLine(0, 920, 450, 920, TFT_BLACK);

    // Switcher
    canvas->drawLine(225, 0, 235, 40, TFT_BLACK);
    canvas->drawLine(230, 0, 240, 40, TFT_BLACK);
    canvas->floodFill(240, 10, TFT_LIGHTGRAY);

    bin_font_print("书签", 24, 0, 200, 270, 8, false, canvas);
    if (g_current_book->isIndexed())
    {
        bin_font_print("目录", 24, 0, 200, 140, 8, false, canvas);
    }

    // Icon
    canvas->fillRect(450 + 35, 40, 20, 35, TFT_LIGHTGRAY);
    canvas->fillTriangle(450 + 35, 75, 460 + 35, 70, 470 + 35, 75, TFT_BLACK);
    canvas->fillCircle(460 + 35, 50, 3, TFT_BLACK);

    // push to display if using global canvas
    if (!canvas && g_canvas)
    {
        M5.Display.powerSaveOff();
        g_canvas->pushSprite(0, 0);
        M5.Display.waitDisplay();
        M5.Display.powerSaveOn();
    }
    else
    {
        bin_font_flush_canvas();
    }
}
