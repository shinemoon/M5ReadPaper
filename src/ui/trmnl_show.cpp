#include "trmnl_show.h"
#include "readpaper.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "device/wifi_hotspot_manager.h"
#include "globals.h"
#include "config/config_manager.h"
#include "test/per_file_debug.h"
#include "SD/SDWrapper.h"
#include <string>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <mbedtls/base64.h>
#include "ui/ui_canvas_image.h"
#include <ArduinoJson.h>
// TOC helpers (find_toc_entry_for_position, get_toc_title_for_index)
#include "ui/toc_display.h"

extern M5Canvas *g_canvas;
extern WiFiHotspotManager *g_wifi_hotspot;
extern bool g_wifi_sta_connected;
extern GlobalConfig g_config;

// 前向声明
static bool extract_rdt_timestamp(const String &content, String &out_timestamp);
static bool fetch_daily_poem(String &out_content, String &out_origin);
static int render_list_items(const char *content, int16_t x, int16_t y, int16_t area_width, int16_t area_height, uint8_t fontSize, uint8_t textColor, int16_t margin);
static bool fetch_rss_feed(const String &url, String &out_titles);
static bool parse_rss_titles(const String &xml_content, String &out_titles);
static bool fetch_weather(const String &citycode, const String &apiKey, String &out_today_info, String &out_tomorrow_info);

// 从 WebDAV 读取 readpaper.rdt 文件内容
static bool fetch_webdav_rdt_config(String &out_content)
{
    // 检查 WebDAV 配置（优先检查配置，避免无意义的WiFi检查）
    if (strlen(g_config.webdav_url) == 0)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] WebDAV 未配置，跳过配置读取");
#endif
        return false;
    }

    // 检查 WiFi 连接
    if (!g_wifi_sta_connected)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] WiFi 未连接，无法读取 WebDAV 配置");
#endif
        return false;
    }

    // 构建目标 URL
    String base_url = String(g_config.webdav_url);
    if (!base_url.endsWith("/"))
    {
        base_url += "/";
    }
    String target_url = base_url + "readpaper/readpaper.rdt";

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] 尝试读取: %s\n", target_url.c_str());
#endif

    // 准备认证
    String webdav_user = String(g_config.webdav_user);
    String webdav_pass = String(g_config.webdav_pass);
    bool has_auth = (webdav_user.length() > 0 || webdav_pass.length() > 0);

    String auth_header;
    if (has_auth)
    {
        char auth_raw[160];
        snprintf(auth_raw, sizeof(auth_raw), "%s:%s", webdav_user.c_str(), webdav_pass.c_str());

        unsigned char b64[256] = {0};
        size_t out_len = 0;
        if (mbedtls_base64_encode(b64, sizeof(b64) - 1, &out_len,
                                  reinterpret_cast<const unsigned char *>(auth_raw),
                                  strlen(auth_raw)) == 0)
        {
            b64[out_len] = '\0';
            auth_header = String("Basic ") + reinterpret_cast<const char *>(b64);
        }
    }

    // 配置 HTTP 客户端
    esp_http_client_config_t cfg = {};
    cfg.url = target_url.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach; // 启用证书验证
    cfg.disable_auto_redirect = false;

    if (has_auth)
    {
        cfg.auth_type = HTTP_AUTH_TYPE_BASIC;
        cfg.username = g_config.webdav_user;
        cfg.password = g_config.webdav_pass;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] HTTP 客户端初始化失败");
#endif
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", "ReadPaper-TRMNL");
    if (auth_header.length() > 0)
    {
        esp_http_client_set_header(client, "Authorization", auth_header.c_str());
    }

    // 发送请求
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] HTTP 打开失败: %s\n", esp_err_to_name(err));
#endif
        esp_http_client_cleanup(client);
        return false;
    }

    err = esp_http_client_fetch_headers(client);
    if (err < 0)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] HTTP 读取头失败: %s\n", esp_err_to_name(err));
#endif
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int status_code = esp_http_client_get_status_code(client);
#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] HTTP 状态码: %d\n", status_code);
#endif

    if (status_code != 200)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    // 读取响应内容
    char buffer[2048];
    int content_length = esp_http_client_get_content_length(client);
    int read_len = 0;
    out_content = "";

    while (read_len < content_length || content_length == -1)
    {
        int data_read = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
        if (data_read <= 0)
        {
            break;
        }
        buffer[data_read] = '\0';
        out_content += String(buffer);
        read_len += data_read;

        // 限制最大读取大小
        if (read_len >= 8192)
        {
            break;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] 读取成功，长度: %d\n", out_content.length());
#endif

    return out_content.length() > 0;
}

// 从 SD 卡读取 RDT 文件内容
static bool read_sdcard_rdt(String &out_content)
{
    const char *rdt_path = "/rdt/readpaper.rdt";

    if (!SDW::SD.exists(rdt_path))
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] SD 卡文件不存在: %s\n", rdt_path);
#endif
        return false;
    }

    File file = SDW::SD.open(rdt_path, "r");
    if (!file)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] 无法打开 SD 卡文件: %s\n", rdt_path);
#endif
        return false;
    }

    out_content = "";
    while (file.available())
    {
        char c = file.read();
        out_content += c;

        // 限制最大读取大小
        if (out_content.length() >= 8192)
        {
            break;
        }
    }
    file.close();

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] 从 SD 卡读取成功，长度: %d\n", out_content.length());
#endif

    return out_content.length() > 0;
}

// 从 WebDAV 获取 RDT 文件的时间戳（通过读取 RDT 内容并解析 timestamp 字段）
static bool fetch_webdav_rdt_timestamp(String &out_timestamp)
{
    // 检查 WebDAV 配置（优先检查配置，避免无意义的WiFi检查）
    if (strlen(g_config.webdav_url) == 0)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] WebDAV 未配置，跳过时间戳检查");
#endif
        return false;
    }

    // 检查 WiFi 连接
    if (!g_wifi_sta_connected)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] WiFi 未连接，无法读取 WebDAV 时间戳");
#endif
        return false;
    }

    // 构建目标 URL
    String base_url = String(g_config.webdav_url);
    if (!base_url.endsWith("/"))
    {
        base_url += "/";
    }
    String target_url = base_url + "readpaper/readpaper.rdt";

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] 获取 WebDAV RDT 时间戳: %s\n", target_url.c_str());
#endif

    // 配置 HTTP 客户端（使用 GET 请求获取内容）
    esp_http_client_config_t cfg = {};
    cfg.url = target_url.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 4096; // 增加到4KB
    cfg.buffer_size_tx = 512;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    if (strlen(g_config.webdav_user) > 0 || strlen(g_config.webdav_pass) > 0)
    {
        cfg.auth_type = HTTP_AUTH_TYPE_BASIC;
        cfg.username = g_config.webdav_user;
        cfg.password = g_config.webdav_pass;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] HTTP 客户端初始化失败");
#endif
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", "ReadPaper-TRMNL");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] HTTP 打开失败: %s\n", esp_err_to_name(err));
#endif
        esp_http_client_cleanup(client);
        return false;
    }

    err = esp_http_client_fetch_headers(client);
    if (err < 0)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] HTTP 读取头失败: %s\n", esp_err_to_name(err));

#endif
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] HTTP 状态码: %d\n", status_code);
#endif
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    // 读取 RDT 内容（限制最大 8KB）
    char buffer[4096];
    int content_length = esp_http_client_get_content_length(client);
    int read_len = 0;
    String rdt_content = "";
    rdt_content.reserve(8192); // 预分配避免多次重新分配

    while (read_len < content_length || content_length == -1)
    {
        int data_read = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
        if (data_read <= 0)
        {
            break;
        }
        buffer[data_read] = '\0';
        rdt_content += String(buffer);
        read_len += data_read;

        // 限制最大读取 8KB（RDT 文件通常较小）
        if (read_len >= 8192)
        {
#if DBG_TRMNL_SHOW
            Serial.println("[TRMNL] 警告：RDT 文件超过8KB，已截断");
#endif
            break;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (rdt_content.length() == 0)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] WebDAV RDT 内容为空");
#endif
        return false;
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] WebDAV RDT 内容长度: %d\n", rdt_content.length());
#endif

    // 解析 timestamp 字段
    return extract_rdt_timestamp(rdt_content, out_timestamp);
}

