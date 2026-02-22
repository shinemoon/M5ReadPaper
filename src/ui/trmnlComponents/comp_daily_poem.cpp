#include "comp_daily_poem.h"
#include "comp_history_cache.h"
#include "readpaper.h"
#include "globals.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "test/per_file_debug.h"
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <ArduinoJson.h>

extern M5Canvas *g_canvas;
extern bool g_wifi_sta_connected;

// ──────────────────────────────────────────────
// fetch_daily_poem
// ──────────────────────────────────────────────
bool fetch_daily_poem(String &out_content, String &out_origin)
{
    if (!g_wifi_sta_connected)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[POEM] WiFi 未连接，无法获取今日诗词");
#endif
        return false;
    }

    const char *api_url = "https://v2.jinrishici.com/one.json";
#if DBG_TRMNL_SHOW
    Serial.printf("[POEM] 请求今日诗词 API: %s\n", api_url);
#endif

    esp_http_client_config_t cfg = {};
    cfg.url = api_url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    String response_content;
    response_content.reserve(4096);
    char buffer[512];
    int total_read = 0;
    while (true)
    {
        int read_len = esp_http_client_read_response(client, buffer, sizeof(buffer) - 1);
        if (read_len <= 0) break;
        buffer[read_len] = '\0';
        response_content += buffer;
        total_read += read_len;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total_read == 0) return false;

    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, response_content) != DeserializationError::Ok)
        return false;

    const char *status_str = doc["status"];
    if (!status_str || strcmp(status_str, "success") != 0)
        return false;

    JsonObject data = doc["data"].as<JsonObject>();
    if (!data) return false;

    const char *content = data["content"];
    JsonObject origin = data["origin"].as<JsonObject>();
    if (!content || !origin) return false;

    const char *title = origin["title"];
    const char *dynasty = origin["dynasty"];
    const char *author = origin["author"];

    out_content = String(content);
    out_origin = "";
    if (title && strlen(title) > 0) out_origin += String(title);
    if (dynasty && strlen(dynasty) > 0)
    {
        if (out_origin.length() > 0) out_origin += "·";
        out_origin += String(dynasty);
    }
    if (author && strlen(author) > 0)
    {
        if (out_origin.length() > 0) out_origin += "·";
        out_origin += String(author);
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[POEM] 今日诗词: %s / %s\n", out_content.c_str(), out_origin.c_str());
#endif
    return true;
}

// ──────────────────────────────────────────────
// render_daily_poem_component
// ──────────────────────────────────────────────
void render_daily_poem_component(JsonObject component)
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

    const char *comp_type = component["type"] | "daily_poem";
    int comp_zindex = component["zIndex"] | 0;

    String poem_content, poem_origin;
    if (fetch_daily_poem(poem_content, poem_origin))
    {
        cache_save(comp_type, comp_zindex, poem_content.c_str(), poem_origin.c_str());
    }
    else
    {
#if DBG_TRMNL_SHOW
        Serial.println("[POEM] 获取今日诗词失败，尝试读取历史缓存");
#endif
        if (!cache_load(comp_type, comp_zindex, poem_content, poem_origin))
        {
            poem_content = "扣舷独啸，不知今夕何夕。";
            poem_origin = "过洞庭·宋·张孝祥";
        }
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[POEM] 渲染今日诗词: 单元格(%d,%d) 像素(%d,%d) 字号%d 颜色%d 宽%d 高%d 对齐%s\n",
                  pos_x, pos_y, x, y, fontSize, textColor, a_w, a_h, alignStr);
#endif

    int used_lines = display_print_wrapped(
        poem_content.c_str(), x, y, a_w, a_h,
        fontSize, textColor, 15, align, false, false);

    uint8_t base_font_size = get_font_size_from_file();
    if (base_font_size == 0) base_font_size = 24;
    float scale_factor = (fontSize > 0) ? ((float)fontSize / (float)base_font_size) : 1.0f;
    int16_t line_height = (int16_t)((base_font_size + LINE_MARGIN) * scale_factor);
    int max_lines = a_h / line_height;
    int remaining_lines = max_lines - used_lines;

    if (remaining_lines > 0 && poem_origin.length() > 0)
    {
        int16_t origin_y = y + used_lines * line_height;
        uint8_t origin_font_size = (uint8_t)(fontSize * 0.8f);

        display_print_wrapped(
            poem_origin.c_str(), x, origin_y, a_w,
            remaining_lines * line_height,
            origin_font_size, (uint8_t)textColor, 15, align, false, false);
    }
}
