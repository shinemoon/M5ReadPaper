#include "api_router.h"
#include "device/wifi_hotspot_manager.h"
#include <Arduino.h>
#include <SPIFFS.h>

// Helper to add common CORS headers for JSON endpoints
static inline void add_cors_headers(WebServer& server) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS, DELETE");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type, X-Requested-With");
}

void ApiRouter::registerRoutes(WebServer& server, WiFiHotspotManager& mgr) {
    // Root (可选)：仍可返回内置/模板页面，便于本地调试
    server.on("/", [&](){ mgr.handleRoot(); });

    // 文件列表（保持与前端兼容的路径）
    server.on("/list", [&](){ add_cors_headers(server); mgr.handleFileList(""); });
    server.on("/list/book", [&](){ add_cors_headers(server); mgr.handleFileList("book"); });
    server.on("/list/font", [&](){ add_cors_headers(server); mgr.handleFileList("font"); });
    server.on("/list/image", [&](){ add_cors_headers(server); mgr.handleFileList("image"); });
    server.on("/list/screenshot", [&](){ add_cors_headers(server); mgr.handleFileList("screenshot"); });

    // OPTIONS 预检支持（Chrome 扩展场景下某些 fetch 可能触发）
    server.on("/list", HTTP_OPTIONS, [&](){ add_cors_headers(server); server.send(204); });
    server.on("/list/book", HTTP_OPTIONS, [&](){ add_cors_headers(server); server.send(204); });
    server.on("/list/font", HTTP_OPTIONS, [&](){ add_cors_headers(server); server.send(204); });
    server.on("/list/image", HTTP_OPTIONS, [&](){ add_cors_headers(server); server.send(204); });
    server.on("/list/screenshot", HTTP_OPTIONS, [&](){ add_cors_headers(server); server.send(204); });

    // 上传：GET 提示、POST 实际上传（回调在 mgr.handleFileUploadPost 中完成响应）
    server.on("/upload", HTTP_GET, [&](){
        add_cors_headers(server);
        mgr.handleFileUpload();
    });
    server.on("/upload", HTTP_POST,
        [](){ /* 响应在上传回调中发送 */ },
        [&](){ mgr.handleFileUploadPost(); }
    );

    // 允许跨域预检
    server.on("/upload", HTTP_OPTIONS, [&](){
        add_cors_headers(server);
        server.send(204);
    });

    // 删除与下载
    server.on("/delete", [&](){ add_cors_headers(server); mgr.handleFileDelete(); });
    server.on("/delete", HTTP_OPTIONS, [&](){ add_cors_headers(server); server.send(204); });
    server.on("/download", [&](){ add_cors_headers(server); mgr.handleFileDownload(); });
    server.on("/download", HTTP_OPTIONS, [&](){ add_cors_headers(server); server.send(204); });

    // 同步时间：独立实现（保持原逻辑），返回文本但带上 CORS 允许跨域
    server.on("/sync_time", HTTP_POST, [&](){
        String body = server.arg("plain");

        // try to extract a numeric timestamp
        long long ts = 0;
        int idx = body.indexOf("timestamp");
        if (idx >= 0) {
            int col = body.indexOf(':', idx);
            if (col >= 0) {
                int i = col + 1;
                while (i < (int)body.length() && (body[i] == ' ' || body[i] == '"')) i++;
                String num;
                while (i < (int)body.length() && isDigit(body[i])) { num += body[i]; i++; }
                if (num.length() > 0) ts = num.toInt();
            }
        }

        // optional tzOffsetMinutes
        int tzOffsetMinutes = 0;
        int idx_tz = body.indexOf("tzOffsetMinutes");
        if (idx_tz >= 0) {
            int col = body.indexOf(':', idx_tz);
            if (col >= 0) {
                int i = col + 1;
                while (i < (int)body.length() && (body[i] == ' ' || body[i] == '"')) i++;
                String num;
                if (i < (int)body.length() && (body[i] == '+' || body[i] == '-')) { num += body[i]; i++; }
                while (i < (int)body.length() && isDigit(body[i])) { num += body[i]; i++; }
                if (num.length() > 0) tzOffsetMinutes = num.toInt();
            }
        }

    add_cors_headers(server);

        if (ts > 0) {
            struct timeval tv; tv.tv_sec = (time_t)ts; tv.tv_usec = 0; settimeofday(&tv, nullptr);
            if (tzOffsetMinutes != 0) {
                int absMin = abs(tzOffsetMinutes);
                int hh = absMin / 60; int mm = absMin % 60;
                String posix = "CST"; posix += (tzOffsetMinutes <= 0) ? "-" : "+"; posix += String(hh);
                if (mm != 0) { char buf[8]; snprintf(buf, sizeof(buf), ":%02d", mm); posix += String(buf); }
                setenv("TZ", posix.c_str(), 1); tzset();
            } else { setenv("TZ", "CST-8", 1); tzset(); }

            time_t now = tv.tv_sec; struct tm local_tm; localtime_r(&now, &local_tm);
            char local_buf[64]; strftime(local_buf, sizeof(local_buf), "%Y-%m-%d %H:%M:%S LOCAL", &local_tm);
            server.send(200, "text/plain", String("Time synced: ") + String(ts) + " (" + String(local_buf) + ")");
        } else {
            server.send(400, "text/plain", "Invalid timestamp");
        }
    });

    server.on("/sync_time", HTTP_OPTIONS, [&](){
        add_cors_headers(server);
        server.send(204);
    });

    // Heartbeat endpoint for frontend health checks
    server.on("/heartbeat", HTTP_GET, [&](){
        add_cors_headers(server);

        // Default values for webapp when /version is missing or unreadable
        String hw = "M5Stack PaperS3";
        String firmware = "ReadPaper";
        String version = "V1.3";

        // Try to read /version from SPIFFS. Expect first line -> hw, second -> firmware, third -> version
        if (SPIFFS.exists("/version")) {
            File vf = SPIFFS.open("/version", "r");
            if (vf) {
                int lineNo = 0;
                while (vf.available() && lineNo < 3) {
                    String line = vf.readStringUntil('\n');
                    // trim CR
                    while (line.endsWith("\r")) line.remove(line.length() - 1);
                    if (lineNo == 0) hw = line.length() ? line : hw;
                    else if (lineNo == 1) firmware = line.length() ? line : firmware;
                    else if (lineNo == 2) version = line.length() ? line : version;
                    lineNo++;
                }
                vf.close();
            }
        }

        String payload = String("{\"status\":\"ok\",\"hw\":\"") + hw + String("\",\"firmware\":\"") + firmware + String("\",\"version\":\"") + version + String("\"}");
        server.send(200, "application/json", payload);
    });

    server.on("/heartbeat", HTTP_OPTIONS, [&](){
        add_cors_headers(server);
        server.send(204);
    });

    // Reading records API endpoint
    server.on("/api/reading_records", HTTP_GET, [&](){
        add_cors_headers(server);
        mgr.handleReadingRecords();
    });

    server.on("/api/reading_records", HTTP_OPTIONS, [&](){
        add_cors_headers(server);
        server.send(204);
    });

    // WebDAV config endpoints
    server.on("/api/webdav_config", HTTP_GET, [&](){
        add_cors_headers(server);
        mgr.handleWebdavConfigGet();
    });
    server.on("/api/webdav_config", HTTP_POST, [&](){
        add_cors_headers(server);
        mgr.handleWebdavConfigUpdate();
    });
    server.on("/api/webdav_config", HTTP_OPTIONS, [&](){
        add_cors_headers(server);
        server.send(204);
    });

    // WiFi config endpoints
    server.on("/api/wifi_config", HTTP_GET, [&](){
        add_cors_headers(server);
        mgr.handleWifiConfigGet();
    });
    server.on("/api/wifi_config", HTTP_POST, [&](){
        add_cors_headers(server);
        mgr.handleWifiConfigUpdate();
    });
    server.on("/api/wifi_config", HTTP_OPTIONS, [&](){
        add_cors_headers(server);
        server.send(204);
    });

    // 更新设备显示配置（接收 JSON { rdt: string, png_base64: string }）
    server.on("/api/update_display", HTTP_POST, [&](){
        add_cors_headers(server);
        mgr.handleUpdateDisplay();
    });
    server.on("/api/update_display", HTTP_OPTIONS, [&](){
        add_cors_headers(server);
        server.send(204);
    });

    // 分块上传 API
    server.on("/api/update_display_start", HTTP_POST, [&](){
        add_cors_headers(server);
        mgr.handleUpdateDisplayStart();
    });
    server.on("/api/update_display_start", HTTP_OPTIONS, [&](){
        add_cors_headers(server);
        server.send(204);
    });

    server.on("/api/update_display_chunk", HTTP_POST, [&](){
        add_cors_headers(server);
        mgr.handleUpdateDisplayChunk();
    });
    server.on("/api/update_display_chunk", HTTP_OPTIONS, [&](){
        add_cors_headers(server);
        server.send(204);
    });

    server.on("/api/update_display_commit", HTTP_POST, [&](){
        add_cors_headers(server);
        mgr.handleUpdateDisplayCommit();
    });
    server.on("/api/update_display_commit", HTTP_OPTIONS, [&](){
        add_cors_headers(server);
        server.send(204);
    });
}