// 从本地 RDT 文件内容提取 timestamp 字段
static bool extract_rdt_timestamp(const String &content, String &out_timestamp)
{
    DynamicJsonDocument doc(8192);
    DeserializationError error = deserializeJson(doc, content);

    if (error)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] JSON 解析失败: %s\n", error.c_str());
#endif
        return false;
    }

    if (doc.containsKey("timestamp"))
    {
        const char *ts = doc["timestamp"];
        out_timestamp = String(ts);
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] 本地时间戳: %s\n", out_timestamp.c_str());
#endif
        return true;
    }

#if DBG_TRMNL_SHOW
    Serial.println("[TRMNL] 本地 RDT 中未找到时间戳字段");
#endif
    return false;
}

// 从今日诗词 API 获取诗词内容
// 从今日诗词 API 获取诗词内容
static bool fetch_daily_poem(String &out_content, String &out_origin)
{
    // 检查 WiFi 连接
    if (!g_wifi_sta_connected)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] WiFi 未连接，无法获取今日诗词");
#endif
        return false;
    }

    const char *api_url = "https://v2.jinrishici.com/one.json";

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] 请求今日诗词 API: %s\n", api_url);
#endif

    // 配置 HTTP 客户端
    esp_http_client_config_t cfg = {};
    cfg.url = api_url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] 创建 HTTP 客户端失败");
#endif
        return false;
    }

    // 打开连接
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] HTTP 打开连接失败: %s\n", esp_err_to_name(err));
#endif
        esp_http_client_cleanup(client);
        return false;
    }

    // 获取响应头
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] API 返回错误状态码: %d\n", status);
#endif
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    // 读取响应内容
    String response_content;
    response_content.reserve(4096);

    char buffer[512];
    int total_read = 0;

    // 使用 read_response 读取完整响应体
    while (true)
    {
        int read_len = esp_http_client_read_response(client, buffer, sizeof(buffer) - 1);
        if (read_len <= 0)
            break;
        buffer[read_len] = '\0';
        response_content += buffer;
        total_read += read_len;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // 检查是否读取到内容
    if (total_read == 0 || response_content.length() == 0)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] API 返回空内容");
#endif
        return false;
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] API 响应长度: %d\n", response_content.length());
#endif

    // 解析 JSON
    DynamicJsonDocument doc(8192);
    DeserializationError error = deserializeJson(doc, response_content);

    if (error)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] JSON 解析失败: %s\n", error.c_str());
#endif
        return false;
    }

    // 检查 status 字段
    const char *status_str = doc["status"];
    if (!status_str || strcmp(status_str, "success") != 0)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] API 返回错误状态: %s\n", status_str ? status_str : "(null)");
#endif
        return false;
    }

    // 提取诗词信息
    JsonObject data = doc["data"].as<JsonObject>();
    if (!data)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] 未找到 data 字段");
#endif
        return false;
    }

    const char *content = data["content"];
    JsonObject origin = data["origin"].as<JsonObject>();

    if (!content || !origin)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] 未找到诗词内容或出处信息");
#endif
        return false;
    }

    const char *title = origin["title"];
    const char *dynasty = origin["dynasty"];
    const char *author = origin["author"];

    // 分别返回正文和出处信息
    out_content = String(content);

    // 格式化出处：标题·朝代·作者
    out_origin = "";
    if (title && strlen(title) > 0)
    {
        out_origin += String(title);
    }
    if (dynasty && strlen(dynasty) > 0)
    {
        if (out_origin.length() > 0)
            out_origin += "·";
        out_origin += String(dynasty);
    }
    if (author && strlen(author) > 0)
    {
        if (out_origin.length() > 0)
            out_origin += "·";
        out_origin += String(author);
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] 今日诗词: %s / %s\n", out_content.c_str(), out_origin.c_str());
#endif

    return true;
}

// 渲染列表项（分号分隔的文本，带bullet点）
// 返回实际渲染的项数
static int render_list_items(
    const char *content, // 分号分隔的文本内容
    int16_t x,           // 起始x坐标
    int16_t y,           // 起始y坐标
    int16_t area_width,  // 可用宽度
    int16_t area_height, // 可用高度
    uint8_t fontSize,    // 字体大小
    uint8_t textColor,   // 文本颜色（0-15灰度）
    int16_t margin       // 行间距（像素）
)
{
    if (!content || strlen(content) == 0)
    {
        return 0;
    }

    // 计算行高
    uint8_t base_font_size = get_font_size_from_file();
    if (base_font_size == 0)
        base_font_size = 24;
    float scale_factor = (fontSize > 0) ? ((float)fontSize / (float)base_font_size) : 1.0f;
    int16_t line_height = (int16_t)(base_font_size * scale_factor) + margin;

    // 计算最多能显示几行
    int max_lines = area_height / line_height;
    if (max_lines <= 0)
        max_lines = 1;

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] 列表渲染: area_height=%d, fontSize=%d, base=%d, scale=%.2f, 行高=%d, margin=%d, 最大行数=%d\n",
                  area_height, fontSize, base_font_size, scale_factor, line_height, margin, max_lines);
#endif

    // 用分号分割文本内容
    String textStr = String(content);
    int item_count = 0;
    int16_t current_y = y;
    int start_pos = 0;

    while (item_count < max_lines)
    {
        // 查找下一个分号
        int semicolon_pos = textStr.indexOf(';', start_pos);
        String item;

        if (semicolon_pos == -1)
        {
            // 没有更多分号，取剩余部分
            item = textStr.substring(start_pos);
            if (item.length() == 0)
                break; // 空内容，结束
        }
        else
        {
            item = textStr.substring(start_pos, semicolon_pos);
            start_pos = semicolon_pos + 1; // 跳过分号
        }

        // 跳过空项
        item.trim();
        if (item.length() == 0)
        {
            if (semicolon_pos == -1)
                break; // 最后一项为空，结束
            continue;  // 跳过空项，继续下一项（不计入item_count）
        }

#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] 列表项%d: '%s' at y=%d (剩余行数=%d)\n", item_count + 1, item.c_str(), current_y, max_lines - item_count);
#endif

        // 绘制 bullet 点（实心圆 + 空心圆形成圆环）
        g_canvas->fillCircle(x, current_y, 6, TFT_BLACK);
        g_canvas->fillCircle(x, current_y, 3, TFT_WHITE);

        // 使用 bin_font_print 单行渲染文本
        bin_font_print(
            item.c_str(),             // text
            fontSize,                 // font_size
            textColor,                // color
            area_width,               // area_width
            x + 20,                   // x（bullet后留20像素）
            current_y - fontSize / 2, // y（垂直居中）
            false,                    // horizontal
            g_canvas,
            TEXT_ALIGN_LEFT, // align（左对齐）
            area_width,      // max_length
            false            // SkipConv
        );

        current_y += line_height;
        item_count++;

        // 如果是最后一项，结束
        if (semicolon_pos == -1)
            break;
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] 列表渲染完成，共%d项\n", item_count);
#endif

    return item_count;
}

// 辅助函数：在累积缓冲区中查找并提取一个完整的item title
// 返回值：找到返回item结束位置，未找到返回-1
static int extract_next_rss_item(String &buffer, bool is_atom, String &out_title)
{
    out_title = "";

    // 查找下一个item/entry起始标签
    int item_start = buffer.indexOf(is_atom ? "<entry>" : "<item>");
    int item_start_with_attr = buffer.indexOf(is_atom ? "<entry " : "<item ");

    if (item_start == -1 || (item_start_with_attr != -1 && item_start_with_attr < item_start))
    {
        item_start = item_start_with_attr;
    }

    if (item_start == -1)
        return -1; // 未找到item起始标签

    // 查找对应的结束标签
    const char *item_end_tag = is_atom ? "</entry>" : "</item>";
    int item_end_tag_len = is_atom ? 8 : 7;
    int item_end = buffer.indexOf(item_end_tag, item_start);

    if (item_end == -1)
        return -1; // item未完整（可能跨缓冲区边界），需要继续读取

    // 在item范围内查找<title>
    int title_start = buffer.indexOf("<title>", item_start);
    if (title_start != -1 && title_start < item_end)
    {
        title_start += 7; // 跳过"<title>"
        int title_end = buffer.indexOf("</title>", title_start);

        if (title_end != -1 && title_end < item_end)
        {
            String title = buffer.substring(title_start, title_end);

            // 处理CDATA
            if (title.startsWith("<![CDATA["))
            {
                title = title.substring(9);
                int cdata_end = title.indexOf("]]>");
                if (cdata_end >= 0)
                    title = title.substring(0, cdata_end);
            }

            title.trim();
            out_title = title;
        }
    }

    // 返回item结束位置（用于从buffer中删除已处理部分）
    return item_end + item_end_tag_len;
}

