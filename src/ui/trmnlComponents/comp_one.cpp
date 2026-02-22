#include "comp_one.h"
#include "readpaper.h"
#include "globals.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "test/per_file_debug.h"
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <ArduinoJson.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <string>
#include <algorithm>

extern M5Canvas *g_canvas;
extern bool g_wifi_sta_connected;

// ──────────────────────────────────────────────
// fetch_one_sentence
// ──────────────────────────────────────────────
bool fetch_one_sentence(String &out_content, String &out_origin)
{
    if (!g_wifi_sta_connected)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[ONE] WiFi 未连接，无法获取 ONE 一言");
#endif
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

#if DBG_TRMNL_SHOW
    Serial.println("[ONE] 请求 ONE 一言 API: https://api.xygeng.cn/openapi/one");
#endif

    // 先进行 DNS 解析，避开 HTTP 客户端的主机名解析 bug
    const char *hostname = "api.xygeng.cn";
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int ret = getaddrinfo(hostname, "443", &hints, &res);
    if (ret != 0 || res == NULL)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[ONE] DNS 解析失败: %d\n", ret);
#endif
        return false;
    }

    static char ip_str[INET_ADDRSTRLEN];
    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &(addr->sin_addr), ip_str, INET_ADDRSTRLEN);
    freeaddrinfo(res);

#if DBG_TRMNL_SHOW
    Serial.printf("[ONE] DNS 解析成功: %s -> %s\n", hostname, ip_str);
#endif

    esp_http_client_config_t cfg = {};
    memset(&cfg, 0, sizeof(cfg));
    cfg.host = ip_str;
    cfg.port = 443;
    cfg.path = "/openapi/one";
    cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;
    cfg.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "User-Agent", "ReadPaper-ONE");

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

    int code = doc["code"] | 0;
    if (code != 200) return false;

    JsonObject data = doc["data"].as<JsonObject>();
    if (!data) return false;

    const char *content = data["content"];
    const char *tag = data["tag"];
    const char *name = data["name"];
    const char *origin = data["origin"];

    if (!content || strlen(content) == 0) return false;

    out_content = String(content);
    out_origin = "";
    if (tag && strlen(tag) > 0) out_origin += String(tag);
    if (name && strlen(name) > 0)
    {
        if (out_origin.length() > 0) out_origin += "·";
        out_origin += String(name);
    }
    if (origin && strlen(origin) > 0)
    {
        if (out_origin.length() > 0) out_origin += "·";
        out_origin += String(origin);
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[ONE] ONE 一言: %s / %s\n", out_content.c_str(), out_origin.c_str());
#endif
    return true;
}

// ──────────────────────────────────────────────
// render_one_component
// ──────────────────────────────────────────────
void render_one_component(JsonObject component)
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

    String one_content, one_origin;
    if (!fetch_one_sentence(one_content, one_origin))
    {
#if DBG_TRMNL_SHOW
        Serial.println("[ONE] 获取ONE一言失败，使用默认文本");
#endif
        one_content = "日日行，不怕千万人，怕自己不强。";
        one_origin = "生活·佚名·佚名";
    }

    // 去除内容中的空行
    String cleaned_content = "";
    int start = 0;
    while (start < (int)one_content.length())
    {
        int end = one_content.indexOf('\n', start);
        if (end == -1) end = (int)one_content.length();
        String line = one_content.substring(start, end);
        line.trim();
        if (line.length() > 0)
        {
            if (cleaned_content.length() > 0) cleaned_content += " ";
            cleaned_content += line;
        }
        start = end + 1;
    }
    one_content = cleaned_content;

    uint8_t base_font_size = get_font_size_from_file();
    if (base_font_size == 0) base_font_size = 24;
    float scale_factor = (fontSize > 0) ? ((float)fontSize / (float)base_font_size) : 1.0f;
    int16_t line_height = (int16_t)((base_font_size + LINE_MARGIN) * scale_factor);
    int max_lines = a_h / line_height;

    int content_max_lines = std::max(1, max_lines - 1);
    int16_t content_height = content_max_lines * line_height;

#if DBG_TRMNL_SHOW
    Serial.printf("[ONE] 渲染ONE一言: 单元格(%d,%d) 像素(%d,%d) 字号%d 颜色%d 宽%d 高%d 对齐%s\n",
                  pos_x, pos_y, x, y, fontSize, textColor, a_w, a_h, alignStr);
    Serial.printf("[ONE] 总行数:%d 内容行数:%d origin行数:1\n", max_lines, content_max_lines);
#endif

    display_print_wrapped(
        one_content.c_str(), x, y, a_w, content_height,
        fontSize, textColor, 15, align, false, false);

    if (one_origin.length() > 0)
    {
        int16_t origin_y = y + content_max_lines * line_height;
        uint8_t origin_font_size = (uint8_t)(fontSize * 0.8f);

        bin_font_print(
            one_origin.c_str(), origin_font_size, (uint8_t)textColor,
            a_w, x, origin_y, false, g_canvas,
            (TextAlign)align, a_w, false);
    }
}
