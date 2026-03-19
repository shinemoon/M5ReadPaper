#include "comp_day.h"
#include "comp_history_cache.h"
#include "readpaper.h"
#include "globals.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "test/per_file_debug.h"
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <ArduinoJson.h>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <time.h>

extern M5Canvas *g_canvas;
extern bool g_wifi_sta_connected;

// ──────────────────────────────────────────────
// fetch_day_info
// ──────────────────────────────────────────────
bool fetch_day_info(String &out_content, String &out_origin)
{
    if (!g_wifi_sta_connected)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[DAY] WiFi 未连接，无法获取日历信息");
#endif
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

#if DBG_TRMNL_SHOW
    Serial.println("[DAY] 请求日历 API: https://api.xygeng.cn/openapi/day");
#endif

    esp_http_client_config_t cfg = {};
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = "https://api.xygeng.cn/openapi/day";
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "User-Agent", "ReadPaper-Day");
    esp_http_client_set_header(client, "Content-Type", "application/json");

    const char *post_body = "{}";
    esp_http_client_set_post_field(client, post_body, (int)strlen(post_body));

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[DAY] 日历 API 返回状态: %d\n", status);
#endif
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    String response_content;
    response_content.reserve(4096);
    char buffer[512];
    while (true)
    {
        int read_len = esp_http_client_read_response(client, buffer, sizeof(buffer) - 1);
        if (read_len <= 0) break;
        buffer[read_len] = '\0';
        response_content += buffer;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (response_content.length() == 0) return false;

    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, response_content) != DeserializationError::Ok)
        return false;

    int code = doc["code"] | 0;
    if (code != 200) return false;

    JsonObject data = doc["data"].as<JsonObject>();
    if (!data) return false;

    const char *date = data["date"] | "";
    JsonObject solar = data["solar"].as<JsonObject>();
    JsonObject lunar = data["lunar"].as<JsonObject>();
    JsonArray festivals = data["festival"].as<JsonArray>();
    const char *sign = data["sign"] | "";

    String content = "";
    if (date && strlen(date) > 0) content += String(date);
    if (solar && solar["day"])
    {
        if (content.length() > 0) content += " ";
        content += String(solar["day"].as<const char *>());
    }

    String lunar_str = "";
    if (lunar)
    {
        const char *lyear = lunar["year"] | "";
        const char *lunarmonth = lunar["lunar_month"] | "";
        const char *llunar_day = lunar["lunar_day"] | "";
        const char *lmonth = lunar["month"] | "";
        const char *ldate = lunar["date"] | "";

        if (lyear && strlen(lyear) > 0) { lunar_str += String(lyear); lunar_str += "年"; }
        if (lunarmonth && strlen(llunar_day) > 0) { lunar_str += String(lunarmonth); lunar_str += "月"; }
        if (llunar_day && strlen(llunar_day) > 0) { lunar_str += String(llunar_day); lunar_str += "日"; }
        if (lmonth && strlen(lmonth) > 0)
        {
            if (lunar_str.length() > 0) lunar_str += " ";
            lunar_str += String(lmonth);
        }
        if (ldate && strlen(ldate) > 0)
        {
            if (lunar_str.length() > 0) lunar_str += " ";
            lunar_str += String(ldate);
        }
    }

    if (lunar_str.length() > 0)
        content += "\n农历：" + lunar_str;

    if (festivals)
    {
        String fest = "";
        for (JsonVariant v : festivals)
        {
            const char *s = v.as<const char *>();
            if (s && strlen(s) > 0)
            {
                if (fest.length() > 0) fest += ",";
                fest += String(s);
            }
        }
        if (fest.length() > 0)
            content += "\n节日：" + fest;
    }

    out_content = content;
    out_origin = String(sign);
    return true;
}

// ──────────────────────────────────────────────
// render_day_component
// ──────────────────────────────────────────────
void render_day_component(JsonObject component)
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

#if DBG_TRMNL_SHOW
    Serial.printf("[DAY] 渲染日历: 单元格(%d,%d) 像素(%d,%d) 字号%d 颜色%d 宽%d 高%d 对齐%s\n",
                  pos_x, pos_y, x, y, fontSize, textColor, a_w, a_h, alignStr);