// 获取RSS feed并返回title列表（分号分隔）
static bool fetch_rss_feed(const String &url, String &out_titles)
{
    out_titles = "";

    // 检查WiFi连接
    if (!g_wifi_sta_connected)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] WiFi未连接，无法获取RSS");
#endif
        return false;
    }

    if (url.length() == 0)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] RSS URL为空");
#endif
        return false;
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] 获取RSS feed: %s\n", url.c_str());
#endif

    // 配置HTTP客户端
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 15000; // RSS可能较大，增加超时时间
    cfg.buffer_size = 8192; // 增加缓冲区
    cfg.buffer_size_tx = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach; // 支持HTTPS

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] HTTP客户端初始化失败");
#endif
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", "ReadPaper-RSS/1.0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] HTTP打开失败: %s\n", esp_err_to_name(err));
#endif
        esp_http_client_cleanup(client);
        return false;
    }

    err = esp_http_client_fetch_headers(client);
    if (err < 0)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] HTTP读取头失败: %s\n", esp_err_to_name(err));
#endif
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int status_code = esp_http_client_get_status_code(client);
#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] HTTP状态码: %d\n", status_code);
#endif

    if (status_code != 200)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] HTTP请求失败，状态码: %d\n", status_code);
#endif
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_get_content_length(client);
#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] RSS内容长度: %d\n", content_length);
#endif

    // 流式读取并解析RSS - 最多提取10个item
    const int MAX_ITEMS = 10;
    const int READ_BUFFER_SIZE = 2048;
    char read_buffer[READ_BUFFER_SIZE];
    String parse_buffer = ""; // 累积未完成解析的内容
    int total_read = 0;
    int item_count = 0;
    bool is_atom = false;
    bool format_detected = false;

    while (item_count < MAX_ITEMS)
    {
        // 读取一块数据
        int data_read = esp_http_client_read(client, read_buffer, READ_BUFFER_SIZE - 1);
        if (data_read < 0)
        {
#if DBG_TRMNL_SHOW
            Serial.println("[TRMNL] 读取RSS内容失败");
#endif
            break;
        }
        else if (data_read == 0)
        {
            // 文件读取完毕
            break;
        }

        read_buffer[data_read] = '\0';
        parse_buffer += String(read_buffer);
        total_read += data_read;

        // 检测RSS格式（只在第一次检测）
        if (!format_detected && parse_buffer.length() > 100)
        {
            is_atom = (parse_buffer.indexOf("<feed") >= 0 || parse_buffer.indexOf("<feed ") >= 0);
            format_detected = true;
#if DBG_TRMNL_SHOW
            Serial.printf("[TRMNL] 检测到RSS格式: %s\n", is_atom ? "Atom" : "RSS");
#endif
        }

        // 尝试从parse_buffer中提取完整的item
        while (item_count < MAX_ITEMS)
        {
            String title = "";
            int item_end_pos = extract_next_rss_item(parse_buffer, is_atom, title);

            if (item_end_pos == -1)
            {
                // 未找到完整item，需要继续读取
                // 为避免内存溢出且不丢失item标签，智能截断buffer
                if (parse_buffer.length() > 16384) // 16KB
                {
                    // 查找最后一个item起始标签的位置
                    int last_item_start = parse_buffer.lastIndexOf(is_atom ? "<entry>" : "<item>");
                    int last_item_start_attr = parse_buffer.lastIndexOf(is_atom ? "<entry " : "<item ");

                    // 取两者中较晚出现的
                    if (last_item_start < last_item_start_attr)
                        last_item_start = last_item_start_attr;

                    if (last_item_start > 0)
                    {
                        // 从最后一个item标签开始保留
                        parse_buffer = parse_buffer.substring(last_item_start);
#if DBG_TRMNL_SHOW
                        Serial.printf("[TRMNL] Buffer过大，从最后item标签截断，保留%d字节\n", parse_buffer.length());
#endif
                    }
                    else
                    {
                        // 找不到item标签，可能整个buffer都是垃圾数据，清空
                        parse_buffer = "";
#if DBG_TRMNL_SHOW
                        Serial.println("[TRMNL] Buffer中无item标签，清空");
#endif
                    }
                }
                break;
            }

            // 成功提取一个title
            if (title.length() > 0)
            {
                if (item_count > 0)
                    out_titles += ";";
                out_titles += title;
                item_count++;

#if DBG_TRMNL_SHOW
                Serial.printf("[TRMNL] RSS item %d: %s\n", item_count, title.c_str());
#endif
            }
#if DBG_TRMNL_SHOW
            else
            {
                Serial.printf("[TRMNL] 跳过空title的item (pos: %d)\n", item_end_pos);
            }
#endif

            // 从buffer中删除已处理的部分
            parse_buffer = parse_buffer.substring(item_end_pos);
#if DBG_TRMNL_SHOW
            Serial.printf("[TRMNL] Buffer剩余: %d字节\n", parse_buffer.length());
#endif

            // 如果已达到目标数量，提前退出
            if (item_count >= MAX_ITEMS)
            {
#if DBG_TRMNL_SHOW
                Serial.printf("[TRMNL] 已提取%d个item，停止读取 (共读取%d字节)\n", item_count, total_read);
#endif
                break;
            }
        }

        // 如果已找到足够的item，停止读取
        if (item_count >= MAX_ITEMS)
            break;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] RSS流式解析完成: 共读取%d字节, 提取%d个标题\n", total_read, item_count);
#endif

    return item_count > 0;
}

// 下载文件到 SD 卡
static bool download_file_to_sdcard(const String &url, const String &local_path, const String &auth_header)
{
#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] 下载文件: %s -> %s\n", url.c_str(), local_path.c_str());
#endif

    // 配置 HTTP 客户端
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;

    if (strlen(g_config.webdav_user) > 0 || strlen(g_config.webdav_pass) > 0)
    {
        cfg.auth_type = HTTP_AUTH_TYPE_BASIC;
        cfg.username = g_config.webdav_user;
        cfg.password = g_config.webdav_pass;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] HTTP 客户端初始化失败");
#endif
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", "ReadPaper-TRMNL");
    if (auth_header.length() > 0)
    {
        esp_http_client_set_header(client, "Authorization", auth_header.c_str());
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] HTTP 打开失败: %s\n", esp_err_to_name(err));
#endif
        esp_http_client_cleanup(client);
        return false;
    }

    err = esp_http_client_fetch_headers(client);
    if (err < 0)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] HTTP 读取头失败: %s\n", esp_err_to_name(err));
#endif
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] HTTP 状态码: %d\n", status_code);
#endif
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    // 确保目录存在
    // 提取目录路径
    String dir_path = local_path.substring(0, local_path.lastIndexOf('/'));
    if (!SDW::SD.exists(dir_path.c_str()))
    {
        SDW::SD.mkdir(dir_path.c_str());
    }

    // 删除已存在的文件
    if (SDW::SD.exists(local_path.c_str()))
    {
        SDW::SD.remove(local_path.c_str());
    }

    // 写入文件
    File file = SDW::SD.open(local_path.c_str(), "w");
    if (!file)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] 无法创建文件: %s\n", local_path.c_str());
#endif
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    char buffer[2048];
    int total_read = 0;
    while (true)
    {
        int data_read = esp_http_client_read(client, buffer, sizeof(buffer));
        if (data_read <= 0)
        {
            break;
        }
        file.write((uint8_t *)buffer, data_read);
        total_read += data_read;
    }

    file.close();
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] 文件下载成功，大小: %d 字节\n", total_read);
#endif

    return total_read > 0;
}

