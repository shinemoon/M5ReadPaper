#include "comp_huangli.h"
#include "readpaper.h"
#include "globals.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "test/per_file_debug.h"
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <string>
#include <cctype>
#include <algorithm>

extern bool g_wifi_sta_connected;

// ──────────────────────────────────────────────
// fetch_huangli_title
// ──────────────────────────────────────────────
bool fetch_huangli_title(String &out_title)
{
    if (!g_wifi_sta_connected)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[HUANGLI] WiFi 未连接，无法获取黄历");
#endif
        return false;
    }

    const char *api_url = "https://www.huangli.com/huangli/";
#if DBG_TRMNL_SHOW
    Serial.printf("[HUANGLI] 请求黄历页面: %s\n", api_url);
#endif

    esp_http_client_config_t cfg = {};
    cfg.url = api_url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 512;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[HUANGLI] 创建 HTTP 客户端失败");
#endif
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[HUANGLI] HTTP 打开连接失败: %s\n", esp_err_to_name(err));
#endif
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[HUANGLI] 黄历页面返回状态码: %d\n", status);
#endif
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    // TLS 握手完成后立即释放 cert bundle
    esp_crt_bundle_detach(NULL);

    // 轻量流式搜索 div.yi-ji 的文本内容
    enum class HLState { SEARCHING, SKIP_TO_GT, COLLECT_TEXT, DONE };
    HLState hl_state = HLState::SEARCHING;

    static const char NEEDLE[] = "yi-ji";
    static const int  NEEDLE_LEN = 5;
    char   window[32] = {};
    int    win_pos  = 0;
    int    win_fill = 0;

    char text_buf[2048] = {};
    int  text_len = 0;
    int  depth    = 0;
    bool in_tag   = false;
    std::string tag_buf;

    char  read_buf[1024];
    int   read_total = 0;
    bool  found = false;

    while (hl_state != HLState::DONE)
    {
        int r = esp_http_client_read(client, read_buf, sizeof(read_buf) - 1);
        if (r <= 0) break;
        read_buf[r] = '\0';
        read_total += r;

        for (int i = 0; i < r && hl_state != HLState::DONE; ++i)
        {
            char c = read_buf[i];

            if (hl_state == HLState::SEARCHING)
            {
                window[win_pos] = c;
                win_pos = (win_pos + 1) % (int)sizeof(window);
                if (win_fill < (int)sizeof(window)) ++win_fill;

                if (win_fill >= NEEDLE_LEN)
                {
                    bool match = true;
                    for (int k = 0; k < NEEDLE_LEN; ++k)
                    {
                        int idx = ((win_pos - NEEDLE_LEN + k) + (int)sizeof(window)) % (int)sizeof(window);
                        char wc = (char)std::tolower((unsigned char)window[idx]);
                        if (wc != NEEDLE[k]) { match = false; break; }
                    }
                    if (match)
                    {
                        bool verified = false;
                        int windowSize = (int)sizeof(window);
                        int startIdx = ((win_pos - NEEDLE_LEN) + windowSize) % windowSize;
                        for (int back = 0; back < win_fill; ++back)
                        {
                            int pos = (startIdx - 1 - back + windowSize) % windowSize;
                            bool foundClass = true;
                            for (int k = 0; k < 5; ++k)
                            {
                                int idx = (pos - (4 - k) + windowSize) % windowSize;
                                char cc = (char)std::tolower((unsigned char)window[idx]);
                                if (cc != "class"[k]) { foundClass = false; break; }
                            }
                            if (foundClass)
                            {
                                bool hasEq = false;
                                int scan = (pos + 1) % windowSize;
                                while (scan != startIdx)
                                {
                                    if (window[scan] == '=') { hasEq = true; break; }
                                    scan = (scan + 1) % windowSize;
                                }
                                if (hasEq) verified = true;
                                break;
                            }
                        }
                        if (verified)
                            hl_state = HLState::SKIP_TO_GT;
                    }
                }
            }
            else if (hl_state == HLState::SKIP_TO_GT)
            {
                if (c == '>') hl_state = HLState::COLLECT_TEXT;
            }
            else if (hl_state == HLState::COLLECT_TEXT)
            {
                if (!in_tag)
                {
                    if (c == '<')
                    {
                        in_tag = true;
                        tag_buf.clear();
                        tag_buf.push_back(c);
                    }
                    else
                    {
                        if (text_len < (int)sizeof(text_buf) - 1)
                            text_buf[text_len++] = c;
                    }
                }
                else
                {
                    tag_buf.push_back(c);
                    if (c == '>')
                    {
                        bool is_end_tag  = (tag_buf.size() >= 2 && tag_buf[1] == '/');
                        bool is_decl     = (tag_buf.size() >= 2 && tag_buf[1] == '!');
                        bool self_close  = (tag_buf.size() >= 2 && tag_buf[tag_buf.size()-2] == '/');

                        if (!is_decl)
                        {
                            if (is_end_tag)
                            {
                                size_t p = 2;
                                std::string name;
                                while (p < tag_buf.size() && std::isalpha((unsigned char)tag_buf[p]))
                                    name.push_back((char)std::tolower((unsigned char)tag_buf[p++]));

                                if (name == "div" && depth == 0)
                                {
                                    hl_state = HLState::DONE;
                                    found = true;
                                }
                                else if (depth > 0)
                                    --depth;
                            }
                            else if (!self_close)
                                ++depth;
                        }
                        in_tag = false;
                        tag_buf.clear();
                    }
                }
            }
        }

        if (read_total >= 131072) break;
    }
    text_buf[text_len] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