#endif

    const char *comp_type = component["type"] | "day";
    int comp_zindex = component["zIndex"] | 0;

    String day_content, day_origin;
    if (fetch_day_info(day_content, day_origin))
    {
        cache_save(comp_type, comp_zindex, day_content.c_str(), day_origin.c_str());
    }
    else
    {
#if DBG_TRMNL_SHOW
        Serial.println("[DAY] 获取日历信息失败，尝试读取历史缓存");
#endif
        if (!cache_load(comp_type, comp_zindex, day_content, day_origin))
        {
            struct tm timeinfo;
            if (getLocalTime(&timeinfo, 0))
            {
                char buf[64];
                const char *weekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
                snprintf(buf, sizeof(buf), "%04d-%02d-%02d %s",
                         timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                         timeinfo.tm_mday, weekdays[timeinfo.tm_wday]);
                day_content = String(buf);
            }
            else
            {
                day_content = "N/A";
            }
            day_origin = "";
        }
    }

    // 分割为行数组
    std::vector<String> lines;
    {
        int start = 0;
        while (start < (int)day_content.length())
        {
            int end = day_content.indexOf('\n', start);
            if (end == -1) end = (int)day_content.length();
            String line = day_content.substring(start, end);
            line.trim();
            if (line.length() > 0) lines.push_back(line);
            start = end + 1;
        }
    }

    String solar_line = lines.empty() ? String("") : lines[0];
    String lunar_line = "";
    int lunar_idx = -1;
    for (size_t i = 1; i < lines.size(); ++i)
    {
        String &ln = lines[i];
        if (ln.startsWith("农历") || ln.startsWith("农历：") || ln.startsWith("农历:"))
        {
            int p = ln.indexOf(':');
            if (p == -1) p = ln.indexOf('：');
            lunar_line = (p != -1 && p + 1 < (int)ln.length()) ? ln.substring(p + 1) : ln;
            lunar_idx = (int)i;
            break;
        }
    }

    String festival_line = "";
    if (lunar_idx != -1)
    {
        for (size_t j = lunar_idx + 1; j < lines.size(); ++j)
        {
            String &ln2 = lines[j];
            if (ln2.startsWith("节日") || ln2.startsWith("节日：") || ln2.startsWith("节日:"))
            {
                int p2 = ln2.indexOf(':');
                if (p2 == -1) p2 = ln2.indexOf('：');
                festival_line = (p2 != -1 && p2 + 1 < (int)ln2.length()) ? ln2.substring(p2 + 1) : ln2;
                break;
            }
        }
    }

    uint8_t base_font_size = get_font_size_from_file();
    if (base_font_size == 0) base_font_size = 24;

    int main_font  = fontSize;
    int lunar_font = std::max(8, (int)(fontSize * 0.8f));

    float main_scale  = (main_font  > 0) ? ((float)main_font  / (float)base_font_size) : 1.0f;
    float lunar_scale = (lunar_font > 0) ? ((float)lunar_font / (float)base_font_size) : 1.0f;

    int16_t main_line_h  = (int16_t)((base_font_size + LINE_MARGIN) * main_scale);
    int16_t lunar_line_h = (int16_t)((base_font_size + LINE_MARGIN) * lunar_scale);

    bool show_lunar = (a_h >= (main_line_h + lunar_line_h));

    // 第一行：阳历
    bin_font_print(solar_line.c_str(), (uint8_t)main_font, (uint8_t)textColor,
                   a_w, x, y, false, g_canvas, (TextAlign)align, a_w);

    // 第二行：农历
    int16_t lunar_y = y + main_font + (int16_t)roundf(fontSize * 0.8f);
    if (show_lunar && lunar_line.length() > 0)
    {
        bin_font_print(lunar_line.c_str(), (uint8_t)lunar_font, (uint8_t)textColor,
                       a_w, x, lunar_y, false, g_canvas, (TextAlign)align, a_w);
    }

    // 第三行：节日
    if (festival_line.length() > 0)
    {
        int16_t festival_font   = (int16_t)lunar_font;
        int16_t festival_line_h = lunar_line_h;
        int16_t festival_y      = lunar_y + lunar_line_h + (int16_t)roundf(fontSize * 0.8f);
        if ((festival_y + festival_line_h - y) <= a_h)
        {
            bin_font_print(festival_line.c_str(), (uint8_t)festival_font, (uint8_t)textColor,
                           a_w, x, festival_y, false, g_canvas, (TextAlign)align, a_w);
        }
    }
}