// 解析并显示 RDT 配置
static bool parse_and_display_rdt(M5Canvas *canvas, const String &content)
{
    // 解析 JSON
    DynamicJsonDocument doc(8192);
    DeserializationError error = deserializeJson(doc, content);

    if (error)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] JSON 解析失败: %s\n", error.c_str());
#endif
        return false;
    }

    // 检查版本
    const char *version = doc["version"] | "unknown";
#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] RDT 版本: %s\n", version);
#endif

    // 检查是否有背景图
    bool has_bgpic = doc["bgpic"] | false;

    // 清空画布
    bin_font_clear_canvas();

    // 加载背景图（如果存在）
    if (has_bgpic)
    {
        const char *bg_path = "/rdt/readpaper.png";
        if (SDW::SD.exists(bg_path))
        {
#if DBG_TRMNL_SHOW
            Serial.printf("[TRMNL] 加载背景图: %s\n", bg_path);
#endif
            // 使用 ui_push_image_to_canvas 加载背景图
            ui_push_image_to_canvas(bg_path, 0, 0);
        }
        else
        {
#if DBG_TRMNL_SHOW
            Serial.printf("[TRMNL] 背景图不存在: %s\n", bg_path);
#endif
        }
    }

    // 解析组件
    JsonArray components = doc["components"];
    if (components)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] 组件数量: %d\n", components.size());
