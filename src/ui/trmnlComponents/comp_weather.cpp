#include "comp_weather.h"
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
// fetch_weather
// ──────────────────────────────────────────────
bool fetch_weather(const String &citycode, const String &apiKey,
                   String &out_today_info, String &out_tomorrow_info)
{
    if (!g_wifi_sta_connected)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[WEATHER] WiFi 未连接，无法获取天气信息");
#endif
        return false;
    }
    if (apiKey.length() == 0)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[WEATHER] API Key 为空，无法获取天气信息");
#endif
        return false;
    }

    String api_url = "https://restapi.amap.com/v3/weather/weatherInfo?city=" + citycode +
                     "&key=" + apiKey + "&extensions=all";
#if DBG_TRMNL_SHOW
    Serial.printf("[WEATHER] 请求天气 API: %s\n", api_url.c_str());
#endif

    esp_http_client_config_t cfg = {};
    cfg.url = api_url.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 8192;
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
    response_content.reserve(8192);
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

    DynamicJsonDocument doc(12288);
    if (deserializeJson(doc, response_content) != DeserializationError::Ok)
        return false;

    const char *status_str = doc["status"];
    if (!status_str || strcmp(status_str, "1") != 0)
        return false;

    JsonArray forecasts = doc["forecasts"].as<JsonArray>();
    if (!forecasts || forecasts.size() == 0) return false;

    JsonObject forecast = forecasts[0].as<JsonObject>();
    if (!forecast) return false;

    JsonArray casts = forecast["casts"].as<JsonArray>();
    if (!casts || casts.size() == 0) return false;

    JsonObject today = casts[0].as<JsonObject>();
    if (!today) return false;

    const char *dayweather = today["dayweather"];
    const char *daytemp    = today["daytemp"];
    const char *nighttemp  = today["nighttemp"];
    const char *daywind    = today["daywind"];
    const char *daypower   = today["daypower"];

    String today_info = "";
    if (dayweather && strlen(dayweather) > 0) today_info += dayweather;
    if (daytemp && nighttemp)
    {
        today_info += " ";
        today_info += nighttemp;
        today_info += "~";
        today_info += daytemp;
        today_info += "℃";
    }
    if (daywind && daypower)
    {
        today_info += " ";
        today_info += daywind;
        today_info += "风";
        today_info += daypower;
        today_info += "级";
    }
    out_today_info = today_info;

    out_tomorrow_info = "";
    if (casts.size() > 1)
    {
        JsonObject tomorrow = casts[1].as<JsonObject>();
        if (tomorrow)
        {
            const char *tmr_dw = tomorrow["dayweather"];
            const char *tmr_dt = tomorrow["daytemp"];
            const char *tmr_nt = tomorrow["nighttemp"];

            String tmr_info = "明天: ";
            if (tmr_dw && strlen(tmr_dw) > 0) tmr_info += tmr_dw;
            if (tmr_dt && tmr_nt)
            {
                tmr_info += " ";
                tmr_info += tmr_nt;
                tmr_info += "~";
                tmr_info += tmr_dt;
                tmr_info += "℃";
            }
            out_tomorrow_info = tmr_info;
        }
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[WEATHER] 今天天气: %s\n", out_today_info.c_str());
    if (out_tomorrow_info.length() > 0)
        Serial.printf("[WEATHER] 明天天气: %s\n", out_tomorrow_info.c_str());
#endif
    return true;
}

// ──────────────────────────────────────────────
// render_weather_component
// ──────────────────────────────────────────────
void render_weather_component(JsonObject component)
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

    String citycode = "110000", apiKey = "";
    int fontSize = 24, textColor = 0, xOffset = 0, yOffset = 0, align = 0;
    if (component.containsKey("config"))
    {
        JsonObject config = component["config"].as<JsonObject>();
        citycode  = config["citycode"] | "110000";
        apiKey    = config["apiKey"] | "";
        fontSize  = config["fontSize"] | 24;
        textColor = config["textColor"] | 0;
        xOffset   = config["xOffset"] | 0;
        yOffset   = config["yOffset"] | 0;
        align     = config["align"] | 0;
    }

    const int CELL_WIDTH = 60, CELL_HEIGHT = 60;
    int16_t x = pos_x * CELL_WIDTH + 20 + xOffset;
    int16_t y = pos_y * CELL_HEIGHT + yOffset;
    a_w = a_w * CELL_WIDTH - 40;
    a_h = a_h * CELL_HEIGHT;

    String today_info, tomorrow_info;
    if (!fetch_weather(citycode, apiKey, today_info, tomorrow_info))
        return;

    int lines_used = display_print_wrapped(
        today_info.c_str(), x, y, a_w, a_h,
        fontSize, textColor, 15, align, false, false);

    uint8_t base_font_size = get_font_size_from_file();
    if (base_font_size == 0) base_font_size = 24;
    float scale_factor = (fontSize > 0) ? ((float)fontSize / (float)base_font_size) : 1.0f;
    int16_t line_height    = (int16_t)((base_font_size + 8) * scale_factor); // LINE_MARGIN=8
    int16_t used_height    = lines_used * line_height;
    int16_t remaining_height = a_h - used_height;

    if (remaining_height > fontSize && tomorrow_info.length() > 0)
    {
        int tomorrow_font_size = (int)(fontSize * 0.8f);
        int16_t tomorrow_y = y + used_height + 30;

        g_canvas->fillRect(x, tomorrow_y, 4, tomorrow_font_size, TFT_BLACK);

        display_print_wrapped(
            tomorrow_info.c_str(), x + 8, tomorrow_y, a_w,
            remaining_height, tomorrow_font_size, textColor, 15, align, false, false);
    }
}
