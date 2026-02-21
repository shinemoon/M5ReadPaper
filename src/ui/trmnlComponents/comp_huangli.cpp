#include "comp_huangli.h"
#include "webParser.h"
#include "readpaper.h"
#include "device/ui_display.h"
#include "test/per_file_debug.h"

extern M5Canvas *g_canvas;
// ──────────────────────────────────────────────
// fetch_huangli_title
// ──────────────────────────────────────────────
bool fetch_huangli_title(String &out_ji, String &out_xiong)
{
    // 从 https://www.huangli.com/huangli/ 提取 class="yi-ji" 元素的文本
    return web_fetch_text_by_class(
               "https://www.huangli.com/huangli/",
               "day-yi",
               out_ji,
               1
               /* match_index=1, max_read_bytes=131072 (defaults) */
               ) &
           // 从 https://www.huangli.com/huangli/ 提取 class="yi-ji" 元素的文本
           web_fetch_text_by_class(
               "https://www.huangli.com/huangli/",
               "day-yi",
               out_xiong,
               3
               /* match_index=1, max_read_bytes=131072 (defaults) */
           );
}

// ──────────────────────────────────────────────
// render_huangli_component
// ──────────────────────────────────────────────
void render_huangli_component(JsonObject component)
{
    int pos_x = 0, pos_y = 0, a_w = 0, a_h = 0;
    if (component.containsKey("position"))
    {
        JsonObject position = component["position"].as<JsonObject>();
        pos_x = position["x"] | 0;
        pos_y = position["y"] | 0;
    }
    JsonObject areaSize = component["size"].as<JsonObject>();
    a_w = areaSize["width"] | 1;
    a_h = areaSize["height"] | 1;

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
        if (strcmp(alignStr, "center") == 0)
            align = 1;
        else if (strcmp(alignStr, "right") == 0)
            align = 2;
    }

    const int CELL_WIDTH = 60, CELL_HEIGHT = 60;
    int16_t x = pos_x * CELL_WIDTH + 60 + xOffset;
    int16_t y = pos_y * CELL_HEIGHT + yOffset;
    a_w = a_w * CELL_WIDTH - 60;
    a_h = a_h * CELL_HEIGHT;

    String JiStr;
    String XiongStr;
    if (!fetch_huangli_title(JiStr, XiongStr))
    {
#if DBG_TRMNL_SHOW
        Serial.println("[HUANGLI] 获取黄历标题失败，使用默认文本");
#endif
        JiStr = "吉不可测";
        XiongStr = "凶未可知";
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[HUANGLI] 渲染黄历: '%s | %s' 单元格(%d,%d) 像素(%d,%d) 字号%d 颜色%d\n",
                  JiStr.c_str(), XiongStr.c_str(), pos_x, pos_y, x, y, fontSize, textColor);
#endif

    int usedLIne = display_print_wrapped(JiStr.c_str(), x, y, a_w, a_h, fontSize * 0.8, textColor, 15, align, false, false);

    g_canvas->drawLine(x, y + (fontSize * 0.8 + LINE_MARGIN) * usedLIne - LINE_MARGIN, a_w, y + (fontSize * 0.8 + LINE_MARGIN) * usedLIne - LINE_MARGIN);

    display_print_wrapped(XiongStr.c_str(), x, y + (fontSize * 0.8 + LINE_MARGIN) * usedLIne, a_w, a_h, fontSize * 0.8, textColor, 15, align, false, false);

    bin_font_print("宜",38,0,540,10,y);
    bin_font_print("忌",38,0,540,10,y + (fontSize * 0.8 + LINE_MARGIN) * usedLIne );
}