#endif

        for (JsonObject component : components)
        {
            // 获取 dynamic 标志
            bool is_dynamic = component["dynamic"] | true;

            // 跳过静态组件（已预渲染到 PNG）
            if (!is_dynamic)
            {
#if DBG_TRMNL_SHOW
                Serial.println("[TRMNL] 跳过静态组件（已预渲染）");
#endif
                continue;
            }

            // 获取组件类型
            const char *type = component["type"] | "unknown";

#if DBG_TRMNL_SHOW
            Serial.printf("[TRMNL] 处理动态组件: %s\n", type);
#endif

            // 处理普通文本组件（dynamic_text）
            if (strcmp(type, "dynamic_text") == 0)
            {
                // 从嵌套结构读取属性
                // position: {x, y}
                int pos_x = 0;
                int pos_y = 0;
                int a_w = 0;
                int a_h = 0;
                if (component.containsKey("position"))
                {
                    JsonObject position = component["position"].as<JsonObject>();
                    pos_x = position["x"] | 0;
                    pos_y = position["y"] | 0;
                }
                JsonObject areaSize = component["size"].as<JsonObject>();
                a_w = areaSize["width"] | 1;
                a_h = areaSize["height"] | 1;

                // config: {text, fontSize, textColor, align, xOffset, yOffset, ...}
                const char *text = "文本";
                int fontSize = 24;
                int textColor = 0;
                const char *alignStr = "left";
                uint8_t align = 0; // 默认左对齐
                int xOffset = 0;   // x偏移量
                int yOffset = 0;   // y偏移量

                if (component.containsKey("config"))
                {
                    JsonObject config = component["config"].as<JsonObject>();
                    text = config["text"] | "文本";
                    fontSize = config["fontSize"] | 24;
                    textColor = config["textColor"] | 0; // 0-15 灰度
                    alignStr = config["align"] | "left";
                    xOffset = config["xOffset"] | 0;
                    yOffset = config["yOffset"] | 0;

                    // 映射对齐方式字符串到数字
                    if (strcmp(alignStr, "center") == 0)
                    {
                        align = 1;
                    }
                    else if (strcmp(alignStr, "right") == 0)
                    {
                        align = 2;
                    }
                }

                // 计算打印起点（单元格坐标转像素）
                // 540/9=60, 960/16=60，每个cell是60x60像素
                const int CELL_WIDTH = 60;
                const int CELL_HEIGHT = 60;
                int16_t x = pos_x * CELL_WIDTH + 20 + xOffset;
                int16_t y = pos_y * CELL_HEIGHT + yOffset;
                a_w = a_w * CELL_WIDTH - 40;
                a_h = a_h * CELL_HEIGHT;

#if DBG_TRMNL_SHOW
                Serial.printf("[TRMNL] 渲染普通文本: '%s' 单元格(%d, %d) 像素(%d, %d) 字号%d 颜色%d 宽度%d 高度%d 对齐%s\n",
                              text, pos_x, pos_y, x, y, fontSize, textColor, a_w, a_h, alignStr);
#endif

                // 使用 display_print_wrapped 进行自动换行打印
                display_print_wrapped(
                    text,      // text
                    x,         // x (起点)
                    y,         // y (起点)
                    a_w,       // area_width (可用宽度)
                    a_h,       // area_height (可用高度)
                    fontSize,  // font_size
                    textColor, // color (0-15 灰度)
                    15,        // bg_color (15=白色 #ffffff)
                    align,     // align (0=左，1=中，2=右)
                    false,     // vertical
                    false      // skip (不跳过繁简转换)
                );
            }
            // 处理今日诗词组件（daily_poem）
            else if (strcmp(type, "daily_poem") == 0)
            {
                // 从嵌套结构读取属性
                int pos_x = 0;
                int pos_y = 0;
                int a_w = 0;
                int a_h = 0;
                if (component.containsKey("position"))
                {
                    JsonObject position = component["position"].as<JsonObject>();
                    pos_x = position["x"] | 0;
                    pos_y = position["y"] | 0;
                }
                JsonObject areaSize = component["size"].as<JsonObject>();
                a_w = areaSize["width"] | 1;
                a_h = areaSize["height"] | 1;

                // config: {fontSize, textColor, align, xOffset, yOffset, ...}
                int fontSize = 24;
                int textColor = 0;
                const char *alignStr = "left";
                uint8_t align = 0;
                int xOffset = 0; // x偏移量
                int yOffset = 0; // y偏移量

                if (component.containsKey("config"))
                {
                    JsonObject config = component["config"].as<JsonObject>();
                    fontSize = config["fontSize"] | 24;
                    textColor = config["textColor"] | 0;
                    alignStr = config["align"] | "left";
                    xOffset = config["xOffset"] | 0;
                    yOffset = config["yOffset"] | 0;

                    // 映射对齐方式
                    if (strcmp(alignStr, "center") == 0)
                    {
                        align = 1;
                    }
                    else if (strcmp(alignStr, "right") == 0)
                    {
                        align = 2;
                    }
                }

                // 计算打印起点（单元格坐标转像素）
                const int CELL_WIDTH = 60;
                const int CELL_HEIGHT = 60;
                int16_t x = pos_x * CELL_WIDTH + 20 + xOffset;
                int16_t y = pos_y * CELL_HEIGHT + yOffset;
                a_w = a_w * CELL_WIDTH - 40;
                a_h = a_h * CELL_HEIGHT;

                // 获取今日诗词
                String poem_content;
                String poem_origin;
                if (!fetch_daily_poem(poem_content, poem_origin))
                {
                    // 获取失败，使用默认诗词
#if DBG_TRMNL_SHOW
                    Serial.println("[TRMNL] 获取今日诗词失败，使用默认诗词");
#endif
                    poem_content = "扣舷独啸，不知今夕何夕。";
                    poem_origin = "过洞庭·宋·张孝祥";
                }

#if DBG_TRMNL_SHOW
                Serial.printf("[TRMNL] 渲染今日诗词: 单元格(%d, %d) 像素(%d, %d) 字号%d 颜色%d 宽度%d 高度%d 对齐%s\n",
                              pos_x, pos_y, x, y, fontSize, textColor, a_w, a_h, alignStr);
#endif

                // 先打印正文（使用配置的字号）
                int used_lines = display_print_wrapped(
                    poem_content.c_str(), // text
                    x,                    // x (起点)
                    y,                    // y (起点)
                    a_w,                  // area_width (可用宽度)
                    a_h,                  // area_height (可用高度)
                    fontSize,             // font_size
                    textColor,            // color (0-15 灰度)
                    15,                   // bg_color (15=白色 #ffffff)
                    align,                // align (0=左，1=中，2=右)
                    false,                // vertical
                    false                 // skip (不跳过繁简转换)
                );

                // 计算剩余行数和出处字号
                uint8_t base_font_size = get_font_size_from_file();
                if (base_font_size == 0)
                    base_font_size = 24;
                float scale_factor = (fontSize > 0) ? ((float)fontSize / (float)base_font_size) : 1.0f;
                int16_t line_height = (int16_t)((base_font_size + LINE_MARGIN) * scale_factor);
                int max_lines = a_h / line_height;
                int remaining_lines = max_lines - used_lines;

                // 如果还有空余行且有出处信息，打印出处（80%字号，深灰色）
                if (remaining_lines > 0 && poem_origin.length() > 0)
                {
                    int16_t origin_y = y + used_lines * line_height;
                    uint8_t origin_font_size = (uint8_t)(fontSize * 0.8f);
                    uint8_t origin_color = textColor; // 深灰色

#if DBG_TRMNL_SHOW
                    Serial.printf("[TRMNL] 打印出处: y=%d, 字号=%d, 颜色=%d, 剩余行数=%d\n",
                                  origin_y, origin_font_size, origin_color, remaining_lines);
#endif

                    display_print_wrapped(
                        poem_origin.c_str(),           // text
                        x,                             // x (起点)
                        origin_y,                      // y (正文后)
                        a_w,                           // area_width
                        remaining_lines * line_height, // area_height (剩余高度)
                        origin_font_size,              // font_size (80%)
                        origin_color,                  // color (深灰色)
                        15,                            // bg_color (白色)
                        align,                         // align
                        false,                         // vertical
                        false                          // skip
                    );
                }
            }
            // 处理阅读状态组件（reading_status）
            else if (strcmp(type, "reading_status") == 0)
            {
                // 位置与大小
                int pos_x = 0;
                int pos_y = 0;
                int a_w = 0;
                int a_h = 0;
                if (component.containsKey("position"))
                {
                    JsonObject position = component["position"].as<JsonObject>();
                    pos_x = position["x"] | 0;
                    pos_y = position["y"] | 0;
                }
                JsonObject areaSize = component["size"].as<JsonObject>();
                a_w = areaSize["width"] | 1;
                a_h = areaSize["height"] | 1;

                // 配置项
                int fontSize = 24;
                int textColor = 0;
                const char *alignStr = "left";
                uint8_t align = 0;
                int xOffset = 0;
                int yOffset = 0;

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

                // 计算像素区域
                const int CELL_WIDTH = 60;
                const int CELL_HEIGHT = 60;
                int16_t x = pos_x * CELL_WIDTH + 20 + xOffset;
                int16_t y = pos_y * CELL_HEIGHT + yOffset;
                a_w = a_w * CELL_WIDTH - 40;
                a_h = a_h * CELL_HEIGHT;

                // 获取当前书名与章节名（若存在）
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
                        if (find_toc_entry_for_position(g_current_book->filePath(), tp.file_pos, entry_index, page, row, on_current))
                        {
                            std::string toc_title;
                            if (get_toc_title_for_index(g_current_book->filePath(), entry_index, toc_title))
                            {
                                chapter_name = toc_title;
                            }
                        }
                    }
                }

                // 使用 bin_font_print 绘制：先书名（fontSize），再换行绘制章节名（0.8*fontSize）
                if (!book_name.empty())
                {
                    // 书名
                    bin_font_print(book_name.c_str(), (uint8_t)fontSize, (uint8_t)textColor, a_w, x, y, false, nullptr, (TextAlign)align, a_w);

                    // 章节名（如果存在），使用 0.8 倍字号并在下一行绘制
                    if (!chapter_name.empty())
                    {
                        int chapSize = std::max(8, (int)(fontSize * 0.9f));
                        int16_t next_y = y + fontSize + 24; // 简单换行间距

                        // 追加阅读百分比：重用 BookHandle 的 position() 与 getFileSize()
                        std::string chapter_display = chapter_name;
                        if (g_current_book)
                        {
                            size_t total = g_current_book->getFileSize();
                            if (total > 0)
                            {
                                size_t pos = g_current_book->position();
                                int pct = (int)((double)pos / (double)total * 100.0 + 0.5); // 四舍五入到整数
                                chapter_display += std::string(" · ") + std::to_string(pct) + std::string("%");
                            }
                        }

                        bin_font_print(chapter_display.c_str(), (uint8_t)chapSize, (uint8_t)textColor, a_w, x, next_y, false, nullptr, (TextAlign)align, a_w);
                    }
                }
            }
            // 处理天气查询组件（weather）
            else if (strcmp(type, "weather") == 0)
            {
                // 从嵌套结构读取属性
                int pos_x = 0;
                int pos_y = 0;
                int a_w = 0;
                int a_h = 0;
                if (component.containsKey("position"))
                {
                    JsonObject position = component["position"].as<JsonObject>();
                    pos_x = position["x"] | 0;
                    pos_y = position["y"] | 0;
                }
                JsonObject areaSize = component["size"].as<JsonObject>();
                a_w = areaSize["width"] | 1;
                a_h = areaSize["height"] | 1;

                // config: {citycode, apiKey, fontSize, textColor, xOffset, yOffset, align}
                String citycode = "110000";
                String apiKey = "";
                int fontSize = 24;
                int textColor = 0;
                int xOffset = 0;
                int yOffset = 0;
                int align = 0; // 0=left

                if (component.containsKey("config"))
                {
                    JsonObject config = component["config"].as<JsonObject>();
                    citycode = config["citycode"] | "110000";
                    apiKey = config["apiKey"] | "";
                    fontSize = config["fontSize"] | 24;
                    textColor = config["textColor"] | 0;
                    xOffset = config["xOffset"] | 0;
                    yOffset = config["yOffset"] | 0;
                    align = config["align"] | 0;
                }

                // 计算打印起点（单元格坐标转像素）
                const int CELL_WIDTH = 60;
                const int CELL_HEIGHT = 60;
                int16_t x = pos_x * CELL_WIDTH + 20 + xOffset;
                int16_t y = pos_y * CELL_HEIGHT + yOffset;
                a_w = a_w * CELL_WIDTH - 40;
                a_h = a_h * CELL_HEIGHT;

                // 获取天气信息
                String today_info, tomorrow_info;
                if (fetch_weather(citycode, apiKey, today_info, tomorrow_info))
                {
                    // 先打印今天的天气（使用 fontSize）
                    int lines_used = display_print_wrapped(
                        today_info.c_str(), // text
                        x,                  // x (起点)
                        y,                  // y (起点)
                        a_w,                // area_width (可用宽度)
                        a_h,                // area_height (可用高度)
                        fontSize,           // font_size
                        textColor,          // color (0-15 灰度)
                        15,                 // bg_color (15=白色)
                        align,              // align (0=左，1=中，2=右)
                        false,              // vertical
                        false               // skip (不跳过繁简转换)
                    );

                    // 计算今天天气占用的高度
                    uint8_t base_font_size = get_font_size_from_file();
                    if (base_font_size == 0)
                        base_font_size = 24;
                    float scale_factor = (fontSize > 0) ? ((float)fontSize / (float)base_font_size) : 1.0f;
                    int16_t line_height = (int16_t)((base_font_size + 8) * scale_factor); // LINE_MARGIN=8
                    int16_t used_height = lines_used * line_height;
                    int16_t remaining_height = a_h - used_height;

                    // 如果剩余高度 > fontSize 且有明天的天气信息，则打印明天的天气（使用 0.8*fontSize）
                    if (remaining_height > fontSize && tomorrow_info.length() > 0)
                    {
                        int tomorrow_font_size = (int)(fontSize * 0.8f);
                        int16_t tomorrow_y = y + used_height + 30;

                        g_canvas->fillRect(x , tomorrow_y, 4, tomorrow_font_size, TFT_BLACK);

                        display_print_wrapped(
                            tomorrow_info.c_str(), // text
                            x + 8,                     // x (起点)
                            tomorrow_y,            // y (今天下方)
                            a_w,                   // area_width (可用宽度)
                            remaining_height,      // area_height (剩余高度)
                            tomorrow_font_size,    // font_size (0.8倍)
                            textColor,             // color (0-15 灰度)
                            15,                    // bg_color (15=白色)
                            align,                 // align (0=左，1=中，2=右)
                            false,                 // vertical
                            false                  // skip (不跳过繁简转换)
                        );
                    }
                }
            }
            // 处理列表组件（list）
            else if (strcmp(type, "list") == 0)
            {
                // 从嵌套结构读取属性
                int pos_x = 0;
                int pos_y = 0;
                int a_w = 0;
                int a_h = 0;
                if (component.containsKey("position"))
                {
                    JsonObject position = component["position"].as<JsonObject>();
                    pos_x = position["x"] | 0;
                    pos_y = position["y"] | 0;
                }
                JsonObject areaSize = component["size"].as<JsonObject>();
                a_w = areaSize["width"] | 1;
                a_h = areaSize["height"] | 1;

                // config: {text, fontSize, textColor, xOffset, yOffset, margin}
                const char *text = "";
                int fontSize = 24;
                int textColor = 0;
                int xOffset = 0;
                int yOffset = 0;
                int margin = 10; // 默认行间距10像素

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

                // 计算打印起点（单元格坐标转像素）
                const int CELL_WIDTH = 60;
                const int CELL_HEIGHT = 60;
                int16_t x = pos_x * CELL_WIDTH + 20 + xOffset;
                int16_t y = pos_y * CELL_HEIGHT + yOffset;
                a_w = a_w * CELL_WIDTH - 40;
                a_h = a_h * CELL_HEIGHT;

#if DBG_TRMNL_SHOW
                Serial.printf("[TRMNL] 渲染列表: 单元格(%d, %d) 像素(%d, %d) 字号%d 颜色%d 宽度%d 高度%d margin%d\n",
                              pos_x, pos_y, x, y, fontSize, textColor, a_w, a_h, margin);
#endif

                // 调用列表渲染函数
                render_list_items(text, x, y, a_w, a_h, fontSize, textColor, margin);
            }
            // 处理RSS组件（rss）
            else if (strcmp(type, "rss") == 0)
            {
                // 从嵌套结构读取属性
                int pos_x = 0;
                int pos_y = 0;
                int a_w = 0;
                int a_h = 0;
                if (component.containsKey("position"))
                {
                    JsonObject position = component["position"].as<JsonObject>();
                    pos_x = position["x"] | 0;
                    pos_y = position["y"] | 0;
                }
                JsonObject areaSize = component["size"].as<JsonObject>();
                a_w = areaSize["width"] | 1;
                a_h = areaSize["height"] | 1;

                // config: {url, fontSize, textColor, xOffset, yOffset, margin}
                const char *url = "";
                int fontSize = 24;
                int textColor = 0;
                int xOffset = 0;
                int yOffset = 0;
                int margin = 10; // 默认行间距10像素

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

                // 计算打印起点（单元格坐标转像素）
                const int CELL_WIDTH = 60;
                const int CELL_HEIGHT = 60;
                int16_t x = pos_x * CELL_WIDTH + 20 + xOffset;
                int16_t y = pos_y * CELL_HEIGHT + yOffset;
                a_w = a_w * CELL_WIDTH - 40;
                a_h = a_h * CELL_HEIGHT;

#if DBG_TRMNL_SHOW
                Serial.printf("[TRMNL] 渲染RSS: 单元格(%d, %d) 像素(%d, %d) 字号%d 颜色%d 宽度%d 高度%d margin%d\n",
                              pos_x, pos_y, x, y, fontSize, textColor, a_w, a_h, margin);
                Serial.printf("[TRMNL] RSS URL: %s\n", url);
#endif

                // 获取RSS feed内容
                String rss_titles = "";
                bool success = fetch_rss_feed(String(url), rss_titles);

                if (success && rss_titles.length() > 0)
                {
#if DBG_TRMNL_SHOW
                    Serial.printf("[TRMNL] RSS获取成功，标题列表: %s\n", rss_titles.c_str());
#endif
                    // 使用list渲染函数显示RSS标题列表
                    render_list_items(rss_titles.c_str(), x, y, a_w, a_h, fontSize, textColor, margin);
                }
                else
                {
#if DBG_TRMNL_SHOW
                    Serial.println("[TRMNL] RSS获取失败或内容为空");
#endif
                    // 显示错误提示
                    bin_font_print(
                        "RSS加载失败",
                        fontSize,
                        textColor,
                        a_w,
                        x,
                        y,
                        false,
                        g_canvas,
                        TEXT_ALIGN_LEFT,
                        a_w,
                        false);
                }
            }
            // TODO: 后续扩展其他动态组件的渲染（clock, barcode等）
        }
    }

    return true;
}