#if DBG_TRMNL_SHOW
    Serial.printf("[HUANGLI] 总读取: %d 字节, found=%d\n", read_total, (int)found);
#endif

    if (!found || text_len == 0)
        return false;

    // 规范化：折叠空白，去首尾
    std::string raw(text_buf, text_len);
    std::string norm;
    norm.reserve(raw.size());
    bool last_space = false;
    for (char ch : raw)
    {
        if (std::isspace((unsigned char)ch))
        {
            if (!last_space) { norm.push_back(' '); last_space = true; }
        }
        else { norm.push_back(ch); last_space = false; }
    }
    size_t s = 0;
    while (s < norm.size() && norm[s] == ' ') ++s;
    size_t e = norm.size();
    while (e > s && norm[e-1] == ' ') --e;
    std::string trimmed = (s >= e) ? "" : norm.substr(s, e - s);

    out_title = String(trimmed.c_str());
#if DBG_TRMNL_SHOW
    Serial.printf("[HUANGLI] 黄历内容: %s\n", trimmed.c_str());
#endif
    return out_title.length() > 0;
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
        if (strcmp(alignStr, "center") == 0) align = 1;
        else if (strcmp(alignStr, "right") == 0) align = 2;
    }

    const int CELL_WIDTH = 60, CELL_HEIGHT = 60;
    int16_t x = pos_x * CELL_WIDTH + 20 + xOffset;
    int16_t y = pos_y * CELL_HEIGHT + yOffset;
    a_w = a_w * CELL_WIDTH - 40;
    a_h = a_h * CELL_HEIGHT;

    String title;
    if (!fetch_huangli_title(title))
    {
#if DBG_TRMNL_SHOW
        Serial.println("[HUANGLI] 获取黄历标题失败，使用默认文本");
#endif
        title = "黄历不可用";
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[HUANGLI] 渲染黄历: '%s' 单元格(%d,%d) 像素(%d,%d) 字号%d 颜色%d\n",
                  title.c_str(), pos_x, pos_y, x, y, fontSize, textColor);
#endif

    display_print_wrapped(title.c_str(), x, y, a_w, a_h,
                          fontSize, textColor, 15, align, false, false);
}
