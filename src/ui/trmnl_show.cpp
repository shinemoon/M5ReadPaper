#include "trmnl_show.h"
#include "readpaper.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "device/wifi_hotspot_manager.h"
#include "globals.h"
#include "config/config_manager.h"
#include "test/per_file_debug.h"
#include <string>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <mbedtls/base64.h>
#include "ui/ui_canvas_image.h"

extern M5Canvas *g_canvas;
extern WiFiHotspotManager *g_wifi_hotspot;
extern bool g_wifi_sta_connected;
extern GlobalConfig g_config;

// 从 WebDAV 读取 readpaper.rdt 文件内容
static bool fetch_webdav_rdt_config(String &out_content)
{
    // 检查 WiFi 连接
    if (!g_wifi_sta_connected)
    {
#if DBG_STATE_MACHINE_TASK
        Serial.println("[TRMNL] WiFi 未连接，无法读取 WebDAV 配置");
#endif
        return false;
    }

    // 检查 WebDAV 配置
    if (strlen(g_config.webdav_url) == 0)
    {
#if DBG_STATE_MACHINE_TASK
        Serial.println("[TRMNL] WebDAV 未配置");
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

#if DBG_STATE_MACHINE_TASK
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
#if DBG_STATE_MACHINE_TASK
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
#if DBG_STATE_MACHINE_TASK
        Serial.printf("[TRMNL] HTTP 打开失败: %s\n", esp_err_to_name(err));
#endif
        esp_http_client_cleanup(client);
        return false;
    }

    err = esp_http_client_fetch_headers(client);
    if (err < 0)
    {
#if DBG_STATE_MACHINE_TASK
        Serial.printf("[TRMNL] HTTP 读取头失败: %s\n", esp_err_to_name(err));
#endif
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int status_code = esp_http_client_get_status_code(client);
#if DBG_STATE_MACHINE_TASK
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

#if DBG_STATE_MACHINE_TASK
    Serial.printf("[TRMNL] 读取成功，长度: %d\n", out_content.length());
#endif

    return out_content.length() > 0;
}

// 解析并显示 RDT 配置
static bool parse_and_display_rdt(M5Canvas *canvas, const String &content)
{
    // TODO: 实现 RDT 文件格式解析
    // 目前默认返回 false，等待后续扩展
#if DBG_STATE_MACHINE_TASK
    Serial.println("[TRMNL] RDT 解析功能尚未实现，使用默认显示");
#endif
    return false;
}

bool trmnl_display(M5Canvas *canvas)
{
    if (canvas == nullptr)
    {
        canvas = g_canvas;
    }

    // 尝试从 WebDAV 读取配置
    String rdt_content;
    if (fetch_webdav_rdt_config(rdt_content))
    {
        // 尝试解析并显示
        if (parse_and_display_rdt(canvas, rdt_content))
        {
#if DBG_STATE_MACHINE_TASK
            Serial.println("[TRMNL] 使用 WebDAV RDT 配置显示");
#endif
            return true;
        }
    }

    // 任何失败情况，使用默认显示
#if DBG_STATE_MACHINE_TASK
    Serial.println("[TRMNL] 使用默认显示");
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