bool trmnl_display(M5Canvas *canvas)
{
    if (canvas == nullptr)
    {
        canvas = g_canvas;
    }

    // 步骤1: 先尝试读取本地 SD 卡的 RDT 文件
    String rdt_content;
    String local_timestamp;
    bool has_local_rdt = read_sdcard_rdt(rdt_content);
    if (has_local_rdt)
    {
        extract_rdt_timestamp(rdt_content, local_timestamp);
    }

#if DBG_TRMNL_SHOW
    if (has_local_rdt)
    {
        Serial.printf("[TRMNL] 本地 RDT 存在，时间戳: %s\n", local_timestamp.length() > 0 ? local_timestamp.c_str() : "(无)");
    }
    else
    {
        Serial.println("[TRMNL] 本地 SD 卡无 RDT 文件");
    }
#endif

    // 步骤2: 检查是否需要从 WebDAV 更新（只有配置了WebDAV时才检查）
    bool need_update_from_webdav = false;
    bool has_webdav_config = (strlen(g_config.webdav_url) > 0);
    
#if DBG_TRMNL_SHOW
    if (has_webdav_config)
    {
        Serial.println("[TRMNL] 检测到 WebDAV 配置，将检查是否需要更新");
    }
    else
    {
        Serial.println("[TRMNL] 未配置 WebDAV，跳过云端更新检查");
    }
#endif

    if (has_local_rdt && has_webdav_config)
    {
        // 本地有RDT且配置了WebDAV，检查是否需要更新
        String webdav_timestamp;
        if (fetch_webdav_rdt_timestamp(webdav_timestamp))
        {
            // 比较时间戳
            if (local_timestamp != webdav_timestamp)
            {
#if DBG_TRMNL_SHOW
                Serial.printf("[TRMNL] 时间戳不一致 (本地: %s, WebDAV: %s)，需要更新\n",
                              local_timestamp.c_str(), webdav_timestamp.c_str());
#endif
                need_update_from_webdav = true;
            }
            else
            {
#if DBG_TRMNL_SHOW
                Serial.println("[TRMNL] 时间戳一致，跳过 WebDAV 下载，使用本地 RDT");
#endif
            }
        }
        else
        {
#if DBG_TRMNL_SHOW
            Serial.println("[TRMNL] 无法获取 WebDAV 时间戳，使用本地 RDT");
#endif
        }
    }
    else if (!has_local_rdt && has_webdav_config)
    {
        // 本地没有 RDT 但配置了 WebDAV，尝试从 WebDAV 下载
        need_update_from_webdav = true;
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] 本地无 RDT，将尝试从 WebDAV 下载");
#endif
    }
    else if (!has_local_rdt && !has_webdav_config)
    {
        // 本地没有 RDT 且未配置 WebDAV，无法获取配置
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] 本地无 RDT 且未配置 WebDAV，将显示默认界面");
#endif
    }

    // 步骤3: 如果需要更新，从 WebDAV 下载最新的 RDT 和 PNG
    if (need_update_from_webdav)
    {
        if (fetch_webdav_rdt_config(rdt_content))
        {
#if DBG_TRMNL_SHOW
            Serial.println("[TRMNL] WebDAV 下载成功");
#endif
            // 准备认证头
            String webdav_user = String(g_config.webdav_user);
            String webdav_pass = String(g_config.webdav_pass);
            bool has_auth = (webdav_user.length() > 0 || webdav_pass.length() > 0);

            String auth_header;
            if (has_auth)
            {
                char auth_raw[160];
                snprintf(auth_raw, sizeof(auth_raw), "%s:%s", webdav_user.c_str(), webdav_pass.c_str());

                unsigned char b64[256] = {0};
                size_t out_len = 0;
                if (mbedtls_base64_encode(b64, sizeof(b64) - 1, &out_len,
                                          reinterpret_cast<const unsigned char *>(auth_raw),
                                          strlen(auth_raw)) == 0)
                {
                    b64[out_len] = '\0';
                    auth_header = String("Basic ") + reinterpret_cast<const char *>(b64);
                }
            }

            // 构建 WebDAV URL
            String base_url = String(g_config.webdav_url);
            if (!base_url.endsWith("/"))
            {
                base_url += "/";
            }

            // 确保 SD 卡目录存在
            if (!SDW::SD.exists("/rdt"))
            {
                SDW::SD.mkdir("/rdt");
            }

            // 保存 RDT 文件到 SD 卡
            const char *rdt_sd_path = "/rdt/readpaper.rdt";
            if (SDW::SD.exists(rdt_sd_path))
            {
                SDW::SD.remove(rdt_sd_path);
            }

            File file = SDW::SD.open(rdt_sd_path, "w");
            if (file)
            {
                file.print(rdt_content);
                file.close();
#if DBG_TRMNL_SHOW
                Serial.printf("[TRMNL] RDT 已保存到 SD 卡: %s\n", rdt_sd_path);
#endif
            }

            // 下载 PNG 文件（但不下载背景图 readpaper_0.png）
            String png_url = base_url + "readpaper/readpaper.png";
            String png_sd_path = "/rdt/readpaper.png";
            download_file_to_sdcard(png_url, png_sd_path, auth_header);
        }
        else
        {
#if DBG_TRMNL_SHOW
            Serial.println("[TRMNL] WebDAV 下载失败");
#endif
            // 如果 WebDAV 下载失败且本地没有 RDT，显示默认界面
            if (!has_local_rdt)
            {
#if DBG_TRMNL_SHOW
                Serial.println("[TRMNL] WebDAV 和本地 SD 卡都没有 RDT，使用默认显示");
#endif
                return show_default_trmnl(canvas);
            }
            // 否则继续使用本地 RDT（已在 rdt_content 中）
        }
    }

    // 步骤3.5: 在渲染RDT之前，确保WiFi已连接（为动态联网组件提供网络支持）
    // 无论是否配置了WebDAV，WiFi都应保持到RDT渲染完成
