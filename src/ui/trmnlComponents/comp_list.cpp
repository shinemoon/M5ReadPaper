#include "comp_list.h"
#include "readpaper.h"
#include "globals.h"
#include "text/bin_font_print.h"
#include "test/per_file_debug.h"

extern M5Canvas *g_canvas;

// ──────────────────────────────────────────────
// render_list_items
// ──────────────────────────────────────────────
int render_list_items(
    const char *content,
    int16_t x, int16_t y,
    int16_t area_width, int16_t area_height,
    uint8_t fontSize, uint8_t textColor,
    int16_t margin)
{
    if (!content || strlen(content) == 0)
        return 0;

    // 计算行高
    uint8_t base_font_size = get_font_size_from_file();
    if (base_font_size == 0)
        base_font_size = 24;
    float scale_factor = (fontSize > 0) ? ((float)fontSize / (float)base_font_size) : 1.0f;
    int16_t line_height = (int16_t)(base_font_size * scale_factor) + margin;

    int max_lines = area_height / line_height;
    if (max_lines <= 0)
        max_lines = 1;

#if DBG_TRMNL_SHOW
    Serial.printf("[LIST] 列表渲染: area_height=%d, fontSize=%d, base=%d, scale=%.2f, 行高=%d, margin=%d, 最大行数=%d\n",
                  area_height, fontSize, base_font_size, scale_factor, line_height, margin, max_lines);
#endif

    String textStr = String(content);
    int item_count = 0;
    int16_t current_y = y;
    int start_pos = 0;

    while (item_count < max_lines)
    {
        int semicolon_pos = textStr.indexOf(';', start_pos);
        String item;

        if (semicolon_pos == -1)
        {
            item = textStr.substring(start_pos);
            if (item.length() == 0)
                break;
        }
        else
        {
            item = textStr.substring(start_pos, semicolon_pos);
            start_pos = semicolon_pos + 1;
        }

        item.trim();
        if (item.length() == 0)
        {
            if (semicolon_pos == -1)
                break;
            continue;
        }

#if DBG_TRMNL_SHOW
        Serial.printf("[LIST] 列表项%d: '%s' at y=%d\n", item_count + 1, item.c_str(), current_y);
#endif

        // 绘制 bullet 点（实心圆 + 空心圆形成圆环）
        g_canvas->fillCircle(x, current_y, 6, TFT_BLACK);
        g_canvas->fillCircle(x, current_y, 3, TFT_WHITE);

        bin_font_print(
            item.c_str(),
            fontSize,
            textColor,
            area_width,
            x + 20,
            current_y - fontSize / 2,
            false,
            g_canvas,
            TEXT_ALIGN_LEFT,
            area_width,
            false);

        current_y += line_height;
        item_count++;

        if (semicolon_pos == -1)
            break;
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[LIST] 列表渲染完成，共%d项\n", item_count);
#endif

    return item_count;
}

// ──────────────────────────────────────────────
// render_list_component
// ──────────────────────────────────────────────
void render_list_component(JsonObject component)
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

    const char *text = "";
    int fontSize = 24, textColor = 0, xOffset = 0, yOffset = 0, margin = 10;
    if (component.containsKey("config"))
    {
        JsonObject config = component["config"].as<JsonObject>();
        text = config["text"] | "";
        fontSize = config["fontSize"] | 24;
        textColor = config["textColor"] | 0;
        xOffset = config["xOffset"] | 0;
        yOffset = config["yOffset"] | 0;
        margin = config["margin"] | 10;
    }

    const int CELL_WIDTH = 60, CELL_HEIGHT = 60;
    int16_t x = pos_x * CELL_WIDTH + 20 + xOffset;
    int16_t y = pos_y * CELL_HEIGHT + yOffset;
    a_w = (int)(cell_w * CELL_WIDTH) - 40;
    a_h = (int)(cell_h * CELL_HEIGHT);

#if DBG_TRMNL_SHOW
    Serial.printf("[LIST] 渲染列表: 单元格(%d,%d) 像素(%d,%d) 字号%d 颜色%d 宽度%d 高度%d margin%d\n",
                  pos_x, pos_y, x, y, fontSize, textColor, a_w, a_h, margin);
#endif

    render_list_items(text, x, y, a_w, a_h, (uint8_t)fontSize, (uint8_t)textColor, (int16_t)margin);
}
