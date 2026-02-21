#include "comp_rss.h"
#include "comp_list.h"
#include "readpaper.h"
#include "globals.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "test/per_file_debug.h"
#include <esp_http_client.h>
#include <esp_crt_bundle.h>

extern M5Canvas *g_canvas;
extern bool g_wifi_sta_connected;

// ──────────────────────────────────────────────
// 内部辅助：从累积缓冲区提取下一个 RSS/Atom item 的 title
// 返回 item 结束位置（供调用者截断 buffer），未找到完整 item 返回 -1
// ──────────────────────────────────────────────
static int extract_next_rss_item(String &buffer, bool is_atom, String &out_title)
{
    out_title = "";

    int item_start = buffer.indexOf(is_atom ? "<entry>" : "<item>");
    int item_start_with_attr = buffer.indexOf(is_atom ? "<entry " : "<item ");
    if (item_start == -1 || (item_start_with_attr != -1 && item_start_with_attr < item_start))
        item_start = item_start_with_attr;
    if (item_start == -1) return -1;

    const char *item_end_tag = is_atom ? "</entry>" : "</item>";
    int item_end_tag_len = is_atom ? 8 : 7;
    int item_end = buffer.indexOf(item_end_tag, item_start);
    if (item_end == -1) return -1;

    int title_start = buffer.indexOf("<title>", item_start);
    if (title_start != -1 && title_start < item_end)
    {
        title_start += 7;
        int title_end = buffer.indexOf("</title>", title_start);
        if (title_end != -1 && title_end < item_end)
        {
            String title = buffer.substring(title_start, title_end);
            if (title.startsWith("<![CDATA["))
            {
                title = title.substring(9);
                int cdata_end = title.indexOf("]]>");
                if (cdata_end >= 0) title = title.substring(0, cdata_end);
            }
            title.trim();
            out_title = title;
        }
    }

    return item_end + item_end_tag_len;
}

// ──────────────────────────────────────────────
// fetch_rss_feed
// ──────────────────────────────────────────────
bool fetch_rss_feed(const String &url, String &out_titles, int max_items)
{
    out_titles = "";

    if (!g_wifi_sta_connected)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[RSS] WiFi未连接，无法获取RSS");
#endif
        return false;
    }
    if (url.length() == 0)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[RSS] RSS URL为空");
#endif
        return false;
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[RSS] 获取RSS feed: %s\n", url.c_str());
#endif

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 8192;
    cfg.buffer_size_tx = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "User-Agent", "ReadPaper-RSS/1.0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        esp_http_client_cleanup(client);
        return false;
    }

    err = esp_http_client_fetch_headers(client);
    if (err < 0)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    const int READ_BUFFER_SIZE = 2048;
    char read_buffer[READ_BUFFER_SIZE];
    String parse_buffer = "";
    int total_read = 0;
    int item_count = 0;
    bool is_atom = false;
    bool format_detected = false;

    while (item_count < max_items)
    {
        int data_read = esp_http_client_read(client, read_buffer, READ_BUFFER_SIZE - 1);
        if (data_read < 0) break;
        if (data_read == 0) break;

        read_buffer[data_read] = '\0';
        parse_buffer += String(read_buffer);
        total_read += data_read;

        if (!format_detected && parse_buffer.length() > 100)
        {
            is_atom = (parse_buffer.indexOf("<feed") >= 0 || parse_buffer.indexOf("<feed ") >= 0);
            format_detected = true;
#if DBG_TRMNL_SHOW
            Serial.printf("[RSS] 检测到RSS格式: %s\n", is_atom ? "Atom" : "RSS");
#endif
        }

        while (item_count < max_items)
        {
            String title = "";
            int item_end_pos = extract_next_rss_item(parse_buffer, is_atom, title);

            if (item_end_pos == -1)
            {
                if (parse_buffer.length() > 16384)
                {
                    int last_item_start = parse_buffer.lastIndexOf(is_atom ? "<entry>" : "<item>");
                    int last_item_start_attr = parse_buffer.lastIndexOf(is_atom ? "<entry " : "<item ");
                    if (last_item_start < last_item_start_attr)
                        last_item_start = last_item_start_attr;

                    if (last_item_start > 0)
                        parse_buffer = parse_buffer.substring(last_item_start);
                    else
                        parse_buffer = "";
                }
                break;
            }

            if (title.length() > 0)
            {
                if (item_count > 0) out_titles += ";";
                out_titles += title;
                item_count++;
#if DBG_TRMNL_SHOW
                Serial.printf("[RSS] RSS item %d: %s\n", item_count, title.c_str());
#endif
            }
            parse_buffer = parse_buffer.substring(item_end_pos);
            if (item_count >= max_items) break;
        }
        if (item_count >= max_items) break;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

#if DBG_TRMNL_SHOW
    Serial.printf("[RSS] 完成: 共读取%d字节, 提取%d个标题\n", total_read, item_count);
#endif
    return item_count > 0;
}

// ──────────────────────────────────────────────
// render_rss_component
// ──────────────────────────────────────────────
void render_rss_component(JsonObject component)
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

    const char *url = "";
    int fontSize = 24, textColor = 0, xOffset = 0, yOffset = 0, margin = 10;
    if (component.containsKey("config"))
    {
        JsonObject config = component["config"].as<JsonObject>();
        url = config["url"] | "";
        fontSize = config["fontSize"] | 24;
        textColor = config["textColor"] | 0;
        xOffset = config["xOffset"] | 0;
        yOffset = config["yOffset"] | 0;
        margin = config["margin"] | 10;
    }

    const int CELL_WIDTH = 60, CELL_HEIGHT = 60;
    int16_t x = pos_x * CELL_WIDTH + 20 + xOffset;
    int16_t y = pos_y * CELL_HEIGHT + yOffset;
    a_w = a_w * CELL_WIDTH - 40;
    a_h = a_h * CELL_HEIGHT;

#if DBG_TRMNL_SHOW
    Serial.printf("[RSS] 渲染RSS: 单元格(%d,%d) 像素(%d,%d) 字号%d 颜色%d 宽%d 高%d margin%d\n",
                  pos_x, pos_y, x, y, fontSize, textColor, a_w, a_h, margin);
    Serial.printf("[RSS] RSS URL: %s\n", url);
#endif

    // 计算最大可显示项数
    uint8_t base_font_size = get_font_size_from_file();
    if (base_font_size == 0) base_font_size = 24;
    float scale_factor = (fontSize > 0) ? ((float)fontSize / (float)base_font_size) : 1.0f;
    int16_t line_height = (int16_t)(base_font_size * scale_factor) + margin;
    int max_lines = a_h / line_height;
    if (max_lines <= 0) max_lines = 1;

    String rss_titles = "";
    bool success = fetch_rss_feed(String(url), rss_titles, max_lines);

    if (success && rss_titles.length() > 0)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[RSS] RSS获取成功，标题列表: %s\n", rss_titles.c_str());
#endif
        render_list_items(rss_titles.c_str(), x, y, a_w, a_h,
                          (uint8_t)fontSize, (uint8_t)textColor, (int16_t)margin);
    }
    else
    {
#if DBG_TRMNL_SHOW
        Serial.println("[RSS] RSS获取失败或内容为空");
#endif
        bin_font_print("RSS加载失败", fontSize, textColor,
                       a_w, x, y, false, g_canvas,
                       TEXT_ALIGN_LEFT, a_w, false);
    }
}