#if DBG_TRMNL_SHOW
    Serial.println("[TRMNL] ===== 准备渲染RDT，检查WiFi连接状态 =====");
#endif
    
    if (!g_wifi_sta_connected && g_wifi_hotspot)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] WiFi当前未连接，尝试连接WiFi以支持动态组件（天气、RSS等）...");
#endif
        g_wifi_hotspot->connectToWiFiFromToken();
        
        // 不论连接成功或失败，都继续后续处理
#if DBG_TRMNL_SHOW
        if (g_wifi_sta_connected)
        {
            Serial.println("[TRMNL] ✓ WiFi连接成功，动态组件可正常工作");
        }
        else
        {
            Serial.println("[TRMNL] ✗ WiFi连接失败，动态组件可能无法工作");
        }
#endif
    }
#if DBG_TRMNL_SHOW
    else if (g_wifi_sta_connected)
    {
        Serial.println("[TRMNL] ✓ WiFi已连接，动态组件可正常工作");
    }
    else
    {
        Serial.println("[TRMNL] ⚠ 没有WiFi管理器，动态组件可能无法工作");
    }
    
    Serial.println("[TRMNL] ===== 开始渲染RDT配置 =====");
#endif

    // 步骤4: 解析并显示 SD 卡的 RDT 配置
    if (parse_and_display_rdt(canvas, rdt_content))
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] RDT 配置显示成功");
#endif
        return true;
    }

    // 解析失败，使用默认显示
#if DBG_TRMNL_SHOW
    Serial.println("[TRMNL] RDT 解析失败，使用默认显示");
#endif
    return show_default_trmnl(canvas);
}

bool show_default_trmnl(M5Canvas *canvas)
{
    if (canvas == nullptr)
    {
        canvas = g_canvas;
    }

    if (!canvas || !g_wifi_hotspot)
    {
        return false;
    }

    // 清空画布
    bin_font_clear_canvas();

    ui_push_image_to_canvas("/spiffs/screenlow.png", 0, 0);
    // 标题区域
    const int16_t title_y = 60;
    const int16_t content_start_y = 80;
    const int16_t line_height = 50;

    // 显示标题
    bin_font_print("无线连接状态", 36, TFT_BLACK, PAPER_S3_WIDTH, 30, title_y, false, canvas, TEXT_ALIGN_LEFT);

    // 分隔线
    canvas->drawWideLine(0, title_y + 45, PAPER_S3_WIDTH - 80, title_y + 45, 1.2f, TFT_BLACK);

    int16_t current_y = content_start_y;

    // 根据连接状态显示不同信息
    if (g_wifi_sta_connected)
    {
        // 检查是否配置了 WebDAV（通过 webdav_url 判断）
        bool has_webdav = (strlen(g_config.webdav_url) > 0);

        if (has_webdav)
        {
            // ===== WebDAV 云同步模式 =====
            current_y += line_height;

            // 显示连接的 WiFi SSID（显示最近成功连接的，或第一个非空的）
            int wifi_idx = g_config.wifi_last_success_idx >= 0 ? g_config.wifi_last_success_idx : 0;
            for (int i = 0; i < 3; i++)
            {
                int check_idx = (wifi_idx + i) % 3;
                if (strlen(g_config.wifi_ssid[check_idx]) > 0)
                {
                    std::string wifi_text = "WiFi: ";
                    wifi_text += g_config.wifi_ssid[check_idx];
                    if (check_idx == g_config.wifi_last_success_idx)
                    {
                        wifi_text += "";
                    }
                    bin_font_print(wifi_text, 28, TFT_BLACK,
                                   PAPER_S3_WIDTH - 160, 30, current_y, false, canvas, TEXT_ALIGN_LEFT);
                    current_y += line_height;
                    break;
                }
            }

            // 显示 IP 地址
            std::string ip_text = "IP地址: ";
            ip_text += g_wifi_hotspot->getIPAddress().c_str();
            bin_font_print(ip_text, 28, TFT_BLACK,
                           PAPER_S3_WIDTH - 160, 30, current_y, false, canvas, TEXT_ALIGN_LEFT);
            current_y += line_height;

            // 显示标题
            //            bin_font_print("WebDaV状态", 36, TFT_BLACK, PAPER_S3_WIDTH - 30, 0, current_y, false, canvas, TEXT_ALIGN_RIGHT);
            // 分隔线
            // canvas->drawLine(50, current_y + 45, PAPER_S3_WIDTH - 80, current_y + 45, TFT_BLACK);

            // 显示 WebDAV 服务器地址
            current_y += 320;
            bin_font_print("WebDAV 服务器:", 24, TFT_BLACK,
                           PAPER_S3_WIDTH - 30, 0, current_y, false, canvas, TEXT_ALIGN_RIGHT);
            current_y += 35;
            canvas->drawLine(50, current_y, PAPER_S3_WIDTH, current_y, TFT_BLACK);
            canvas->drawWideLine(270, current_y, PAPER_S3_WIDTH, current_y, 1.2f, TFT_BLACK);
            current_y += 24;
            bin_font_print(g_config.webdav_url, 24, TFT_BLACK,
                           PAPER_S3_WIDTH - 30, 0, current_y, false, canvas, TEXT_ALIGN_RIGHT);
            current_y += 40;

            // 显示用户名
            if (strlen(g_config.webdav_user) > 0)
            {
                std::string user_text = "用户: ";
                user_text += g_config.webdav_user;
                bin_font_print(user_text, 24, TFT_BLACK,
                               PAPER_S3_WIDTH - 30, 0, current_y, false, canvas, TEXT_ALIGN_RIGHT);
                current_y += line_height;
            }
            canvas->drawLine(0, 760, PAPER_S3_WIDTH - 60, 760, TFT_BLACK);
            canvas->drawWideLine(0, 760, PAPER_S3_WIDTH - 360, 760, 1.2f, TFT_BLACK);
            // 同步状态提示
            bin_font_print("设置就绪，请通过扩展配置显示。", 24, TFT_BLACK,
                           PAPER_S3_WIDTH - 30, 30, 780, false, canvas, TEXT_ALIGN_LEFT);
        }
        else
        {
            // ===== WebDAV 云同步模式 =====
            current_y += line_height;

            // 显示连接的 WiFi SSID（显示最近成功连接的，或第一个非空的）
            int wifi_idx = g_config.wifi_last_success_idx >= 0 ? g_config.wifi_last_success_idx : 0;
            for (int i = 0; i < 3; i++)
            {
                int check_idx = (wifi_idx + i) % 3;
                if (strlen(g_config.wifi_ssid[check_idx]) > 0)
                {
                    std::string wifi_text = "WiFi: ";
                    wifi_text += g_config.wifi_ssid[check_idx];
                    if (check_idx == g_config.wifi_last_success_idx)
                    {
                        wifi_text += "";
                    }
                    bin_font_print(wifi_text, 28, TFT_BLACK,
                                   PAPER_S3_WIDTH - 160, 30, current_y, false, canvas, TEXT_ALIGN_LEFT);
                    current_y += line_height;
                    break;
                }
            }

            // 显示 IP 地址
            std::string ip_text = "IP地址: ";
            ip_text += g_wifi_hotspot->getIPAddress().c_str();
            bin_font_print(ip_text, 28, TFT_BLACK,
                           PAPER_S3_WIDTH - 160, 30, current_y, false, canvas, TEXT_ALIGN_LEFT);
            current_y += line_height;

            // 显示 WebDAV 服务器地址
            current_y += 320;
            bin_font_print("WebDAV 服务器:", 24, TFT_BLACK,
                           PAPER_S3_WIDTH - 30, 0, current_y, false, canvas, TEXT_ALIGN_RIGHT);
            current_y += 35;
            canvas->drawLine(50, current_y, PAPER_S3_WIDTH, current_y, TFT_BLACK);
            canvas->drawWideLine(270, current_y, PAPER_S3_WIDTH, current_y, 1.2f, TFT_BLACK);
            current_y += 24;
            bin_font_print("没有有效WebDAV配置", 24, TFT_BLACK,
                           PAPER_S3_WIDTH - 30, 0, current_y, false, canvas, TEXT_ALIGN_RIGHT);
            current_y += 40;
        }
    }
    else
    {
        // ===== WebDAV 云同步模式 =====
        current_y += line_height;
        bin_font_print("WiFi 未连接", 28, TFT_BLACK,
                       PAPER_S3_WIDTH - 160, 30, current_y, false, canvas, TEXT_ALIGN_LEFT);
        current_y += line_height;
    }
    // 操作提示
    bin_font_print("点击屏幕返回菜单", 24, TFT_BLACK,
                   PAPER_S3_WIDTH, 0, PAPER_S3_HEIGHT - 80, false, canvas, TEXT_ALIGN_CENTER);
    canvas->drawLine(120, PAPER_S3_HEIGHT - 50, PAPER_S3_WIDTH - 120, PAPER_S3_HEIGHT - 50, TFT_BLACK);

    return true;
}

