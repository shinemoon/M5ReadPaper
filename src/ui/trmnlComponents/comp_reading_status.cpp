#include "comp_reading_status.h"
#include "readpaper.h"
#include "globals.h"
#include "text/bin_font_print.h"
#include "test/per_file_debug.h"
#include "current_book.h"
#include "ui/toc_display.h"
#include <string>
#include <algorithm>

// ──────────────────────────────────────────────
// render_reading_status_component
// ──────────────────────────────────────────────
void render_reading_status_component(JsonObject component)
{
    int pos_x = 0, pos_y = 0, a_w = 0, a_h = 0;
    if (component.containsKey("position"))
    {
        JsonObject position = component["position"].as<JsonObject>();
        pos_x = position["x"] | 0;
        pos_y = position["y"] | 0;
    }
    JsonObject areaSize = component["size"].as<JsonObject>();
    float cell_w = areaSize["width"].as<float>(); if (cell_w < 0.01f) cell_w = 1.0f;
    float cell_h = areaSize["height"].as<float>(); if (cell_h < 0.01f) cell_h = 1.0f;

    int fontSize = 24, textColor = 0, xOffset = 0, yOffset = 0;
    const char *alignStr = "left";
    uint8_t align = 0;
    if (component.containsKey("config"))
    {
        JsonObject config = component["config"].as<JsonObject>();
        fontSize = config["fontSize"] | 24;
        textColor = config["textColor"] | 0;
        alignStr = config["align"] | "left";
        xOffset = config["xOffset"] | 0;
        yOffset = config["yOffset"] | 0;
        if (strcmp(alignStr, "center") == 0) align = 1;
        else if (strcmp(alignStr, "right") == 0) align = 2;
    }

    const int CELL_WIDTH = 60, CELL_HEIGHT = 60;
    int16_t x = pos_x * CELL_WIDTH + 20 + xOffset;
    int16_t y = pos_y * CELL_HEIGHT + yOffset;
    a_w = (int)(cell_w * CELL_WIDTH) - 40;
    a_h = (int)(cell_h * CELL_HEIGHT);

    // 获取当前书名与章节名
    std::string book_name;
    std::string chapter_name;
    if (g_current_book && g_current_book->isOpen())
    {
        book_name = g_current_book->getBookName();
        TextPageResult tp = g_current_book->currentPage();
        if (tp.success)
        {
            size_t entry_index;
            int page, row;
            bool on_current;
            if (find_toc_entry_for_position(g_current_book->filePath(), tp.file_pos,
                                            entry_index, page, row, on_current))
            {
                std::string toc_title;
                if (get_toc_title_for_index(g_current_book->filePath(), entry_index, toc_title))
                    chapter_name = toc_title;
            }
        }
    }

    if (!book_name.empty())
    {
        bin_font_print(book_name.c_str(), (uint8_t)fontSize, (uint8_t)textColor,
                       a_w, x, y, false, nullptr, (TextAlign)align, a_w);

        // 计算阅读进度百分比（无论是否有章节名均尝试）
        int pct = -1;
        if (g_current_book)
        {
            size_t total = g_current_book->getFileSize();
            if (total > 0)
            {
                size_t pos = g_current_book->position();
                pct = (int)((double)pos / (double)total * 100.0 + 0.5);
            }
        }

        int chapSize = std::max(8, (int)(fontSize * 0.9f));
        int16_t next_y = y + fontSize + 24;

        if (!chapter_name.empty())
        {
            std::string chapter_display = chapter_name;
            if (pct >= 0)
                chapter_display += std::string(" · ") + std::to_string(pct) + std::string("%");

            bin_font_print(chapter_display.c_str(), (uint8_t)chapSize, (uint8_t)textColor,
                           a_w, x, next_y, false, nullptr, (TextAlign)align, a_w);
        }
        else if (pct >= 0)
        {
            // 无章节信息但有进度，单独显示百分比
            std::string pct_display = std::to_string(pct) + std::string("%");
            bin_font_print(pct_display.c_str(), (uint8_t)chapSize, (uint8_t)textColor,
                           a_w, x, next_y, false, nullptr, (TextAlign)align, a_w);
        }
    }
}