// 获取天气信息（高德天气API）
static bool fetch_weather(const String &citycode, const String &apiKey, String &out_today_info, String &out_tomorrow_info)
{
    // 检查 WiFi 连接
    if (!g_wifi_sta_connected)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] WiFi 未连接，无法获取天气信息");
#endif
        return false;
    }

    // 检查 apiKey 是否有效
    if (apiKey.length() == 0)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] API Key 为空，无法获取天气信息");
#endif
        return false;
    }

    // 构建 API URL：extensions=all 获取预报信息
    String api_url = "https://restapi.amap.com/v3/weather/weatherInfo?city=" + citycode +
                     "&key=" + apiKey + "&extensions=all";

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] 请求天气 API: %s\n", api_url.c_str());
#endif

    // 配置 HTTP 客户端
    esp_http_client_config_t cfg = {};
    cfg.url = api_url.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 8192;
    cfg.buffer_size_tx = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] 创建 HTTP 客户端失败");
#endif
        return false;
    }

    // 打开连接
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] HTTP 打开连接失败: %s\n", esp_err_to_name(err));
#endif
        esp_http_client_cleanup(client);
        return false;
    }

    // 获取响应头
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] API 返回错误状态码: %d\n", status);
#endif
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    // 读取响应内容
    String response_content;
    response_content.reserve(8192);

    char buffer[512];
    int total_read = 0;

    // 使用 read_response 读取完整响应体
    while (true)
    {
        int read_len = esp_http_client_read_response(client, buffer, sizeof(buffer) - 1);
        if (read_len <= 0)
            break;
        buffer[read_len] = '\0';
        response_content += buffer;
        total_read += read_len;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // 检查是否读取到内容
    if (total_read == 0 || response_content.length() == 0)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] API 返回空内容");
#endif
        return false;
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] 天气 API 响应长度: %d\n", response_content.length());
    Serial.printf("[TRMNL] 响应内容: %s\n", response_content.c_str());
#endif

    // 解析 JSON
    DynamicJsonDocument doc(12288);
    DeserializationError error = deserializeJson(doc, response_content);

    if (error)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] JSON 解析失败: %s\n", error.c_str());
#endif
        return false;
    }

    // 检查 status 字段
    const char *status_str = doc["status"];
    if (!status_str || strcmp(status_str, "1") != 0)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[TRMNL] API 返回错误状态: %s\n", status_str ? status_str : "(null)");
        const char *info = doc["info"];
        if (info)
        {
            Serial.printf("[TRMNL] 错误信息: %s\n", info);
        }
#endif
        return false;
    }

    // 提取预报信息
    JsonArray forecasts = doc["forecasts"].as<JsonArray>();
    if (!forecasts || forecasts.size() == 0)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] 未找到 forecasts 数组");
#endif
        return false;
    }

    JsonObject forecast = forecasts[0].as<JsonObject>();
    if (!forecast)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] forecasts[0] 为空");
#endif
        return false;
    }

    JsonArray casts = forecast["casts"].as<JsonArray>();
    if (!casts || casts.size() == 0)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] 未找到 casts 数组");
        Serial.println("[TRMNL] 完整响应内容:");
        Serial.println(response_content.c_str());
        Serial.printf("[TRMNL] forecast 对象包含的字段: ");
        for (JsonPair kv : forecast) {
            Serial.printf("%s ", kv.key().c_str());
        }
        Serial.println();
#endif
        return false;
    }

    // 提取今天的天气信息
    JsonObject today = casts[0].as<JsonObject>();
    if (!today)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[TRMNL] casts[0] 为空");
#endif
        return false;
    }

    const char *dayweather = today["dayweather"];
    const char *daytemp = today["daytemp"];
    const char *nighttemp = today["nighttemp"];
    const char *daywind = today["daywind"];
    const char *daypower = today["daypower"];

    // 格式化今天的天气信息：天气 温度 风力
    String today_info = "";
    if (dayweather && strlen(dayweather) > 0)
    {
        today_info += dayweather;
    }
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

    // 提取明天的天气信息（如果有）
    out_tomorrow_info = "";
    if (casts.size() > 1)
    {
        JsonObject tomorrow = casts[1].as<JsonObject>();
        if (tomorrow)
        {
            const char *tmr_dayweather = tomorrow["dayweather"];
            const char *tmr_daytemp = tomorrow["daytemp"];
            const char *tmr_nighttemp = tomorrow["nighttemp"];

            String tomorrow_info = "明天: ";
            if (tmr_dayweather && strlen(tmr_dayweather) > 0)
            {
                tomorrow_info += tmr_dayweather;
            }
            if (tmr_daytemp && tmr_nighttemp)
            {
                tomorrow_info += " ";
                tomorrow_info += tmr_nighttemp;
                tomorrow_info += "~";
                tomorrow_info += tmr_daytemp;
                tomorrow_info += "℃";
            }

            out_tomorrow_info = tomorrow_info;
        }
    }

#if DBG_TRMNL_SHOW
    Serial.printf("[TRMNL] 今天天气: %s\n", out_today_info.c_str());
    if (out_tomorrow_info.length() > 0)
    {
        Serial.printf("[TRMNL] 明天天气: %s\n", out_tomorrow_info.c_str());
    }
#endif

    return true;
}
