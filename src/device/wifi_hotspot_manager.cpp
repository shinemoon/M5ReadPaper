#include "wifi_hotspot_manager.h"
#include "readpaper.h"
#include "test/per_file_debug.h"
#include "efficient_file_scanner.h"
#include "file_manager.h"
#include <Arduino.h>
#include "book_file_manager.h"
#include "config/config_manager.h"
#include "globals.h"
#include "current_book.h"
#include "SD/SDWrapper.h"
#include "api/api_router.h"
#include "internal_fs.h"
#include <SPIFFS.h>
#include "text/book_handle.h"
#include "text/tags_handle.h"
#include "device/safe_fs.h"
#include <set>
#include <map>
#include "ui/ui_lock_screen.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <mbedtls/base64.h>
#include "ui/trmnlComponents/comp_history_cache.h"

extern GlobalConfig g_config;

// Helper: collapse duplicate slashes and remove trailing slash (except root)
static std::string normalize_real_path(const std::string &p) {
    if (p.empty()) return std::string();
    std::string out;
    bool last_slash = false;
    for (char c : p) {
        if (c == '/') {
            if (!last_slash) {
                out.push_back('/');
                last_slash = true;
            }
        } else {
            out.push_back(c);
            last_slash = false;
        }
    }
    if (out.length() > 1 && out.back() == '/') out.pop_back();
    return out;
}

// 全局实例
WiFiHotspotManager* g_wifi_hotspot = nullptr;

WiFiHotspotManager::WiFiHotspotManager() 
    : webServer(nullptr), running(false), uploadInProgress(false), lastStopMs(0), stopInProgress(false) {
    // 初始化SPIFFS用于读取模板
    if (!InternalFS::begin(true)) {
#if DBG_WIFI_HOTSPOT
        Serial.println("[WIFI_HOTSPOT] 内部存储初始化失败！");
#endif
    } else {
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] %s 初始化成功。\n", InternalFS::label());
#endif
    }
}

WiFiHotspotManager::~WiFiHotspotManager() {
    stop();
    if (webServer) {
        delete webServer;
        webServer = nullptr;
    }
}

bool WiFiHotspotManager::start(const char* ssid, const char* password) {
    if (running || stopInProgress) {
        return true; // 已经在运行
    }

    // 防止 stop 后立即 start，给 WiFi/LwIP 网络栈一点恢复时间。
    const uint32_t now = millis();
    const uint32_t sinceStop = now - lastStopMs;
    if (lastStopMs != 0 && sinceStop < 1200) {
        delay(1200 - sinceStop);
    }

    // 使用默认值或用户提供的值
    currentSSID = ssid ? String(ssid) : String(DEFAULT_SSID);
    currentPassword = password ? String(password) : String(DEFAULT_PASSWORD);

#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] 正在启动WiFi热点...\n");
    Serial.printf("[WIFI_HOTSPOT] SSID: %s\n", currentSSID.c_str());
    Serial.printf("[WIFI_HOTSPOT] Password: %s\n", currentPassword.c_str());
    // 打印详细内存统计
    Serial.printf("[WIFI_HOTSPOT] 启动前内存状态:\n");
    Serial.printf("[WIFI_HOTSPOT]   DRAM Free: %u bytes (%.2f KB)\n", 
                  esp_get_free_heap_size(), esp_get_free_heap_size() / 1024.0);
    Serial.printf("[WIFI_HOTSPOT]   DRAM Min Free: %u bytes (%.2f KB)\n",
                  esp_get_minimum_free_heap_size(), esp_get_minimum_free_heap_size() / 1024.0);
    Serial.printf("[WIFI_HOTSPOT]   PSRAM Free: %u bytes (%.2f KB)\n",
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM), 
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024.0);
    Serial.printf("[WIFI_HOTSPOT]   Internal Free: %u bytes (%.2f KB)\n",
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0);
    Serial.printf("[WIFI_HOTSPOT]   DMA Free: %u bytes (%.2f KB)\n",
                  heap_caps_get_free_size(MALLOC_CAP_DMA),
                  heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024.0);
#endif

    // 确保NVS已初始化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] NVS分区满或版本不匹配，正在擦除并重新初始化...\n");
#endif
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    
    if (ret != ESP_OK) {
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] 错误: NVS初始化失败 (%s)\n", esp_err_to_name(ret));
#endif
        // 继续尝试，可能不需要NVS
    }

    // 避免用 softAPdisconnect(true) 触发 WiFi 全栈重置，改为仅关闭 AP 再切 AP 模式。
    WiFi.softAPdisconnect(false);
    delay(220);
    WiFi.mode(WIFI_AP);
    delay(420);
    
    // 配置IP地址
    IPAddress local_IP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    
    if (!WiFi.softAPConfig(local_IP, gateway, subnet)) {
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] 警告: IP配置失败，使用默认配置\n");
#endif
        // 继续尝试，可能会使用默认IP配置
    }
    
    // 尝试启动热点，增加重试机制
    int retries = 2;
    bool success = false;
    
    for (int i = 0; i < retries; i++) {
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] 尝试启动热点 (第 %d 次)...\n", i + 1);
#endif
        
        if (WiFi.softAP(currentSSID.c_str(), currentPassword.c_str(), DEFAULT_CHANNEL, 0, MAX_CONNECTIONS)) {
            success = true;
            break;
        }
        
        delay(800); // 等待后重试
        WiFi.softAPdisconnect(false);
        delay(220);
        WiFi.mode(WIFI_AP);
        delay(420);
    }
    
    if (!success) {
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] 错误: WiFi热点启动失败，已尝试 %d 次\n", retries);
#endif
        return false;
    }

    // 等待热点完全启动
    delay(1000);

#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] WiFi热点启动成功\n");
    Serial.printf("[WIFI_HOTSPOT] IP地址: %s\n", getIPAddress().c_str());
#endif

    // 创建并配置Web服务器
    if (!webServer) {
        webServer = new WebServer(80);
        // 注意：ESP32 WebServer不支持keepAlive方法
        // 设置较小的缓冲区以减少内存使用
    }

    // 配置路由改为通过 API 路由器统一注册（头文件在顶部包含）
    ApiRouter::registerRoutes(*webServer, *this);
    webServer->on("/favicon.ico", [this]() { webServer->send(204); }); // 避免404报错
    webServer->onNotFound([this]() { handleNotFound(); });

    // 启动Web服务器
    webServer->begin();

#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] Web服务器启动成功，端口: 80\n");
    Serial.printf("[WIFI_HOTSPOT] 访问地址: http://%s\n", getIPAddress().c_str());
#endif

    running = true;
    return true;
}

void WiFiHotspotManager::stop() {
    if (!running) {
        return;
    }

    stopInProgress = true;

    // 先切换状态，避免其他任务继续进入 handleClient。
    running = false;

#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] 正在停止WiFi热点和Web服务器...\n");
#endif

    // 停止Web服务器
    if (webServer) {
        webServer->stop();
    }

    // 仅关闭 AP，再切到 OFF，减少 AP_STOP/AP_START 抖动。
    WiFi.softAPdisconnect(false);
    delay(320);
    WiFi.mode(WIFI_OFF);
    delay(220);
    lastStopMs = millis();
    stopInProgress = false;

#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] WiFi热点已停止\n");
#endif
}

bool WiFiHotspotManager::isRunning() const {
    return running;
}

void WiFiHotspotManager::handleClient() {
    if (!webServer || !running) {
        return;
    }
    
    // 检查内存状态
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 32768) { // 提高到32KB，LWIP需要更多内存
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] 内存不足，跳过客户端处理: %u bytes\n", freeHeap);
#endif
        return;
    }
    
    // 如果正在上传文件，跳过处理以避免LWIP pbuf冲突
    if (isUploadInProgress()) {
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] 上传进行中，跳过客户端处理以避免网络冲突\n");
#endif
        return;
    }
    
    // 在处理客户端请求前让出CPU，防止与定时器冲突
    yield();
    
    // 处理客户端请求 - 移除中断禁用，因为这会导致看门狗超时
    // WebServer库和LWIP已经有适当的保护机制
    webServer->handleClient();
    
    // 处理完成后再次让出CPU
    yield();
}

const char* WiFiHotspotManager::getSSID() const {
    return currentSSID.c_str();
}

const char* WiFiHotspotManager::getPassword() const {
    return currentPassword.c_str();
}

String WiFiHotspotManager::getIPAddress() const {
    // 优先返回热点模式的IP
    if (running) {
        return WiFi.softAPIP().toString();
    }
    // 如果是STA模式连接成功，返回STA模式的IP
    extern bool g_wifi_sta_connected;
    if (g_wifi_sta_connected && WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "0.0.0.0";
}

int WiFiHotspotManager::getConnectedClients() const {
    if (running) {
        return WiFi.softAPgetStationNum();
    }
    return 0;
}

bool WiFiHotspotManager::isUploadInProgress() const {
    return uploadInProgress;
}

void WiFiHotspotManager::handleRoot() {
#if DBG_WIFI_HOTSPOT
    Serial.println("[WIFI_HOTSPOT] handleRoot() 开始");
#endif
    String html = generateWebPage();
#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] 生成HTML完成，大小: %d bytes\n", html.length());
#endif
    webServer->send(200, "text/html; charset=utf-8", html);
#if DBG_WIFI_HOTSPOT
    Serial.println("[WIFI_HOTSPOT] handleRoot() 完成");
#endif
}

void WiFiHotspotManager::handleFileList(String category) {
    // 检查内存状态
    if (ESP.getFreeHeap() < 10240) {
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] 警告：内存不足 (%d bytes)，可能影响分页功能\n", ESP.getFreeHeap());
#endif
    }
    
    String path;
    if (category == "book") {
        path = "/book";
        // Optional subdir parameter for directory navigation (e.g. ?subdir=manga)
        if (webServer->hasArg("subdir")) {
            String subdirParam = webServer->arg("subdir");
            std::string sd_val = std::string(subdirParam.c_str());
            // Security: reject path traversal and overlong values
            if (sd_val.find("..") == std::string::npos && sd_val.length() <= 128) {
                // Strip leading slashes
                while (!sd_val.empty() && sd_val[0] == '/') sd_val = sd_val.substr(1);
                if (!sd_val.empty()) {
                    path = String("/book/") + String(sd_val.c_str());
                }
            }
        }
    } else if (category == "font") {
        path = "/font";
    } else if (category == "image") {
        path = "/image";
    } else if (category == "screenshot") {
        path = "/screenshot";
    } else {
        path = "/";
    }

    // 解析分页参数（可选，默认返回全部以保持向后兼容）
    int page = webServer->hasArg("page") ? webServer->arg("page").toInt() : 0;
    int perPage = webServer->hasArg("perPage") ? webServer->arg("perPage").toInt() : 0;
    bool usePagination = (page > 0 && perPage > 0);

#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] /list/%s 请求, 目录: %s, 分页: %s (page=%d, perPage=%d), 剩余内存: %d\n", 
                 category.c_str(), path.c_str(), usePagination ? "是" : "否", page, perPage, ESP.getFreeHeap());
#endif

    // 采用路由层统一添加 CORS 头，避免重复
    webServer->setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer->send(200, "application/json", "");
    
    if (SDW::SD.exists(path.c_str())) {
        unsigned long startTime = millis();
        (void)startTime; // avoid unused-variable when DBG_WIFI_HOTSPOT==0
        std::string stdPath = path.c_str();
        yield();

        // 如果使用分页，先获取总数（轻量级操作）
        int totalFiles = 0;
        if (usePagination) {
            // For book paths, count dirs + .txt files; for others count all
            // We compute this after the full scan below, so leave at 0 for now
            (void)totalFiles;
        }

        // 根据是否分页选择扫描方式
        std::vector<FileInfo> files;
        if (usePagination) {
            // Scan all entries (dirs + all files) then filter manually
            std::vector<FileInfo> allFiles = EfficientFileScanner::scanDirectory(stdPath);
            // For book paths, keep only dirs + .txt files
            if (stdPath.rfind("/book", 0) == 0) {
                std::vector<FileInfo> filtered;
                for (auto& fi : allFiles) {
                    if (fi.isDirectory) {
                        filtered.push_back(fi);
                    } else if (fi.name.length() >= 4) {
                        std::string ext = fi.name.substr(fi.name.length() - 4);
                        for (char& c : ext) if (c >= 'A' && c <= 'Z') c += 32;
                        if (ext == ".txt") filtered.push_back(fi);
                    }
                }
                allFiles = std::move(filtered);
            }
            // Sort: dirs first, then alphabetical within each group
            std::sort(allFiles.begin(), allFiles.end(), [](const FileInfo &a, const FileInfo &b) {
                if (a.isDirectory != b.isDirectory) return (int)a.isDirectory > (int)b.isDirectory;
                std::string A = a.name; std::string B = b.name;
                for (char &c : A) if ((unsigned char)c >= 'A' && (unsigned char)c <= 'Z') c = c - 'A' + 'a';
                for (char &c : B) if ((unsigned char)c >= 'A' && (unsigned char)c <= 'Z') c = c - 'A' + 'a';
                return A < B;
            });
            totalFiles = (int)allFiles.size();
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] 文件总数: %d, 耗时: %lu ms\n", totalFiles, millis() - startTime);
#endif
            int startIndex = (page - 1) * perPage;
            for (int i = startIndex; i < startIndex + perPage && i < (int)allFiles.size(); ++i) {
                files.push_back(allFiles[i]);
            }
        } else {
            // Non-paginated: scan all entries and sort (dirs first, then alpha)
            files = EfficientFileScanner::scanDirectory(stdPath);
            std::sort(files.begin(), files.end(), [](const FileInfo &a, const FileInfo &b) {
                if (a.isDirectory != b.isDirectory) return (int)a.isDirectory > (int)b.isDirectory;
                std::string A = a.name; std::string B = b.name;
                for (char &c : A) if ((unsigned char)c >= 'A' && (unsigned char)c <= 'Z') c = c - 'A' + 'a';
                for (char &c : B) if ((unsigned char)c >= 'A' && (unsigned char)c <= 'Z') c = c - 'A' + 'a';
                return A < B;
            });
        }
        yield();
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] 扫描完成，返回 %d 个文件，耗时: %lu ms，剩余内存: %d bytes\n", 
                     (int)files.size(), millis() - startTime, ESP.getFreeHeap());
#endif

        // Optimization: for book category, only scan .idx files (not all files)
        // This avoids scanning txt/epub files twice and uses efficient lookup
        std::set<std::string> idxStems; // stems of files that have corresponding .idx
        if (stdPath.rfind("/book", 0) == 0) {
            // Efficient: only iterate directory once looking for .idx files
            File dir = SDW::SD.open(stdPath.c_str());
            if (dir && dir.isDirectory()) {
                dir.rewindDirectory();
                while (true) {
                    if (ESP.getFreeHeap() < 4096) break;
                    File entry = dir.openNextFile();
                    if (!entry) break;
                    
                    const char* namePtr = entry.name();
                    if (namePtr && strlen(namePtr) > 4) {
                        std::string fname = std::string(namePtr);
                        // Only process .idx files
                        if (fname.length() >= 4 && fname.substr(fname.length() - 4) == ".idx") {
                            std::string stem = fname.substr(0, fname.length() - 4);
                            idxStems.insert(stem);
                        }
                    }
                    entry.close();
                    
                    // Yield every 5 files
                    if (idxStems.size() % 5 == 0) yield();
                }
                dir.close();
            }
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] 找到 %d 个 .idx 文件，耗时: %lu ms\n", 
                         (int)idxStems.size(), millis() - startTime);
#endif
        }

        // 如果使用分页，返回包含元数据的 JSON 对象
        if (usePagination) {
            webServer->sendContent("{\"total\":");
            webServer->sendContent(String(totalFiles));
            webServer->sendContent(",\"page\":");
            webServer->sendContent(String(page));
            webServer->sendContent(",\"perPage\":");
            webServer->sendContent(String(perPage));
            webServer->sendContent(",\"files\":[");
        } else {
            // 向后兼容：返回纯数组
            webServer->sendContent("[");
        }
        
        bool first = true;

        // 使用运行时配置限制，且受编译期上限保护
        size_t runtimeLimit = (size_t)g_config.main_menu_file_count;
        size_t cap = (size_t)MAX_MAIN_MENU_FILE_COUNT;
        int effectiveLimit = (int)(runtimeLimit < cap ? runtimeLimit : cap);

        int count = 0;
        for (const auto& fileInfo : files) {
            if (count >= effectiveLimit) break;
            if (ESP.getFreeHeap() < 4096) break;
            
            // For book category, only return .txt files (skip .idx and other formats)
            if (stdPath.rfind("/book", 0) == 0 && !fileInfo.isDirectory) {
                std::string fname = fileInfo.name;
                // Check if file has .txt extension (case-insensitive)
                bool isTxt = false;
                if (fname.length() >= 4) {
                    std::string ext = fname.substr(fname.length() - 4);
                    // Simple lowercase conversion
                    for (char& c : ext) {
                        if (c >= 'A' && c <= 'Z') c += 32;
                    }
                    isTxt = (ext == ".txt");
                }
                // Skip non-txt files (including .idx)
                if (!isTxt) continue;
            }
            
            std::string safeFileName = fileInfo.name;
            // fullPath: the real full path exposed to client for actions (not truncated)
            std::string fullPath = stdPath + "/" + fileInfo.name;
            size_t pos = 0;
            while ((pos = safeFileName.find("\"", pos)) != std::string::npos) {
                safeFileName.replace(pos, 1, "\\\"");
                pos += 2;
            }
            // escape fullPath as well for JSON inclusion
            pos = 0;
            std::string escapedFullPath = fullPath;
            while ((pos = escapedFullPath.find("\"", pos)) != std::string::npos) {
                escapedFullPath.replace(pos, 1, "\\\"");
                pos += 2;
            }
            pos = 0;
            while ((pos = safeFileName.find("\\", pos)) != std::string::npos) {
                safeFileName.replace(pos, 1, "\\\\");
                pos += 2;
            }
            if (safeFileName.length() > 60) {
                safeFileName = safeFileName.substr(0, 57) + "...";
            }
            char jsonItem[512];
            // Determine if this file equals the currently opened book or currently used font (isCurrent)
            int isCurrent = 0;
            if (stdPath.rfind("/book", 0) == 0 && g_current_book) {
                std::string cur_fp = g_current_book->filePath();
                std::string cur_real;
                bool cur_use_spiffs = false;
                resolve_fake_path(cur_fp, cur_real, cur_use_spiffs);
                cur_real = normalize_real_path(cur_real);

                // construct fake path for this file (same format used by client: /book/NAME)
                std::string item_fake = stdPath + "/" + fileInfo.name;
                std::string item_real;
                bool item_use_spiffs = false;
                resolve_fake_path(item_fake, item_real, item_use_spiffs);
                item_real = normalize_real_path(item_real);

                if (cur_real == item_real && cur_use_spiffs == item_use_spiffs) {
                    isCurrent = 1;
                }
            }
            else if (path == "/font") {
                std::string cfg_fp = std::string(g_config.fontset);
                if (!cfg_fp.empty() && cfg_fp[0] == '/') {
                    std::string cur_real;
                    bool cur_use_spiffs = false;
                    resolve_fake_path(cfg_fp, cur_real, cur_use_spiffs);
                    cur_real = normalize_real_path(cur_real);

                    std::string item_fake = stdPath + "/" + fileInfo.name;
                    std::string item_real;
                    bool item_use_spiffs = false;
                    resolve_fake_path(item_fake, item_real, item_use_spiffs);
                    item_real = normalize_real_path(item_real);

                    if (cur_real == item_real && cur_use_spiffs == item_use_spiffs) {
                        isCurrent = 1;
                    }
                }
            }

            // Determine whether a same-name (without extension) .idx file exists
            // Use pre-built lookup set for O(1) check instead of filesystem exists()
            int isIdxed = 0;
            if (stdPath.rfind("/book", 0) == 0 && !fileInfo.isDirectory) {
                // derive stem (name without extension)
                std::string fname = fileInfo.name;
                size_t dot = fname.find_last_of('.');
                std::string stem = (dot == std::string::npos) ? fname : fname.substr(0, dot);
                // Fast O(log n) lookup in pre-built set (no filesystem calls)
                if (idxStems.find(stem) != idxStems.end()) {
                    isIdxed = 1;
                }
            }

            int written = snprintf(jsonItem, sizeof(jsonItem), 
                        "%s{\"name\":\"%s\",\"type\":\"%s\",\"size\":%d,\"isCurrent\":%d,\"isIdxed\":%d,\"path\":\"%s\"}",
                        first ? "" : ",",
                        safeFileName.c_str(), 
                        fileInfo.isDirectory ? "dir" : "file",
                        (int)fileInfo.size,
                        isCurrent,
                        isIdxed,
                        escapedFullPath.c_str());
            if (written > 0 && written < (int)sizeof(jsonItem)) {
                webServer->sendContent(jsonItem);
                first = false;
                yield();
            }
            count++;
        }
    }
    
    // 根据分页模式返回不同的结束标记
    if (usePagination) {
        webServer->sendContent("]}"); // 结束 files 数组和根对象
    } else {
        webServer->sendContent("]"); // 结束数组（向后兼容）
    }
    
    webServer->sendContent("");
    webServer->client().flush();
#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] /list 响应完成，剩余内存: %d bytes\n", ESP.getFreeHeap());
#endif
}

void WiFiHotspotManager::handleFileUpload() {
    String html = generateUploadForm();
    webServer->send(200, "text/html; charset=utf-8", html);
}

// ── Directory-operation helpers ─────────────────────────────────────────────

// Recursively collect all .txt file paths (full SD paths like /book/sub/name.txt) under dirPath
static void collectTxtFiles(const std::string& dirPath, std::vector<std::string>& paths) {
    File dir = SDW::SD.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) return;
    dir.rewindDirectory();
    while (true) {
        if (ESP.getFreeHeap() < 4096) break;
        File entry = dir.openNextFile();
        if (!entry) break;
        const char* nam = entry.name();
        if (!nam || strlen(nam) == 0) { entry.close(); continue; }
        std::string name = nam;
        bool isDir = entry.isDirectory();
        entry.close();
        std::string childPath = dirPath + "/" + name;
        if (isDir) {
            collectTxtFiles(childPath, paths);
        } else if (name.length() >= 4) {
            std::string ext = name.substr(name.length() - 4);
            for (char& c : ext) if (c >= 'A' && c <= 'Z') c += 32;
            if (ext == ".txt") paths.push_back(childPath);
        }
        yield();
    }
    dir.close();
}

// Recursively remove a directory and all its contents from SD
static void sdRemoveDirectoryRecursive(const std::string& path) {
    File dir = SDW::SD.open(path.c_str());
    if (!dir) return;
    if (!dir.isDirectory()) {
        dir.close();
        SDW::SD.remove(path.c_str());
        return;
    }
    dir.rewindDirectory();
    // Collect children first to avoid iterator invalidation
    std::vector<std::string> children;
    std::vector<bool> childIsDir;
    while (true) {
        if (ESP.getFreeHeap() < 4096) break;
        File entry = dir.openNextFile();
        if (!entry) break;
        const char* nam = entry.name();
        if (!nam || strlen(nam) == 0) { entry.close(); continue; }
        std::string name = nam;
        bool isD = entry.isDirectory();
        entry.close();
        children.push_back(path + "/" + name);
        childIsDir.push_back(isD);
    }
    dir.close();
    for (size_t i = 0; i < children.size(); ++i) {
        if (childIsDir[i]) sdRemoveDirectoryRecursive(children[i]);
        else SDW::SD.remove(children[i].c_str());
        yield();
    }
    SDW::SD.rmdir(path.c_str());
}

// ────────────────────────────────────────────────────────────────────────────

void WiFiHotspotManager::handleFileDelete() {
    String path = webServer->arg("path");
    if (path.isEmpty()) {
        webServer->send(400, "application/json", String("{\"ok\":false,\"message\":\"Missing path parameter\"}"));
        return;
    }

    // Prevent deleting the currently opened book: resolve and compare real paths first
    if (path.startsWith("/book/") && g_current_book) {
        std::string cur_fp = g_current_book->filePath();
        std::string cur_real;
        bool cur_use_spiffs = false;
        resolve_fake_path(cur_fp, cur_real, cur_use_spiffs);
        cur_real = normalize_real_path(cur_real);

        std::string del_fp = std::string(path.c_str());
        std::string del_real;
        bool del_use_spiffs = false;
        resolve_fake_path(del_fp, del_real, del_use_spiffs);
        del_real = normalize_real_path(del_real);

#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] delete check: current='%s' (spiffs=%d) target='%s' (spiffs=%d)\n",
                      cur_real.c_str(), cur_use_spiffs, del_real.c_str(), del_use_spiffs);
#endif

        if (cur_real == del_real && cur_use_spiffs == del_use_spiffs) {
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] Deny deletion of currently opened book: %s\n", path.c_str());
#endif
            webServer->send(400, "application/json", String("{\"ok\":false,\"message\":\"Cannot delete currently opened book\"}"));
            return;
        }
    }

    // ── Directory delete: recursive sidecar cleanup + rmdir ──
    {
        File testF = SDW::SD.open(path.c_str());
        bool isDir = testF && testF.isDirectory();
        if (testF) testF.close();

        if (isDir) {
            // Block if current book lives inside this directory
            if (g_current_book) {
                std::string cur_fp = g_current_book->filePath();
                std::string cur_real;
                bool cur_spiffs = false;
                resolve_fake_path(cur_fp, cur_real, cur_spiffs);
                std::string cur_canonical = (cur_spiffs ? "/spiffs" : "/sd") + cur_real;
                std::string dir_canonical = std::string("/sd") + path.c_str();
                if (!cur_spiffs && cur_canonical.rfind(dir_canonical + "/", 0) == 0) {
                    webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Cannot delete directory containing the currently opened book\"}");
                    return;
                }
            }

            // Collect all .txt files and clean up their sidecars + history entries
            std::vector<std::string> txtFiles;
            collectTxtFiles(std::string(path.c_str()), txtFiles);

            extern void removeIndexFilesForBookForPath(const std::string &book_file_path);
            extern std::string getBookmarkFileName(const std::string &book_file_path);
            extern bool removeBookFromHistory(const std::string &book_path);

            const char* bmExts[] = {".bm", ".page", ".progress", ".complete", ".rec", ".tags"};
            for (const auto& txtPath : txtFiles) {
                std::string canonical = "/sd" + txtPath;
                removeIndexFilesForBookForPath(canonical);
                // Remove all bookmark sidecar variants
                {
                    std::string bmBase = getBookmarkFileName(canonical);
                    size_t d = bmBase.rfind('.');
                    std::string bmStem = (d != std::string::npos) ? bmBase.substr(0, d) : bmBase;
                    for (const auto& ext : bmExts) {
                        std::string fn = bmStem + ext;
                        if (SDW::SD.exists(fn.c_str())) SDW::SD.remove(fn.c_str());
                        std::string tmp = fn + ".tmp";
                        if (SDW::SD.exists(tmp.c_str())) SDW::SD.remove(tmp.c_str());
                    }
                }
                // Remove .idx sidecar (alongside the .txt in the same directory)
                std::string idxPath = txtPath;
                size_t dot = idxPath.find_last_of('.');
                if (dot != std::string::npos) idxPath = idxPath.substr(0, dot) + ".idx";
                if (SDW::SD.exists(idxPath.c_str())) SDW::SD.remove(idxPath.c_str());
                removeBookFromHistory(canonical);
                yield();
            }

            // Recursively delete the directory tree
            sdRemoveDirectoryRecursive(std::string(path.c_str()));
            BookFileManager::refreshCache();
            webServer->send(200, "application/json", "{\"ok\":true,\"message\":\"Directory deleted successfully\"}");
            return;
        }
    }

    if (SDW::SD.remove(path.c_str())) {
        webServer->send(200, "application/json", String("{\"ok\":true,\"message\":\"File deleted successfully\"}"));
        // 如果删除的是字体目录下的文件，刷新全局字体列表
        if (path.startsWith("/font/")) {
            font_list_scan();
        }
        // 如果删除的是书籍目录下的文件，刷新书籍缓存，并处理当前阅读的书被删的情况
        if (path.startsWith("/book/")) {
            // 刷新缓存以反映最新的 /book 列表
            BookFileManager::refreshCache();

            // 额外清理：使用解析出的真实路径（带 /sd 或 /spiffs 前缀）来删除辅助文件
            std::string orig_fp = std::string(path.c_str());
            std::string real_fp;
            bool use_spiffs = false;
            if (!resolve_fake_path(orig_fp, real_fp, use_spiffs)) {
                // fallback to original if resolution fails
                real_fp = orig_fp;
                use_spiffs = false;
            }

            // Ensure book paths map to SD even when client omits /sd prefix (e.g. "/book/...")
            if (orig_fp.rfind("/book/", 0) == 0 || real_fp.rfind("/book/", 0) == 0 || orig_fp.rfind("/sd/", 0) == 0) {
                use_spiffs = false;
            }

            // 构造带前缀的 canonical path：/sd + real_fp 或 /spiffs + real_fp
            std::string canonical_fp = (use_spiffs ? std::string("/spiffs") : std::string("/sd")) + real_fp;

            // 1) 删除索引/进度相关文件（.page, .progress, .complete 等，位于 /bookmarks/）
            removeIndexFilesForBookForPath(canonical_fp);

            // 2) 删除书签文件 (.bm)（位于 SD 上的 /bookmarks）
            std::string bm_fn = getBookmarkFileName(canonical_fp);
            if (SDW::SD.exists(bm_fn.c_str())) {
                SDW::SD.remove(bm_fn.c_str());
            }

            // 3) 删除 tags 文件（位于 SD /bookmarks，下同）
            (void)clearTagsForFile(canonical_fp);

            // 4) 删除同目录的 .idx 侧车文件（位于 SD，例如 /book/xxx.idx）
            // real_fp 示例： "/book/xxx.txt"，将其扩展名替换为 .idx
            std::string idx_fp = real_fp;
            size_t dot = idx_fp.find_last_of('.');
            if (dot != std::string::npos) idx_fp = idx_fp.substr(0, dot) + ".idx";
            else idx_fp += ".idx";
            // idx_fp is path under SD (e.g. "/book/xxx.idx")
            if (SDW::SD.exists(idx_fp.c_str())) {
                SDW::SD.remove(idx_fp.c_str());
            }

            // 5) 从 history.list 中删除该书籍记录
            extern bool removeBookFromHistory(const std::string &book_path);
            removeBookFromHistory(canonical_fp);

            // 如果当前正在阅读的文件与删除的文件相同，尝试回退到默认文件
#if DBG_WIFI_HOTSPOT
                Serial.printf("[WIFI_HOTSPOT] comparing current book '%s' with deleted path '%s'\n", g_current_book ? g_current_book->filePath().c_str() : "(null)", path.c_str());
#endif
            if (g_current_book) {
                // 将两边路径规范化，去掉 /sd/ 或 /spiffs/ 前缀后比较
                std::string cur_fp = g_current_book->filePath();
                std::string cur_real;
                bool cur_use_spiffs = false;
                resolve_fake_path(cur_fp, cur_real, cur_use_spiffs);
                cur_real = normalize_real_path(cur_real);

                std::string del_fp = std::string(path.c_str());
                std::string del_real;
                bool del_use_spiffs = false;
                resolve_fake_path(del_fp, del_real, del_use_spiffs);
                del_real = normalize_real_path(del_real);

#if DBG_WIFI_HOTSPOT
                Serial.printf("[WIFI_HOTSPOT] normalized current='%s' (spiffs=%d) deleted='%s' (spiffs=%d)\n",
                              cur_real.c_str(), cur_use_spiffs, del_real.c_str(), del_use_spiffs);
#endif

                if (cur_real == del_real && cur_use_spiffs == del_use_spiffs) {
#if DBG_WIFI_HOTSPOT
                Serial.printf("[WIFI_HOTSPOT] 当前书籍已被删除: %s，尝试回退到默认文件\n", path.c_str());
#endif
                // 使用 config_update_current_book 来回退并持久化配置
                int16_t area_w = PAPER_S3_WIDTH - get_reading_effective_margin_left() - get_reading_effective_margin_right();
                int16_t area_h = PAPER_S3_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM;
                float fsize = SYSFONTSIZE;
                BookHandle* newb = config_update_current_book("/spiffs/ReadPaper.txt", area_w, area_h, fsize);
                    if (!newb) {
                    // 如果回退失败，清空当前引用
                    {
                        auto old_sp = std::atomic_load(&__g_current_book_shared);
                        if (old_sp) {
                            old_sp->markForClose();
                            // replace with null (will delete when no more references)
                            std::atomic_store(&__g_current_book_shared, std::shared_ptr<BookHandle>(nullptr));
                        }
                    }
                }
                }
            }
        }
    } else {
        webServer->send(500, "application/json", String("{\"ok\":false,\"message\":\"Failed to delete file\"}"));
    }
}

void WiFiHotspotManager::handleFileRename() {
    String old_path_str = webServer->arg("old_path");
    String new_name_str = webServer->arg("new_name");

    if (old_path_str.isEmpty() || new_name_str.isEmpty()) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Missing parameters: old_path and new_name required\"}");
        return;
    }

    // Only /book/ directory is supported
    if (!old_path_str.startsWith("/book/")) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Only /book/ files can be renamed\"}");
        return;
    }

    // Validate new_name: no path separators, must end with .txt, reasonable length
    std::string new_name = new_name_str.c_str();
    if (new_name.find('/') != std::string::npos || new_name.find('\\') != std::string::npos) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Invalid file name: path separators not allowed\"}");
        return;
    }
    if (new_name.empty() || new_name.length() > 64) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Invalid file name length\"}");
        return;
    }

    // Detect if source is a directory before enforcing .txt extension requirement
    bool sourceIsDir = false;
    {
        File testF = SDW::SD.open(old_path_str.c_str());
        if (testF) { sourceIsDir = testF.isDirectory(); testF.close(); }
    }

    if (!sourceIsDir) {
        std::string ext = new_name.length() >= 4 ? new_name.substr(new_name.length() - 4) : "";
        for (char &c : ext) c = (char)tolower((unsigned char)c);
        if (ext != ".txt") {
            webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Only .txt files are supported\"}");
            return;
        }
    }

    std::string old_path = old_path_str.c_str();

    // Construct new path (same directory as old)
    size_t lastSlash = old_path.rfind('/');
    std::string dir = (lastSlash != std::string::npos) ? old_path.substr(0, lastSlash) : "";
    std::string new_path = dir + "/" + new_name;

    if (!SDW::SD.exists(old_path.c_str())) {
        webServer->send(404, "application/json", "{\"ok\":false,\"message\":\"Source file not found\"}");
        return;
    }
    if (SDW::SD.exists(new_path.c_str())) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"A file with the new name already exists\"}");
        return;
    }

    if (!SDW::SD.rename(old_path.c_str(), new_path.c_str())) {
        webServer->send(500, "application/json", "{\"ok\":false,\"message\":\"Failed to rename file\"}");
        return;
    }

    // Main rename succeeded — respond immediately
    webServer->send(200, "application/json", "{\"ok\":true,\"message\":\"File renamed successfully\"}");

    // ── Post-rename: update all related auxiliary files ──

    // ── Directory rename: recursively update all .txt sidecars inside ──
    if (sourceIsDir) {
        extern std::string getBookmarkFileName(const std::string &book_file_path);
        extern bool renameBookInHistory(const std::string &old_path, const std::string &new_path);

        std::vector<std::string> txtFiles;
        collectTxtFiles(new_path, txtFiles);

        for (const auto& newTxtPath : txtFiles) {
            // Derive old path: replace new_path prefix with old_path
            std::string oldTxtPath = old_path + newTxtPath.substr(new_path.length());
            std::string old_txt_canonical = "/sd" + oldTxtPath;
            std::string new_txt_canonical = "/sd" + newTxtPath;

            // Robust copy-and-delete rename for bookmark sidecars
            auto robust_rename_sidecar = [&](const std::string &src, const std::string &dst) {
                if (!SDW::SD.exists(src.c_str())) return;
                if (SDW::SD.rename(src.c_str(), dst.c_str())) return;
                // Fallback: copy then delete
                File rs = SDW::SD.open(src.c_str(), "r");
                if (!rs) return;
                File rd = SDW::SD.open(dst.c_str(), "w");
                if (!rd) { rs.close(); return; }
                uint8_t buf[512];
                while (rs.available()) { int n = rs.read(buf, sizeof(buf)); if (n <= 0) break; rd.write(buf, (size_t)n); }
                rd.flush(); rd.close(); rs.close();
                SDW::SD.remove(src.c_str());
            };

            // Rename all 6 sidecar extensions + .tmp variants
            const char* bmExts[] = {".bm", ".page", ".progress", ".complete", ".rec", ".tags"};
            for (const auto& ext : bmExts) {
                std::string oldBm = getBookmarkFileName(old_txt_canonical);
                size_t d = oldBm.rfind('.');
                std::string oldFn = (d != std::string::npos) ? oldBm.substr(0, d) + ext : oldBm + ext;
                std::string newBm = getBookmarkFileName(new_txt_canonical);
                d = newBm.rfind('.');
                std::string newFn = (d != std::string::npos) ? newBm.substr(0, d) + ext : newBm + ext;
                robust_rename_sidecar(oldFn, newFn);
                robust_rename_sidecar(oldFn + ".tmp", newFn + ".tmp");
            }

            // Rename .idx sidecar
            {
                size_t d = oldTxtPath.find_last_of('.');
                std::string oldIdx = (d != std::string::npos) ? oldTxtPath.substr(0, d) + ".idx" : oldTxtPath + ".idx";
                d = newTxtPath.find_last_of('.');
                std::string newIdx = (d != std::string::npos) ? newTxtPath.substr(0, d) + ".idx" : newTxtPath + ".idx";
                if (SDW::SD.exists(oldIdx.c_str())) SDW::SD.rename(oldIdx.c_str(), newIdx.c_str());
            }

            // Patch file_path= in .bm and .progress
            auto patch_file_path_dir = [&](const std::string &fn) {
                if (!SDW::SD.exists(fn.c_str())) return;
                std::vector<std::string> lines;
                {
                    File f = SDW::SD.open(fn.c_str(), "r");
                    if (!f) return;
                    while (f.available()) {
                        String line = f.readStringUntil('\n'); line.trim();
                        std::string ln = line.c_str();
                        if (ln.rfind("file_path=", 0) == 0) ln = "file_path=" + new_txt_canonical;
                        lines.push_back(ln);
                    }
                    f.close();
                }
                if (!lines.empty()) {
                    SafeFS::safeWrite(fn, [&](File &f) -> bool {
                        for (const auto &ln : lines) f.println(ln.c_str());
                        return true;
                    });
                }
            };
            {
                std::string bmBase = getBookmarkFileName(new_txt_canonical);
                size_t d = bmBase.rfind('.');
                std::string bmStem = (d != std::string::npos) ? bmBase.substr(0, d) : bmBase;
                patch_file_path_dir(bmStem + ".bm");
                patch_file_path_dir(bmStem + ".progress");
            }

            // Update history.list
            renameBookInHistory(old_txt_canonical, new_txt_canonical);

            // If currently reading this book, update config
            {
                std::string cfg_raw = std::string(g_config.currentReadFile);
                std::string cfg_real;
                bool cfg_spiffs = false;
                resolve_fake_path(cfg_raw, cfg_real, cfg_spiffs);
                cfg_real = normalize_real_path(cfg_real);
                std::string old_norm = normalize_real_path(oldTxtPath);
                if (!cfg_spiffs && cfg_real == old_norm) {
                    strncpy(g_config.currentReadFile, new_txt_canonical.c_str(), sizeof(g_config.currentReadFile) - 1);
                    g_config.currentReadFile[sizeof(g_config.currentReadFile) - 1] = '\0';
                    config_save();
                    if (g_current_book) {
                        int16_t aw = g_current_book->getAreaWidth();
                        int16_t ah = g_current_book->getAreaHeight();
                        float fs = g_current_book->getFontSize();
                        config_update_current_book(new_txt_canonical.c_str(), aw, ah, fs);
                    }
                }
            }

            yield();
        }

        BookFileManager::refreshCache();
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] 目录重命名完成: '%s' → '%s'\n", old_path.c_str(), new_path.c_str());
#endif
        return;
    }

    // ── Single-file post-rename: update all related auxiliary files ──

    // Resolve real paths (strips /sd or /spiffs prefix if present)
    std::string old_real_fp;
    bool old_spiffs = false;
    if (!resolve_fake_path(old_path, old_real_fp, old_spiffs)) old_real_fp = old_path;
    old_spiffs = false; // books are always on SD

    std::string new_real_fp;
    bool new_spiffs = false;
    if (!resolve_fake_path(new_path, new_real_fp, new_spiffs)) new_real_fp = new_path;
    new_spiffs = false;

    // Canonical paths (with /sd prefix)
    std::string old_canonical = "/sd" + old_real_fp;
    std::string new_canonical = "/sd" + new_real_fp;

    // 1) Rename .idx sidecar in the same directory
    {
        auto make_idx_path = [](const std::string &fp) -> std::string {
            size_t d = fp.rfind('.');
            return (d != std::string::npos) ? fp.substr(0, d) + ".idx" : fp + ".idx";
        };
        std::string old_idx = make_idx_path(old_real_fp);
        std::string new_idx = make_idx_path(new_real_fp);
        if (SDW::SD.exists(old_idx.c_str())) {
            SDW::SD.rename(old_idx.c_str(), new_idx.c_str());
        }
    }

    // 2) Rename bookmark-related files in /bookmarks/
    {
        extern std::string getBookmarkFileName(const std::string &book_file_path);

        // Robust rename: try SDW::SD.rename first; if it fails, fall back to copy+delete
        auto robust_rename = [&](const std::string &src, const std::string &dst) {
            if (!SDW::SD.exists(src.c_str())) return;
            if (SDW::SD.rename(src.c_str(), dst.c_str())) return; // fast path

            // Fallback: copy contents then delete source
            File rs = SDW::SD.open(src.c_str(), "r");
            if (!rs) return;
            File rd = SDW::SD.open(dst.c_str(), "w");
            if (!rd) { rs.close(); return; }
            uint8_t buf[512];
            while (rs.available()) {
                int n = rs.read(buf, sizeof(buf));
                if (n <= 0) break;
                rd.write(buf, (size_t)n);
            }
            rd.flush();
            rd.close();
            rs.close();
            SDW::SD.remove(src.c_str());
        };

        // getBookmarkFileName returns /bookmarks/<safe>.bm; we swap the extension
        auto rename_bm = [&](const char *ext) {
            std::string old_bm = getBookmarkFileName(old_canonical);
            size_t d = old_bm.rfind('.');
            std::string old_fn = (d != std::string::npos) ? old_bm.substr(0, d) + ext : old_bm + ext;

            std::string new_bm = getBookmarkFileName(new_canonical);
            d = new_bm.rfind('.');
            std::string new_fn = (d != std::string::npos) ? new_bm.substr(0, d) + ext : new_bm + ext;

            robust_rename(old_fn, new_fn);

            // Also handle SafeFS .tmp sidecar (best-effort)
            std::string old_tmp = old_fn + ".tmp";
            std::string new_tmp = new_fn + ".tmp";
            robust_rename(old_tmp, new_tmp);
        };
        rename_bm(".bm");
        rename_bm(".page");
        rename_bm(".progress");
        rename_bm(".complete");
        rename_bm(".rec");
        rename_bm(".tags");
    }

    // 3) Update the file_path= line inside bookmark files that embed it (.bm, .progress)
    {
        extern std::string getBookmarkFileName(const std::string &book_file_path);
        // Helper: patch file_path= line in-place for a given bookmark file
        auto patch_file_path = [&](const std::string &fn) {
            if (!SDW::SD.exists(fn.c_str())) return;
            std::vector<std::string> lines;
            {
                File f = SDW::SD.open(fn.c_str(), "r");
                if (!f) return;
                while (f.available()) {
                    String line = f.readStringUntil('\n');
                    line.trim();
                    std::string ln = line.c_str();
                    if (ln.rfind("file_path=", 0) == 0) {
                        ln = std::string("file_path=") + new_canonical;
                    }
                    lines.push_back(ln);
                }
                f.close();
            }
            if (!lines.empty()) {
                SafeFS::safeWrite(fn, [&](File &f) -> bool {
                    for (const auto &ln : lines) f.println(ln.c_str());
                    return true;
                });
            }
        };

        // Helper to get the progress filename from the bookmark filename base
        auto make_fn = [&](const char *ext) -> std::string {
            std::string bm = getBookmarkFileName(new_canonical);
            size_t d = bm.rfind('.');
            return (d != std::string::npos) ? bm.substr(0, d) + ext : bm + ext;
        };

        patch_file_path(make_fn(".bm"));
        patch_file_path(make_fn(".progress"));
    }

    // 4) Update history.list: replace old canonical path with new
    {
        extern bool renameBookInHistory(const std::string &old_path, const std::string &new_path);
        renameBookInHistory(old_canonical, new_canonical);
    }

    // 5) If the currently open book is being renamed, update config and re-open it
    {
        std::string cfg_raw = std::string(g_config.currentReadFile);
        std::string cfg_real;
        bool cfg_spiffs = false;
        resolve_fake_path(cfg_raw, cfg_real, cfg_spiffs);
        cfg_real = normalize_real_path(cfg_real);
        std::string old_norm = normalize_real_path(old_real_fp);

        if (!cfg_spiffs && cfg_real == old_norm) {
            // Update persisted config path
            strncpy(g_config.currentReadFile, new_canonical.c_str(), sizeof(g_config.currentReadFile) - 1);
            g_config.currentReadFile[sizeof(g_config.currentReadFile) - 1] = '\0';
            config_save();

            // Re-open the renamed file as current book so runtime state is consistent
            if (g_current_book) {
                int16_t aw = g_current_book->getAreaWidth();
                int16_t ah = g_current_book->getAreaHeight();
                float fs = g_current_book->getFontSize();
                config_update_current_book(new_canonical.c_str(), aw, ah, fs);
            }
        }
    }

    // 6) Refresh book-list cache
    BookFileManager::refreshCache();

#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] 书籍重命名完成: '%s' → '%s'\n",
                  old_canonical.c_str(), new_canonical.c_str());
#endif
}

void WiFiHotspotManager::handleMkdir() {
    String path = webServer->arg("path");
    if (path.isEmpty()) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Missing path parameter\"}");
        return;
    }
    // Only allow creating directories under /book/
    if (!path.startsWith("/book/")) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Directories can only be created under /book/\"}");
        return;
    }
    // Security: reject path traversal
    std::string pathStr = path.c_str();
    if (pathStr.find("..") != std::string::npos) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Invalid path\"}");
        return;
    }
    if (SDW::SD.exists(path.c_str())) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Path already exists\"}");
        return;
    }
    if (SDW::SD.mkdir(path.c_str())) {
        BookFileManager::refreshCache();
        webServer->send(200, "application/json", "{\"ok\":true,\"message\":\"Directory created successfully\"}");
    } else {
        webServer->send(500, "application/json", "{\"ok\":false,\"message\":\"Failed to create directory\"}");
    }
}

void WiFiHotspotManager::handleFileDownload() {
    String path = webServer->arg("path");
    if (path.isEmpty()) {
        webServer->send(400, "application/json", String("{\"ok\":false,\"message\":\"Missing path parameter\"}"));
        return;
    }
    // Normalize path and determine filename for Content-Disposition
    String norm = String(normalize_real_path(path.c_str()).c_str());
    if (norm.length() == 0) {
        webServer->send(400, "application/json", String("{\"ok\":false,\"message\":\"Invalid path parameter\"}"));
        return;
    }

    // Extract basename
    String filename = norm;
    int lastSlash = filename.lastIndexOf('/');
    if (lastSlash >= 0) filename = filename.substring(lastSlash + 1);
    if (filename.length() == 0) filename = "download";

    // Prefer SPIFFS, then SD
    File file;
    if (SPIFFS.exists(norm)) {
        file = SPIFFS.open(norm.c_str(), "r");
    } else if (SDW::SD.exists(norm.c_str())) {
        file = SDW::SD.open(norm.c_str(), "r");
    }

    if (!file || file.isDirectory()) {
        if (file) file.close();
        webServer->send(404, "application/json", String("{\"ok\":false,\"message\":\"File not found\"}"));
        return;
    }

    // Set appropriate content type and Content-Disposition so browser saves with correct filename
    String contentType = getContentType(norm);
    String disposition = "attachment; filename=\"" + filename + "\"";
    webServer->sendHeader("Content-Type", contentType);
    webServer->sendHeader("Content-Disposition", disposition);
    webServer->streamFile(file, contentType);
    file.close();
}

// Helper function to parse a .rec file and return JSON object for a single book
static void parseRecFileToJson(const std::string &rec_file_path, const std::string &book_path, String &jsonOutput) {
    jsonOutput = "{";
    jsonOutput += "\"book_path\":\"" + String(book_path.c_str()) + "\",";
    
    // Extract book name from path
    size_t lastSlash = book_path.find_last_of("/");
    std::string bookName = (lastSlash != std::string::npos) ? book_path.substr(lastSlash + 1) : book_path;
    jsonOutput += "\"book_name\":\"" + String(bookName.c_str()) + "\",";
    
    if (!SDW::SD.exists(rec_file_path.c_str())) {
        jsonOutput += "\"error\":\"Record file not found\",";
        jsonOutput += "\"total_hours\":0,\"total_minutes\":0,";
        jsonOutput += "\"hourly_records\":{},";
        jsonOutput += "\"daily_summary\":{},";
        jsonOutput += "\"monthly_summary\":{}";
        jsonOutput += "}";
        return;
    }
    
    File rf = SDW::SD.open(rec_file_path.c_str(), "r");
    if (!rf) {
        jsonOutput += "\"error\":\"Failed to open record file\",";
        jsonOutput += "\"total_hours\":0,\"total_minutes\":0,";
        jsonOutput += "\"hourly_records\":{},";
        jsonOutput += "\"daily_summary\":{},";
        jsonOutput += "\"monthly_summary\":{}";
        jsonOutput += "}";
        return;
    }
    
    // Get total time from .bm file (bookmark file) instead of .rec file
    // This matches the device-side ui_time_rec logic
    int totalHours = 0;
    int totalMinutes = 0;
    
    std::string bm_file_path = getBookmarkFileName(book_path);
    
#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] Reading .bm file for total time: %s\n", bm_file_path.c_str());
#endif
    
    if (SDW::SD.exists(bm_file_path.c_str())) {
        File bmf = SDW::SD.open(bm_file_path.c_str(), "r");
        if (bmf) {
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] .bm file opened successfully\n");
#endif
            while (bmf.available()) {
                String line = bmf.readStringUntil('\n');
                line.trim();
                if (line.length() == 0) continue;
                
                int eq_pos = line.indexOf('=');
                if (eq_pos > 0) {
                    String key = line.substring(0, eq_pos);
                    String value = line.substring(eq_pos + 1);
                    key.trim();
                    value.trim();
                    
                    if (key == "readhour") {
                        totalHours = value.toInt();
#if DBG_WIFI_HOTSPOT
                        Serial.printf("[WIFI_HOTSPOT] Found readhour=%d\n", totalHours);
#endif
                    } else if (key == "readmin") {
                        totalMinutes = value.toInt();
#if DBG_WIFI_HOTSPOT
                        Serial.printf("[WIFI_HOTSPOT] Found readmin=%d\n", totalMinutes);
#endif
                    }
                }
            }
            bmf.close();
        }
#if DBG_WIFI_HOTSPOT
        else {
            Serial.printf("[WIFI_HOTSPOT] Failed to open .bm file\n");
        }
    } else {
        Serial.printf("[WIFI_HOTSPOT] .bm file does not exist: %s\n", bm_file_path.c_str());
#endif
    }
    
    // Skip the first line of .rec file (no longer used for total time)
    if (rf.available()) {
        rf.readStringUntil('\n');
    }
    
    jsonOutput += "\"total_hours\":" + String(totalHours) + ",";
    jsonOutput += "\"total_minutes\":" + String(totalMinutes) + ",";
    
    // Parse hourly records
    std::map<std::string, int32_t> hourlyRecords;
    std::map<std::string, int32_t> dailySummary;
    std::map<std::string, int32_t> monthlySummary;
    
    // Time period distribution (same logic as ui_time_rec.cpp)
    int32_t morning_mins = 0;   // 04:00-12:00
    int32_t afternoon_mins = 0; // 12:00-20:00
    int32_t night_mins = 0;     // 20:00-04:00
    int32_t unknown_mins = 0;   // Format errors
    
    while (rf.available()) {
        String line = rf.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        
        int colon = line.indexOf(':');
        if (colon > 0) {
            String ts = line.substring(0, colon);
            String val = line.substring(colon + 1);
            
            // Parse time value (XXhXXm or XXm)
            int32_t mins = 0;
            int h_pos = val.indexOf('h');
            if (h_pos > 0) {
                int hours = val.substring(0, h_pos).toInt();
                int m_pos = val.indexOf('m', h_pos);
                int minutes = 0;
                if (m_pos > h_pos + 1) {
                    minutes = val.substring(h_pos + 1, m_pos).toInt();
                }
                mins = hours * 60 + minutes;
            } else {
                int m_pos = val.indexOf('m');
                if (m_pos > 0) {
                    mins = val.substring(0, m_pos).toInt();
                }
            }
            
            std::string timestamp = ts.c_str();
            // 仅处理格式正确的时间戳（YYYYMMDDHH = 10位）
            if (timestamp.length() == 10 && mins >= 0) {
                hourlyRecords[timestamp] = mins;
                
                // Aggregate by day (YYYYMMDD)
                std::string day = timestamp.substr(0, 8);
                dailySummary[day] += mins;
                
                // Aggregate by month (YYYYMM)
                std::string month = timestamp.substr(0, 6);
                monthlySummary[month] += mins;
                
                // Calculate time period distribution
                // Extract hour (last 2 digits)
                int hour = atoi(timestamp.substr(8, 2).c_str());
                
                if (hour >= 4 && hour < 12) {
                    morning_mins += mins;
                } else if (hour >= 12 && hour < 20) {
                    afternoon_mins += mins;
                } else { // 20:00-04:00 (20-23, 0-3)
                    night_mins += mins;
                }
            } else {
                // Format error, count as unknown
                unknown_mins += mins;
            }
        }
    }
    rf.close();
    
    // Build hourly_records JSON
    jsonOutput += "\"hourly_records\":{";
    bool first = true;
    for (const auto& entry : hourlyRecords) {
        if (!first) jsonOutput += ",";
        jsonOutput += "\"" + String(entry.first.c_str()) + "\":" + String(entry.second);
        first = false;
    }
    jsonOutput += "},";
    
    // Build daily_summary JSON
    jsonOutput += "\"daily_summary\":{";
    first = true;
    for (const auto& entry : dailySummary) {
        if (!first) jsonOutput += ",";
        jsonOutput += "\"" + String(entry.first.c_str()) + "\":" + String(entry.second);
        first = false;
    }
    jsonOutput += "},";
    
    // Build monthly_summary JSON
    jsonOutput += "\"monthly_summary\":{";
    first = true;
    for (const auto& entry : monthlySummary) {
        if (!first) jsonOutput += ",";
        jsonOutput += "\"" + String(entry.first.c_str()) + "\":" + String(entry.second);
        first = false;
    }
    jsonOutput += "},";
    
    // Add time period distribution (04-12, 12-20, 20-04, unknown)
    // Based on actual hourly records sum, not bm file total
    int32_t actual_total_mins = morning_mins + afternoon_mins + night_mins + unknown_mins;
    jsonOutput += "\"time_distribution\":{";
    jsonOutput += "\"morning_04_12\":" + String(morning_mins) + ",";
    jsonOutput += "\"afternoon_12_20\":" + String(afternoon_mins) + ",";
    jsonOutput += "\"night_20_04\":" + String(night_mins) + ",";
    jsonOutput += "\"unknown\":" + String(unknown_mins) + ",";
    jsonOutput += "\"total_from_records\":" + String(actual_total_mins);
    jsonOutput += "}";
    
    jsonOutput += "}";
}

void WiFiHotspotManager::handleReadingRecords() {
    // Parse query parameters
    // Supports: 
    // - /api/reading_records?book=/book/example.txt (single book)
    // - /api/reading_records?books=/book/a.txt,/book/b.txt (multiple books)
    // - /api/reading_records (all books with .rec files)
    
    String bookParam = webServer->hasArg("book") ? webServer->arg("book") : "";
    String booksParam = webServer->hasArg("books") ? webServer->arg("books") : "";
    
#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] /api/reading_records request, book: %s, books: %s\n", 
                 bookParam.c_str(), booksParam.c_str());
#endif
    
    webServer->setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer->send(200, "application/json", "");
    
    std::vector<std::string> bookPaths;
    
    // Single book query
    if (bookParam.length() > 0) {
        bookPaths.push_back(std::string(bookParam.c_str()));
    }
    // Multiple books query
    else if (booksParam.length() > 0) {
        String remaining = booksParam;
        while (remaining.length() > 0) {
            int commaPos = remaining.indexOf(',');
            if (commaPos > 0) {
                String path = remaining.substring(0, commaPos);
                path.trim();
                if (path.length() > 0) {
                    bookPaths.push_back(std::string(path.c_str()));
                }
                remaining = remaining.substring(commaPos + 1);
            } else {
                remaining.trim();
                if (remaining.length() > 0) {
                    bookPaths.push_back(std::string(remaining.c_str()));
                }
                break;
            }
        }
    }
    // All books query - scan /bookmarks directory for .rec files
    else {
        std::string bookmarksDir = "/bookmarks";
        if (SDW::SD.exists(bookmarksDir.c_str())) {
            File dir = SDW::SD.open(bookmarksDir.c_str());
            if (dir && dir.isDirectory()) {
                dir.rewindDirectory();
                while (true) {
                    File entry = dir.openNextFile();
                    if (!entry) break;
                    
                    const char* namePtr = entry.name();
                    if (namePtr) {
                        std::string fname = std::string(namePtr);
                        // Look for .rec files (format: _book_bookname.rec or _sd_book_bookname.rec)
                        if (fname.length() > 4 && fname.substr(fname.length() - 4) == ".rec") {
                            // Convert .rec filename back to book path
                            // _book_bookname.rec -> /book/bookname
                            // _sd_book_bookname.rec -> /sd/book/bookname
                            std::string bookName = fname.substr(0, fname.length() - 4);
                            
                            // Replace underscores back to slashes to reconstruct path
                            std::string bookPath;
                            for (size_t i = 0; i < bookName.length(); i++) {
                                if (bookName[i] == '_') {
                                    bookPath += '/';
                                } else {
                                    bookPath += bookName[i];
                                }
                            }
                            
                            // Only include if it looks like a valid book path
                            if (bookPath.find("/book/") != std::string::npos || 
                                bookPath.find("/sd/book/") != std::string::npos ||
                                bookPath.find("/spiffs/") != std::string::npos) {
                                bookPaths.push_back(bookPath);
                            }
                        }
                    }
                    entry.close();
                    yield();
                }
                dir.close();
            }
        }
    }
    
    // Send response with progress info
    int totalBooks = bookPaths.size();
    webServer->sendContent("{\"total\":" + String(totalBooks) + ",");
    webServer->sendContent("\"records\":[");
    
    bool first = true;
    int processed = 0;
    
    for (const auto& bookPath : bookPaths) {
        if (ESP.getFreeHeap() < 4096) {
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] 内存不足，停止处理，已处理 %d/%d\n", processed, totalBooks);
#endif
            break;
        }
        
        // Try different path prefixes to find the .rec file
        std::string recPath;
        std::string actualBookPath = bookPath;
        
        // If path doesn't have /sd/ or /spiffs/ prefix, try both
        if (bookPath.find("/sd/") != 0 && bookPath.find("/spiffs/") != 0) {
            // Try /sd prefix first (most common for books)
            std::string sdPath = "/sd" + bookPath;
            std::string sdRecPath = getRecordFileName(sdPath);
            
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] Checking rec file: %s\n", sdRecPath.c_str());
#endif
            
            if (SDW::SD.exists(sdRecPath.c_str())) {
                recPath = sdRecPath;
                actualBookPath = sdPath;
            } else {
                // Try /spiffs prefix
                std::string spiffsPath = "/spiffs" + bookPath;
                std::string spiffsRecPath = getRecordFileName(spiffsPath);
                
#if DBG_WIFI_HOTSPOT
                Serial.printf("[WIFI_HOTSPOT] Checking rec file: %s\n", spiffsRecPath.c_str());
#endif
                
                if (SDW::SD.exists(spiffsRecPath.c_str())) {
                    recPath = spiffsRecPath;
                    actualBookPath = spiffsPath;
                } else {
                    // Try without prefix as last resort
                    recPath = getRecordFileName(bookPath);
                    
#if DBG_WIFI_HOTSPOT
                    Serial.printf("[WIFI_HOTSPOT] Checking rec file: %s\n", recPath.c_str());
#endif
                }
            }
        } else {
            recPath = getRecordFileName(bookPath);
            
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] Checking rec file: %s\n", recPath.c_str());
#endif
        }
        
        String recordJson;
        parseRecFileToJson(recPath, actualBookPath, recordJson);
        
        if (!first) {
            webServer->sendContent(",");
        }
        webServer->sendContent(recordJson);
        first = false;
        processed++;
        
        yield();
    }
    
    webServer->sendContent("],");
    webServer->sendContent("\"processed\":" + String(processed));
    webServer->sendContent("}");
    webServer->sendContent(""); // 结束响应
    
#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] /api/reading_records 完成，处理了 %d/%d 本书\n", processed, totalBooks);
#endif
}

void WiFiHotspotManager::handleDeviceGuide() {
    if (!webServer) {
        return;
    }

    String hw = "M5Stack PaperS3";
    String firmware = "ReadPaper";
    String version = "V1.3";

    if (SPIFFS.exists("/version")) {
        File vf = SPIFFS.open("/version", "r");
        if (vf) {
            int lineNo = 0;
            while (vf.available() && lineNo < 3) {
                String line = vf.readStringUntil('\n');
                while (line.endsWith("\r")) {
                    line.remove(line.length() - 1);
                }
                if (lineNo == 0 && line.length()) {
                    hw = line;
                } else if (lineNo == 1 && line.length()) {
                    firmware = line;
                } else if (lineNo == 2 && line.length()) {
                    version = line;
                }
                lineNo++;
            }
            vf.close();
        }
    }

    String currentBookPath = "";
    {
        std::shared_ptr<BookHandle> current_book = current_book_shared();
        if (current_book) {
            currentBookPath = String(current_book->filePath().c_str());
        } else if (strlen(g_config.currentReadFile) > 0) {
            currentBookPath = String(g_config.currentReadFile);
        }
    }

    bool hasWebdav = strlen(g_config.webdav_url) > 0;
    bool hasWifiConfig = false;
    for (int i = 0; i < 3; i++) {
        if (strlen(g_config.wifi_ssid[i]) > 0) {
            hasWifiConfig = true;
            break;
        }
    }

    JsonDocument doc;
    doc["ok"] = true;
    doc["schema_version"] = 1;
    doc["schema"] = "readpaper.device_guide.v1";

    JsonObject device = doc["device"].to<JsonObject>();
    device["hw"] = hw;
    device["firmware"] = firmware;
    device["version"] = version;
    device["ip"] = getIPAddress();
    device["wifi_sta_connected"] = g_wifi_sta_connected;
    device["wifi_ap_clients"] = getConnectedClients();
    device["upload_in_progress"] = isUploadInProgress();
    device["current_book"] = currentBookPath;

    JsonObject sections = doc["sections"].to<JsonObject>();
    sections["file_management"] = true;
    sections["time_management"] = true;
    sections["settings_management"] = true;

    // Normalized alias for cross-device clients.
    JsonObject modules = doc["modules"].to<JsonObject>();
    modules["file"] = true;
    modules["time"] = true;
    modules["settings"] = true;

    JsonObject fileManagement = doc["fileManagement"].to<JsonObject>();
    fileManagement["required"] = true;
    JsonArray tabs = fileManagement["tabs"].to<JsonArray>();

    {
        JsonObject tab = tabs.add<JsonObject>();
        tab["id"] = "book";
        tab["apiTab"] = "book";
        tab["title"] = "书籍";
        tab["hint"] = "支持 unicode/GBK 编码的 txt 文件。";
        tab["supportsHierarchy"] = true;
        tab["allowUpload"] = true;
        tab["allowDelete"] = true;
        tab["allowRename"] = true;
        tab["allowMkdir"] = true;
        tab["allowReadingRecords"] = true;
        tab["showIdxBadge"] = true;
    }
    {
        JsonObject tab = tabs.add<JsonObject>();
        tab["id"] = "font";
        tab["apiTab"] = "font";
        tab["title"] = "字体";
        tab["hint"] = "请上传工具生成的 font.bin。";
        tab["supportsHierarchy"] = false;
        tab["allowUpload"] = true;
        tab["allowDelete"] = true;
        tab["allowRename"] = false;
        tab["allowMkdir"] = false;
        tab["allowReadingRecords"] = false;
        tab["showIdxBadge"] = false;
    }
    {
        JsonObject tab = tabs.add<JsonObject>();
        tab["id"] = "image";
        tab["apiTab"] = "image";
        tab["title"] = "锁屏";
        tab["hint"] = "锁屏图片建议 540x960，支持透明 png。";
        tab["supportsHierarchy"] = false;
        tab["allowUpload"] = true;
        tab["allowDelete"] = true;
        tab["allowRename"] = false;
        tab["allowMkdir"] = false;
        tab["allowReadingRecords"] = false;
        tab["showIdxBadge"] = false;
    }
    {
        JsonObject tab = tabs.add<JsonObject>();
        tab["id"] = "screenshot";
        tab["apiTab"] = "screenshot";
        tab["title"] = "截图";
        tab["hint"] = "设备截图存储目录。";
        tab["supportsHierarchy"] = false;
        tab["allowUpload"] = false;
        tab["allowDelete"] = true;
        tab["allowRename"] = false;
        tab["allowMkdir"] = false;
        tab["allowReadingRecords"] = false;
        tab["showIdxBadge"] = false;
        tab["supportsScback"] = true;
    }

    JsonObject timeManagement = doc["timeManagement"].to<JsonObject>();
    timeManagement["enabled"] = true;
    timeManagement["allowSyncTime"] = true;

    JsonObject settingsManagement = doc["settingsManagement"].to<JsonObject>();
    settingsManagement["enabled"] = true;
    settingsManagement["allowWifiConfig"] = true;
    settingsManagement["allowWebdavConfig"] = true;
    settingsManagement["hasWifiConfig"] = hasWifiConfig;
    settingsManagement["hasWebdavConfig"] = hasWebdav;

    JsonObject endpoints = doc["endpoints"].to<JsonObject>();
    endpoints["heartbeat"] = "/heartbeat";
    endpoints["list"] = "/list";
    endpoints["upload"] = "/upload";
    endpoints["download"] = "/download";
    endpoints["delete"] = "/delete";
    endpoints["rename"] = "/rename";
    endpoints["mkdir"] = "/mkdir";
    endpoints["sync_time"] = "/sync_time";
    endpoints["reading_records"] = "/api/reading_records";
    endpoints["wifi_config"] = "/api/wifi_config";
    endpoints["webdav_config"] = "/api/webdav_config";
    endpoints["device_guide"] = "/api/device_guide";
    endpoints["guide"] = "/api/guide";

    // Normalized snake_case aliases while keeping old camelCase keys.
    JsonObject fileManagementCompat = doc["file_management"].to<JsonObject>();
    fileManagementCompat["required"] = true;
    fileManagementCompat["tabs"] = tabs;

    JsonObject timeManagementCompat = doc["time_management"].to<JsonObject>();
    timeManagementCompat["enabled"] = true;
    timeManagementCompat["allow_sync_time"] = true;

    JsonObject settingsManagementCompat = doc["settings_management"].to<JsonObject>();
    settingsManagementCompat["enabled"] = true;
    settingsManagementCompat["allow_wifi_config"] = true;
    settingsManagementCompat["allow_webdav_config"] = true;
    settingsManagementCompat["has_wifi_config"] = hasWifiConfig;
    settingsManagementCompat["has_webdav_config"] = hasWebdav;

    String payload;
    serializeJson(doc, payload);
    webServer->send(200, "application/json", payload);
}

void WiFiHotspotManager::handleWebdavConfigGet() {
    if (!webServer) {
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    JsonObject cfg = doc["config"].to<JsonObject>();
    cfg["url"] = g_config.webdav_url;
    cfg["username"] = g_config.webdav_user;
    cfg["password"] = g_config.webdav_pass;

    String payload;
    serializeJson(doc, payload);
    webServer->send(200, "application/json", payload);
}

void WiFiHotspotManager::handleWebdavConfigUpdate() {
    if (!webServer) {
        return;
    }

    String body = webServer->arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"invalid json\"}");
        return;
    }

    JsonObject cfg = doc["config"].is<JsonObject>() ? doc["config"].as<JsonObject>() : JsonObject();

    auto apply_string = [](char *dest, size_t cap, const char *value) {
        if (!dest || cap == 0 || !value) {
            return;
        }
        strncpy(dest, value, cap - 1);
        dest[cap - 1] = '\0';
    };

    const char *url = nullptr;
    const char *user = nullptr;
    const char *pass = nullptr;

    if (!cfg.isNull()) {
        if (cfg["url"].is<const char*>()) url = cfg["url"].as<const char*>();
        if (cfg["username"].is<const char*>()) user = cfg["username"].as<const char*>();
        if (cfg["password"].is<const char*>()) pass = cfg["password"].as<const char*>();
    }
    if (doc["url"].is<const char*>()) url = doc["url"].as<const char*>();
    if (doc["username"].is<const char*>()) user = doc["username"].as<const char*>();
    if (doc["password"].is<const char*>()) pass = doc["password"].as<const char*>();

    if (url) apply_string(g_config.webdav_url, sizeof(g_config.webdav_url), url);
    if (user) apply_string(g_config.webdav_user, sizeof(g_config.webdav_user), user);
    if (pass) apply_string(g_config.webdav_pass, sizeof(g_config.webdav_pass), pass);

    bool saved = config_save();

    JsonDocument resp;
    resp["ok"] = saved;
    if (!saved) {
        resp["message"] = "save failed";
    }
    JsonObject outCfg = resp["config"].to<JsonObject>();
    outCfg["url"] = g_config.webdav_url;
    outCfg["username"] = g_config.webdav_user;
    outCfg["password"] = g_config.webdav_pass;

    String payload;
    serializeJson(resp, payload);
    webServer->send(saved ? 200 : 500, "application/json", payload);
}

void WiFiHotspotManager::handleWifiConfigGet() {
    if (!webServer) {
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    JsonArray configs = doc["configs"].to<JsonArray>();
    
    for (int i = 0; i < 3; i++) {
        JsonObject cfg = configs.add<JsonObject>();
        cfg["ssid"] = g_config.wifi_ssid[i];
        cfg["password"] = g_config.wifi_pass[i];
    }
    
    doc["last_success_idx"] = g_config.wifi_last_success_idx;

    String payload;
    serializeJson(doc, payload);
    webServer->send(200, "application/json", payload);
}

void WiFiHotspotManager::handleWifiConfigUpdate() {
    if (!webServer) {
        return;
    }

    String body = webServer->arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"invalid json\"}");
        return;
    }

    auto apply_string = [](char *dest, size_t cap, const char *value) {
        if (!dest || cap == 0 || !value) {
            return;
        }
        strncpy(dest, value, cap - 1);
        dest[cap - 1] = '\0';
    };

    // 支持新格式（configs数组）
    if (doc["configs"].is<JsonArray>()) {
        JsonArray configs = doc["configs"].as<JsonArray>();
        int idx = 0;
        for (JsonVariant v : configs) {
            if (idx >= 3) break;
            if (v.is<JsonObject>()) {
                JsonObject cfg = v.as<JsonObject>();
                if (cfg["ssid"].is<const char*>()) {
                    apply_string(g_config.wifi_ssid[idx], sizeof(g_config.wifi_ssid[idx]), cfg["ssid"].as<const char*>());
                }
                if (cfg["password"].is<const char*>()) {
                    apply_string(g_config.wifi_pass[idx], sizeof(g_config.wifi_pass[idx]), cfg["password"].as<const char*>());
                }
            }
            idx++;
        }
    }
    
    // 兼容旧格式（单组ssid/password）
    JsonObject cfg = doc["config"].is<JsonObject>() ? doc["config"].as<JsonObject>() : JsonObject();
    const char *ssid = nullptr;
    const char *pass = nullptr;

    if (!cfg.isNull()) {
        if (cfg["ssid"].is<const char*>()) ssid = cfg["ssid"].as<const char*>();
        if (cfg["password"].is<const char*>()) pass = cfg["password"].as<const char*>();
    }
    if (doc["ssid"].is<const char*>()) ssid = doc["ssid"].as<const char*>();
    if (doc["password"].is<const char*>()) pass = doc["password"].as<const char*>();

    if (ssid) apply_string(g_config.wifi_ssid[0], sizeof(g_config.wifi_ssid[0]), ssid);
    if (pass) apply_string(g_config.wifi_pass[0], sizeof(g_config.wifi_pass[0]), pass);

    bool saved = config_save();

    JsonDocument resp;
    resp["ok"] = saved;
    if (!saved) {
        resp["message"] = "save failed";
    }
    JsonArray outConfigs = resp["configs"].to<JsonArray>();
    for (int i = 0; i < 3; i++) {
        JsonObject outCfg = outConfigs.add<JsonObject>();
        outCfg["ssid"] = g_config.wifi_ssid[i];
        outCfg["password"] = g_config.wifi_pass[i];
    }
    resp["last_success_idx"] = g_config.wifi_last_success_idx;

    String payload;
    serializeJson(resp, payload);
    webServer->send(saved ? 200 : 500, "application/json", payload);
}

void WiFiHotspotManager::handleAdvConfigGet() {
    if (!webServer) {
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;

    JsonObject config = doc["config"].to<JsonObject>();
    JsonObject reading = config["reading"].to<JsonObject>();
    JsonObject tradeoff = reading["font_render_tradeoff_level"].to<JsonObject>();
    tradeoff["title"] = "缩放渲染档位";
    uint8_t level = get_font_render_tradeoff_level();
    // UI 仅暴露两档：0=速度，1=高质。内部均衡档(1)对外映射为速度。
    tradeoff["value"] = (level == FONT_RENDER_TRADEOFF_QUALITY) ? 1 : 0;
    JsonArray options = tradeoff["options"].to<JsonArray>();
    options.add("速度");
    options.add("高质");
    tradeoff["hint"] = "速度更快但细节略降；高质更稳但稍慢";

    String payload;
    serializeJson(doc, payload);
    webServer->send(200, "application/json", payload);
}

void WiFiHotspotManager::handleAdvConfigUpdate() {
    if (!webServer) {
        return;
    }

    String body = webServer->arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"invalid json\"}");
        return;
    }

    int level = get_font_render_tradeoff_level();
    bool has_update = false;

    auto extract_level = [&](JsonVariantConst node) -> bool {
        if (node.is<int>()) {
            level = node.as<int>();
            return true;
        }
        if (node.is<JsonObjectConst>()) {
            JsonObjectConst obj = node.as<JsonObjectConst>();
            JsonVariantConst value_node = obj["value"];
            if (value_node.is<int>()) {
                int raw = value_node.as<int>();
                JsonVariantConst options_node = obj["options"];
                if (options_node.is<JsonArrayConst>() && options_node.as<JsonArrayConst>().size() == 2) {
                    // 新 UI 仅两档：0=速度, 1=高质
                    level = (raw <= 0) ? FONT_RENDER_TRADEOFF_FAST : FONT_RENDER_TRADEOFF_QUALITY;
                } else {
                    // 兼容旧三档或纯 value 结构
                    level = raw;
                }
                return true;
            }
        }
        return false;
    };

    if (doc["config"].is<JsonObjectConst>()) {
        JsonObjectConst config = doc["config"].as<JsonObjectConst>();
        if (config["reading"].is<JsonObjectConst>()) {
            JsonObjectConst reading = config["reading"].as<JsonObjectConst>();
            if (extract_level(reading["font_render_tradeoff_level"])) {
                has_update = true;
            }
        }
        if (!has_update && extract_level(config["font_render_tradeoff_level"])) {
            has_update = true;
        }
    }

    if (!has_update && extract_level(doc["font_render_tradeoff_level"])) {
        has_update = true;
    }

    if (has_update) {
        g_config.font_render_tradeoff_level = clamp_font_render_tradeoff_level(level);
    }

    bool saved = config_save();

    JsonDocument resp;
    resp["ok"] = saved;
    if (!saved) {
        resp["message"] = "save failed";
    }
    JsonObject config = resp["config"].to<JsonObject>();
    JsonObject reading = config["reading"].to<JsonObject>();
    JsonObject tradeoff = reading["font_render_tradeoff_level"].to<JsonObject>();
    tradeoff["title"] = "缩放渲染档位";
    uint8_t out_level = get_font_render_tradeoff_level();
    tradeoff["value"] = (out_level == FONT_RENDER_TRADEOFF_QUALITY) ? 1 : 0;
    JsonArray options = tradeoff["options"].to<JsonArray>();
    options.add("速度");
    options.add("高质");
    tradeoff["hint"] = "速度更快但细节略降；高质更稳但稍慢";

    String payload;
    serializeJson(resp, payload);
    webServer->send(saved ? 200 : 500, "application/json", payload);
}

void WiFiHotspotManager::handleNotFound() {
    String message = "File Not Found\n\n";
    message += "URI: " + webServer->uri() + "\n";
    message += "Method: " + String(webServer->method() == HTTP_GET ? "GET" : "POST") + "\n";
    message += "Arguments: " + String(webServer->args()) + "\n";
    
    for (uint8_t i = 0; i < webServer->args(); i++) {
        message += " " + webServer->argName(i) + ": " + webServer->arg(i) + "\n";
    }
    
    webServer->send(404, "text/plain", message);
}

void WiFiHotspotManager::handleFileUploadPost() {
    HTTPUpload& upload = webServer->upload();
    
    static File uploadFile;
    static String uploadTab = "";
    static String uploadDir = "/";  // 默认上传目录
    static String fullPath = "";    // 完整文件路径
    static String tmpPath = "";     // 临时文件路径（写入期间使用）
    static unsigned long lastWriteTime = 0;
    static size_t totalBytesWritten = 0; // 实际写入的字节数
    static unsigned long uploadStartTime = 0; // 上传开始时间
    const unsigned long UPLOAD_TIMEOUT = 300000; // 上传超时时间300秒（5分钟）支持大文件
    
    if (upload.status == UPLOAD_FILE_START) {
        // 设置上传状态标志
        uploadInProgress = true;
        
        String filename = upload.filename;
        if (!filename.startsWith("/")) filename = "/" + filename;
        
        // 检查内存状况 - 只需要足够的缓冲区内存用于流式处理
        size_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < 32768) { // 需要至少32KB内存用于缓冲区和基本操作
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] 内存不足，拒绝上传: %u bytes (流式处理需要至少32KB)\n", freeHeap);
#endif
            webServer->sendHeader("Connection", "close");
            webServer->sendHeader("Access-Control-Allow-Origin", "*");
            webServer->send(507, "application/json", String("{\"ok\":false,\"message\":\"Insufficient memory for streaming upload - need at least 32KB free\"}"));
            return;
        }
        
        // 重置状态变量
        totalBytesWritten = 0;
        uploadStartTime = millis();
        
        // 检查文件大小限制 - 提升到50MB支持大文本文件
        const size_t maxFileSize = 50 * 1024 * 1024; // 50MB限制，支持大文本文件
        if (upload.totalSize > maxFileSize) {
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] 文件过大: %u bytes (最大支持20MB)\n", upload.totalSize);
#endif
            webServer->sendHeader("Connection", "close");
            webServer->sendHeader("Access-Control-Allow-Origin", "*");
            webServer->send(413, "application/json", String("{\"ok\":false,\"message\":\"File too large - maximum 20MB supported\"}"));
            return;
        }
        
        // 获取tab参数，确定上传目录
        uploadTab = webServer->arg("tab");
        uploadDir = "/";
        if (uploadTab == "book") {
            uploadDir = "/book/";
            // 支持子目录：webapp 侧会传 subdir=manga 或 subdir=manga/sub 等
            String subdir = webServer->arg("subdir");
            if (subdir.length() > 0) {
                // 去掉首尾的 '/' 防止路径重复
                while (subdir.startsWith("/")) subdir = subdir.substring(1);
                while (subdir.endsWith("/"))   subdir = subdir.substring(0, subdir.length()-1);
                if (subdir.length() > 0) uploadDir = "/book/" + subdir + "/";
            }
        } else if (uploadTab == "font") {
            uploadDir = "/font/";
        } else if (uploadTab == "image") {
            uploadDir = "/image/";
        }

        // 支持特殊 tab=scback：不使用上传文件名，而是强制保存为 SD 根目录下的 /scback.png
        if (uploadTab == "scback") {
            filename = "/scback.png"; // 强制目标文件名
        }

        // 构建完整路径
        fullPath = uploadDir + filename.substring(1); // 去掉开头的/
        
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] 开始上传文件: %s (%u bytes)\n", fullPath.c_str(), upload.totalSize);
#endif
        
        // 检查SD卡可用性
    uint64_t cardSize = SDW::SD.cardSize() / (1024 * 1024); // MB
    uint64_t usedBytes = SDW::SD.usedBytes() / (1024 * 1024); // MB
        uint64_t freeBytes = (SDW::SD.cardSize() - SDW::SD.usedBytes()) / (1024 * 1024); // MB
    (void)cardSize; // might only be used in debug prints
    (void)usedBytes; // might only be used in debug prints
        
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] SD卡状态: 总计 %llu MB, 已用 %llu MB, 剩余 %llu MB\n", 
                    cardSize, usedBytes, freeBytes);
#endif
        
        if (freeBytes < (upload.totalSize / (1024 * 1024) + 10)) { // 预留10MB空间
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] SD卡空间不足，需要 %u MB，剩余 %llu MB\n", 
                        (upload.totalSize / (1024 * 1024) + 1), freeBytes);
#endif
            webServer->sendHeader("Connection", "close");
            webServer->sendHeader("Access-Control-Allow-Origin", "*");
            webServer->send(507, "application/json", String("{\"ok\":false,\"message\":\"Insufficient storage space\"}"));
            return;
        }
        
        // 确保目录存在
        String dirPath = fullPath.substring(0, fullPath.lastIndexOf('/'));
        if (!SDW::SD.exists(dirPath.c_str())) {
            // 创建目录
            SDW::SD.mkdir(dirPath.c_str());
        }
        
        // 使用临时文件写入，上传完成后再重命名到目标路径，避免部分写入被认为存在
        tmpPath = fullPath + ".tmp";
        if (SDW::SD.exists(tmpPath.c_str())) {
            SDW::SD.remove(tmpPath.c_str());
        }

    uploadFile = SDW::SD.open(tmpPath, "w");
        if (!uploadFile) {
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] 错误: 无法创建文件 %s\n", fullPath.c_str());
#endif
            webServer->sendHeader("Connection", "close");
            webServer->sendHeader("Access-Control-Allow-Origin", "*");
            webServer->send(500, "application/json", String("{\"ok\":false,\"message\":\"Failed to create file\"}"));
            return;
        }
        
        lastWriteTime = millis();
        
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        // 定期yielding防止看门狗超时和网络冲突
        static unsigned long lastYieldTime = 0;
        if (millis() - lastYieldTime > 20) { // 每20ms yield一次，防止看门狗超时
            yield();
            delay(1); // 短暂延迟让系统稳定
            lastYieldTime = millis();
        }
        
        // 检查上传超时
        if (millis() - uploadStartTime > UPLOAD_TIMEOUT) {
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] 上传超时，已用时: %lu ms\n", millis() - uploadStartTime);
#endif
            if (uploadFile) {
                uploadFile.close();
                if (SDW::SD.exists(tmpPath.c_str())) SDW::SD.remove(tmpPath.c_str());
            }
            webServer->sendHeader("Connection", "close");
            webServer->sendHeader("Access-Control-Allow-Origin", "*");
            webServer->send(408, "application/json", String("{\"ok\":false,\"message\":\"Upload timeout\"}"));
            return;
        }
        
        if (uploadFile) {
            // 检查可用内存 - 流式处理只需要基本缓冲区内存
            size_t freeHeap = ESP.getFreeHeap();
            if (freeHeap < 24576) { // 需要至少24KB内存用于缓冲区和网络处理
#if DBG_WIFI_HOTSPOT
                Serial.printf("[WIFI_HOTSPOT] 内存不足 (%u bytes)，暂停写入\n", freeHeap);
#endif
                delay(50); // 稍微延迟让系统恢复
                yield(); // 让出CPU防止看门狗超时
                return;
            }
            
            // 写入数据 - 流式处理，每次只处理小块数据
            if (upload.currentSize > 0) {
                // 添加看门狗重置和CPU yielding
                yield(); // 让出CPU
                
                size_t bytesToWrite = upload.currentSize;
                size_t bytesWritten = uploadFile.write(upload.buf, bytesToWrite);
                
                if (bytesWritten != bytesToWrite) {
#if DBG_WIFI_HOTSPOT
                    Serial.printf("[WIFI_HOTSPOT] 写入失败: 期望 %u, 实际 %u\n", bytesToWrite, bytesWritten);
#endif
                    uploadFile.close();
                    if (SDW::SD.exists(tmpPath.c_str())) SDW::SD.remove(tmpPath.c_str());
                    webServer->sendHeader("Connection", "close");
                    webServer->sendHeader("Access-Control-Allow-Origin", "*");
                    webServer->send(500, "application/json", String("{\"ok\":false,\"message\":\"Write failed\"}"));
                    return;
                }
                
                totalBytesWritten += bytesWritten;
                lastWriteTime = millis();
                
                // 优化刷新频率，减少IO开销
                if (totalBytesWritten % (16 * 1024) == 0) { // 每16KB刷新一次，平衡性能和可靠性
                    uploadFile.flush();
                    yield(); // 让出CPU防止看门狗超时
                    delay(2); // 稍长延迟让SD卡完成写入
                }
                
#if DBG_WIFI_HOTSPOT
                if (totalBytesWritten % (100 * 1024) == 0) { // 每100KB打印一次进度
                    float progress = (float)totalBytesWritten / upload.totalSize * 100;
                    Serial.printf("[WIFI_HOTSPOT] 上传进度: %.1f%% (%u/%u bytes), 内存: %u\n", 
                                progress, totalBytesWritten, upload.totalSize, ESP.getFreeHeap());
                    yield(); // 让出CPU
                }
#endif
            }
        }
        
    } else if (upload.status == UPLOAD_FILE_END) {
        // 上传完成时的CPU yielding
        yield(); 
        
            if (uploadFile) {
                uploadFile.flush(); // 确保所有数据写入
                uploadFile.close();

                // 文件验证前的内存检查和yielding
            if (ESP.getFreeHeap() < 16384) {
#if DBG_WIFI_HOTSPOT
                Serial.printf("[WIFI_HOTSPOT] 内存不足，跳过文件验证\n");
#endif
                webServer->sendHeader("Connection", "close");
                webServer->sendHeader("Access-Control-Allow-Origin", "*");
                webServer->send(200, "application/json", String("{\"ok\":true,\"message\":\"File uploaded (verification skipped due to low memory)\"}"));
                return;
            }
            
            yield(); // 让出CPU
            
            // 验证文件完整性 - 使用更宽松的检查
            // 由于写入的是临时文件，先验证临时文件再重命名
            File verifyFile = SDW::SD.open(tmpPath.c_str(), "r");
            if (verifyFile) {
                size_t actualFileSize = verifyFile.size();
                verifyFile.close();

                // 允许小幅差异（最多1%或1KB，取较小值），因为multipart传输可能有边界字符差异
                size_t tolerance = std::min((size_t)(upload.totalSize * 0.01), (size_t)1024);
                size_t sizeDiff = (actualFileSize > upload.totalSize) ?
                                 (actualFileSize - upload.totalSize) :
                                 (upload.totalSize - actualFileSize);

                if (sizeDiff > tolerance) {
#if DBG_WIFI_HOTSPOT
                    Serial.printf("[WIFI_HOTSPOT] 文件大小差异过大: 期望 %u, 实际 %u, 差异 %u (容忍度 %u)\n",
                                  upload.totalSize, actualFileSize, sizeDiff, tolerance);
#endif
                    if (SDW::SD.exists(tmpPath.c_str())) SDW::SD.remove(tmpPath.c_str());
                    webServer->sendHeader("Connection", "close");
                    webServer->sendHeader("Access-Control-Allow-Origin", "*");
                    webServer->send(500, "application/json", String("{\"ok\":false,\"message\":\"File size mismatch, upload corrupted\"}"));
                    return;
                }

#if DBG_WIFI_HOTSPOT
                unsigned long uploadTime = millis() - uploadStartTime;
                float speed = actualFileSize / (uploadTime / 1000.0) / 1024.0; // KB/s
                Serial.printf("[WIFI_HOTSPOT] 文件上传完成: %s, 大小: %u bytes, 耗时: %lu ms, 速度: %.1f KB/s\n",
                            fullPath.c_str(), actualFileSize, uploadTime, speed);
#endif

                // 验证成功，准备覆盖原有文件。如果直接删除失败，使用备份路径暂存旧文件。
                bool backupUsed = false;
                String backupPath = "";
                auto ensureOverwriteSlot = [&]() -> bool {
                    if (!SDW::SD.exists(fullPath.c_str())) return true;
                    if (SDW::SD.remove(fullPath.c_str())) return true;
                    backupPath = fullPath + String(".upload.bak");
                    int attempt = 0;
                    while (SDW::SD.exists(backupPath.c_str()) && attempt < 5) {
                        backupPath = fullPath + String(".upload.bak") + String(++attempt);
                    }
                    if (SDW::SD.rename(fullPath.c_str(), backupPath.c_str())) {
                        backupUsed = true;
                        return true;
                    }
                    backupPath = "";
                    backupUsed = false;
                    return false;
                };

                if (!ensureOverwriteSlot()) {
#if DBG_WIFI_HOTSPOT
                    Serial.printf("[WIFI_HOTSPOT] 无法覆盖已有文件: %s\n", fullPath.c_str());
#endif
                    if (SDW::SD.exists(tmpPath.c_str())) SDW::SD.remove(tmpPath.c_str());
                    webServer->sendHeader("Connection", "close");
                    webServer->sendHeader("Access-Control-Allow-Origin", "*");
                    webServer->send(500, "application/json", String("{\"ok\":false,\"message\":\"Cannot overwrite existing file\"}"));
                    return;
                }

                bool renamed = SDW::SD.rename(tmpPath.c_str(), fullPath.c_str());
                if (!renamed) {
                    // 无法重命名，尝试恢复备份并返回错误
                    if (backupUsed && backupPath.length()) {
                        if (!SDW::SD.exists(fullPath.c_str())) {
                            SDW::SD.rename(backupPath.c_str(), fullPath.c_str());
                        } else {
                            SDW::SD.remove(backupPath.c_str());
                        }
                    }
                    if (SDW::SD.exists(tmpPath.c_str())) SDW::SD.remove(tmpPath.c_str());
                    webServer->sendHeader("Connection", "close");
                    webServer->sendHeader("Access-Control-Allow-Origin", "*");
                    webServer->send(500, "application/json", String("{\"ok\":false,\"message\":\"Failed to finalize uploaded file\"}"));
                    return;
                }

                if (backupUsed && backupPath.length() && SDW::SD.exists(backupPath.c_str())) {
                    SDW::SD.remove(backupPath.c_str());
                }

                // 发送成功响应，添加CORS头
                webServer->sendHeader("Connection", "close");
                webServer->sendHeader("Access-Control-Allow-Origin", "*");
                webServer->send(200, "application/json", String("{\"ok\":true,\"message\":\"File uploaded successfully\"}"));

                // 如果上传到字体目录，刷新全局字体列表
                if (fullPath.startsWith("/font/")) {
                    font_list_scan();
                }

                // 如果上传到书籍目录，刷新书籍缓存并检测覆盖
                if (fullPath.startsWith("/book/")) {
                    BookFileManager::refreshCache();

                    if (g_current_book) {
                        std::string cur_fp = g_current_book->filePath();
                        std::string cur_real;
                        bool cur_use_spiffs = false;
                        resolve_fake_path(cur_fp, cur_real, cur_use_spiffs);
                        cur_real = normalize_real_path(cur_real);

                        std::string up_fp = std::string(fullPath.c_str());
                        std::string up_real;
                        bool up_use_spiffs = false;
                        resolve_fake_path(up_fp, up_real, up_use_spiffs);
                        up_real = normalize_real_path(up_real);

#if DBG_WIFI_HOTSPOT
                        Serial.printf("[WIFI_HOTSPOT] normalized current='%s' (spiffs=%d) uploaded='%s' (spiffs=%d)\n",
                                      cur_real.c_str(), cur_use_spiffs, up_real.c_str(), up_use_spiffs);
#endif

                        if (cur_real == up_real && cur_use_spiffs == up_use_spiffs) {
#if DBG_WIFI_HOTSPOT
                            Serial.printf("[WIFI_HOTSPOT] 当前书籍已被覆盖，触发强制重建索引: %s\n", fullPath.c_str());
#endif
                            extern void requestForceReindex();
                            requestForceReindex();
                        }
                    }
                }
                // 如果上传到图片目录，清理锁屏图片缓存以便下次重新扫描
                if (fullPath.startsWith("/image/")) {
                    lockscreen_image_cache_invalidate();
                }
            } else {
#if DBG_WIFI_HOTSPOT
                Serial.printf("[WIFI_HOTSPOT] 无法验证上传文件: %s\n", fullPath.c_str());
#endif
                webServer->sendHeader("Connection", "close");
                webServer->sendHeader("Access-Control-Allow-Origin", "*");
                webServer->send(500, "application/json", String("{\"ok\":false,\"message\":\"Cannot verify uploaded file\"}"));
            }
        } else {
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] 上传结束但文件句柄无效\n");
#endif
            webServer->sendHeader("Connection", "close");
            webServer->sendHeader("Access-Control-Allow-Origin", "*");
            webServer->send(500, "application/json", String("{\"ok\":false,\"message\":\"Invalid file handle\"}"));
        }
        
    // 清除上传状态标志
        uploadInProgress = false;
        
        // 重置状态变量
        uploadTab = "";
        fullPath = "";
        totalBytesWritten = 0;
        uploadStartTime = 0;
    (void)lastWriteTime; // suppress unused-but-set warning
        
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        // 处理上传中止情况
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] 文件上传被中止: %s (已写入 %u bytes)\n", fullPath.c_str(), totalBytesWritten);
#endif
        if (uploadFile) {
            uploadFile.close();
        }
        if (SDW::SD.exists(fullPath)) {
            SDW::SD.remove(fullPath); // 删除不完整的文件
        }
        
    // 清除上传状态标志
    uploadInProgress = false;
        
        // 重置状态变量
        uploadTab = "";
        fullPath = "";
        totalBytesWritten = 0;
        uploadStartTime = 0;
        
    webServer->sendHeader("Connection", "close");
    webServer->sendHeader("Access-Control-Allow-Origin", "*");
    webServer->send(500, "application/json", String("{\"ok\":false,\"message\":\"Upload aborted\"}"));

        // 如果上传目标在字体目录，刷新字体列表以移除可能的残留项
        if (fullPath.startsWith("/font/")) {
            font_list_scan();
        }
    }
}

String WiFiHotspotManager::formatFileSize(size_t bytes) {
    if (bytes < 1024) {
        return String(bytes) + " B";
    } else if (bytes < 1024 * 1024) {
        return String(bytes / 1024.0, 1) + " KB";
    } else {
        return String(bytes / (1024.0 * 1024.0), 1) + " MB";
    }
}

String WiFiHotspotManager::getContentType(String filename) {
    if (filename.endsWith(".htm")) return "text/html";
    else if (filename.endsWith(".html")) return "text/html";
    else if (filename.endsWith(".css")) return "text/css";
    else if (filename.endsWith(".js")) return "application/javascript";
    else if (filename.endsWith(".png")) return "image/png";
    else if (filename.endsWith(".gif")) return "image/gif";
    else if (filename.endsWith(".jpg")) return "image/jpeg";
    else if (filename.endsWith(".ico")) return "image/x-icon";
    else if (filename.endsWith(".xml")) return "text/xml";
    else if (filename.endsWith(".pdf")) return "application/x-pdf";
    else if (filename.endsWith(".zip")) return "application/x-zip";
    else if (filename.endsWith(".gz")) return "application/x-gzip";
    else if (filename.endsWith(".txt")) return "text/plain";
    return "application/octet-stream";
}

bool WiFiHotspotManager::handleFileRead(String path) {
    if (path.endsWith("/")) path += "index.htm";
    String contentType = getContentType(path);
    // 优先 SPIFFS
    if (SPIFFS.exists(path)) {
        File file = SPIFFS.open(path, "r");
        if (file && !file.isDirectory()) {
            webServer->streamFile(file, contentType);
            file.close();
            return true;
        }
    }
    // 再查 SD 卡
    File file = SDW::SD.open(path, "r");
    if (file && !file.isDirectory()) {
        webServer->streamFile(file, contentType);
        file.close();
        return true;
    }
    return false;
}

String WiFiHotspotManager::generateWebPage() {
#if DBG_WIFI_HOTSPOT
    unsigned long startTime = millis();
#endif
    
    // 优先从SPIFFS读取模板，使用分块读取避免大内存占用
    if (SPIFFS.exists("/template.html")) {
        File file = SPIFFS.open("/template.html", "r");
        if (file && file.size() > 0) {
            // 检查文件大小，如果太大则使用备用模板
            size_t fileSize = file.size();
            if (fileSize > 150000) { // 限制模板文件大小为150KB
#if DBG_WIFI_HOTSPOT
                Serial.printf("[WIFI_HOTSPOT] 模板文件过大 (%d bytes)，使用内置模板\n", fileSize);
#endif
                file.close();
                return generateFallbackTemplate();
            }
            
            // 使用预分配的String减少内存碎片
            String html;
            html.reserve(fileSize + 100); // 预留一点额外空间
            
            // 分块读取文件，减少内存峰值
            char buffer[512];
            while (file.available()) {
                size_t bytesRead = file.readBytes(buffer, sizeof(buffer));
                html.concat(buffer, bytesRead);
                
                // 内存检查
                if (ESP.getFreeHeap() < 8192) { // 保留8KB内存
#if DBG_WIFI_HOTSPOT
                    Serial.printf("[WIFI_HOTSPOT] 内存不足，中止模板加载\n");
#endif
                    file.close();
                    return generateFallbackTemplate();
                }
            }
            file.close();
            
            // 读取版本信息并替换 #curver 占位符
            String version = "";
            if (SPIFFS.exists("/version")) {
                File versionFile = SPIFFS.open("/version", "r");
                if (versionFile && versionFile.size() > 0) {
                    // 读取version文件的最后一行（版本号）
                    while (versionFile.available()) {
                        String line = versionFile.readStringUntil('\n');
                        line.trim();
                        if (line.length() > 0) {
                            version = line; // 保留最后一个非空行
                        }
                    }
                    versionFile.close();
                }
            }
            
            // 如果成功读取到版本号，替换HTML中的占位符
            if (version.length() > 0) {
                String placeholder = "<span id=\"curver\"></span>";
                String replacement = "<span id=\"curver\">" + version + "</span>";
                html.replace(placeholder, replacement);
#if DBG_WIFI_HOTSPOT
                Serial.printf("[WIFI_HOTSPOT] 版本信息已填充: %s\n", version.c_str());
#endif
            }
            
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] 从SPIFFS读取模板成功，大小: %d bytes, 耗时: %lu ms, 剩余内存: %d\n", 
                         html.length(), millis() - startTime, ESP.getFreeHeap());
#endif
            return html;
        }
#if DBG_WIFI_HOTSPOT
        Serial.println("[WIFI_HOTSPOT] SPIFFS模板文件打开失败，使用内置模板");
#endif
    } else {
#if DBG_WIFI_HOTSPOT
        Serial.println("[WIFI_HOTSPOT] SPIFFS中未找到template.html，使用内置模板");
#endif
    }
    
    return generateFallbackTemplate();
}

String WiFiHotspotManager::generateFallbackTemplate() {
    // 兜底：内置简化模板，仅显示错误提示
    return F(R"html(<!DOCTYPE html>
<html>
<head>
    <meta charset='utf-8'>
    <title>模板文件缺失</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; color: #b00; background: #fff8f8; }
        .err-box { border: 2px solid #b00; background: #fff0f0; padding: 30px; border-radius: 8px; max-width: 500px; margin: 60px auto; text-align: center; }
        h2 { color: #b00; }
    </style>
</head>
<body>
    <div class='err-box'>
        <h2>模板文件未找到</h2>
        <p>请将 <b>template.html</b> 上传到 SPIFFS 后重启设备。</p>
        <p>如需恢复功能，请参考文档或联系开发者。</p>
    </div>
</body>
</html>)html");
}

String WiFiHotspotManager::generateUploadForm() {
    return generateWebPage(); // 使用同一个页面
}

void wifi_hotspot_init() {
    if (!g_wifi_hotspot) {
        g_wifi_hotspot = new WiFiHotspotManager();
    }
}

void wifi_hotspot_cleanup() {
    if (g_wifi_hotspot) {
        delete g_wifi_hotspot;
        g_wifi_hotspot = nullptr;
    }
}

bool WiFiHotspotManager::connectToWiFiFromToken() {
#if DBG_WIFI_HOTSPOT
    Serial.println("[WIFI_HOTSPOT] 尝试从配置连接WiFi...");
#endif

    // 确保NVS已初始化（STA连接需要WiFi/NVS就绪）
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
#if DBG_WIFI_HOTSPOT
        Serial.println("[WIFI_HOTSPOT] NVS分区满或版本不匹配，正在擦除并重新初始化...");
#endif
        nvs_flash_erase();
        nvs_ret = nvs_flash_init();
    }
#if DBG_WIFI_HOTSPOT
    if (nvs_ret != ESP_OK) {
        Serial.printf("[WIFI_HOTSPOT] 错误: NVS初始化失败 (%s)\n", esp_err_to_name(nvs_ret));
    }
#endif

    // 重置连接状态
    extern bool g_wifi_sta_connected;
    g_wifi_sta_connected = false;

    // 停止热点模式（如果正在运行）
    if (running) {
        stop();
        delay(500);
    }

    // 切换到STA模式
    WiFi.mode(WIFI_STA);
    delay(500);

    // 构建尝试顺序：优先尝试最近成功的WiFi
    int try_order[3];
    int try_count = 0;
    
    // 先添加最近成功的索引
    if (g_config.wifi_last_success_idx >= 0 && g_config.wifi_last_success_idx < 3) {
        String ssid = String(g_config.wifi_ssid[g_config.wifi_last_success_idx]).c_str();
        ssid.trim();
        if (ssid.length() > 0) {
            try_order[try_count++] = g_config.wifi_last_success_idx;
        }
    }
    
    // 再添加其他配置
    for (int i = 0; i < 3; i++) {
        if (i == g_config.wifi_last_success_idx) {
            continue; // 跳过已添加的
        }
        String ssid = String(g_config.wifi_ssid[i]).c_str();
        ssid.trim();
        if (ssid.length() > 0) {
            try_order[try_count++] = i;
        }
    }

    if (try_count == 0) {
#if DBG_WIFI_HOTSPOT
        Serial.println("[WIFI_HOTSPOT] 错误: 没有配置任何WiFi");
#endif
        WiFi.mode(WIFI_OFF);
        return false;
    }

#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] 找到 %d 组WiFi配置\n", try_count);
#endif

    // 逐个尝试连接；对每个 AP 做有限次重试再轮换，避免卡在单个不可达 AP 上
    const int per_ap_retries = 3; // 每个 AP 最多重试次数
    for (int attempt = 0; attempt < try_count; attempt++) {
        int idx = try_order[attempt];
        String ssid = String(g_config.wifi_ssid[idx]).c_str();
        String password = String(g_config.wifi_pass[idx]).c_str();
        ssid.trim();
        password.trim();

        bool connected_for_this_ap = false;

        for (int rep = 0; rep < per_ap_retries; rep++) {
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] 尝试连接 (AP %d/%d) 第 %d 次: '%s'\n", 
                          attempt + 1, try_count, rep + 1, ssid.c_str());
#endif

            // 每次重试前确保已断开并短暂等待以清理状态
            WiFi.disconnect();
            delay(200);

            // 开始连接
            WiFi.begin(ssid.c_str(), password.c_str());

            // 等待连接，最多5秒（10 * 500ms）
            int timeout = 10; // 10 * 500ms = 5秒
            while (WiFi.status() != WL_CONNECTED && timeout > 0) {
                delay(500);
                timeout--;
#if DBG_WIFI_HOTSPOT
                Serial.print(".");
#endif
            }
#if DBG_WIFI_HOTSPOT
            Serial.println();
#endif

            if (WiFi.status() == WL_CONNECTED) {
#if DBG_WIFI_HOTSPOT
                Serial.printf("[WIFI_HOTSPOT] ✅ WiFi连接成功: %s\n", ssid.c_str());
                Serial.printf("[WIFI_HOTSPOT] IP地址: %s\n", WiFi.localIP().toString().c_str());
#endif
                // 更新最近成功的索引
                if (g_config.wifi_last_success_idx != idx) {
                    g_config.wifi_last_success_idx = idx;
                    config_save(); // 保存配置
#if DBG_WIFI_HOTSPOT
                    Serial.printf("[WIFI_HOTSPOT] 更新最近成功WiFi索引: %d\n", idx);
#endif
                }

                g_wifi_sta_connected = true;

                // 同步网络时间（HTTPS证书验证需要正确的系统时间）
#if DBG_WIFI_HOTSPOT
                Serial.println("[WIFI_HOTSPOT] 正在同步网络时间...");
#endif
                configTime(8 * 3600, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");

                struct tm timeinfo;
                if (getLocalTime(&timeinfo, 0)) {
#if DBG_WIFI_HOTSPOT
                    Serial.printf("[WIFI_HOTSPOT] ✅ 时间同步成功: %04d-%02d-%02d %02d:%02d:%02d\n",
                                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
#endif
                } else {
#if DBG_WIFI_HOTSPOT
                    Serial.println("[WIFI_HOTSPOT] ⚠️ 时间同步超时，HTTPS请求可能失败");
#endif
                }

                connected_for_this_ap = true;
                break; // 跳出 per-ap 重试循环
            } else {
#if DBG_WIFI_HOTSPOT
                Serial.printf("[WIFI_HOTSPOT] ❌ 连接失败 (AP %d 次): %s\n", rep + 1, ssid.c_str());
#endif
                WiFi.disconnect();
                delay(1000); // 重试间隔
            }
        } // end per-ap retries

        if (connected_for_this_ap) {
            return true; // 成功连接，返回
        }

        // 否则继续尝试下一个 AP（如果有）
    }

    // 所有WiFi都尝试失败
#if DBG_WIFI_HOTSPOT
    Serial.println("[WIFI_HOTSPOT] ❌ 所有WiFi配置都连接失败");
#endif
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    g_wifi_sta_connected = false;
    return false;
}

void WiFiHotspotManager::disconnectWiFi() {
#if DBG_WIFI_HOTSPOT
    Serial.println("[WIFI_HOTSPOT] 断开WiFi连接...");
#endif

    // 分阶段安全关闭WiFi，给lwIP/TCP协议栈足够时间清理资源
    // 1. 先断开WiFi连接（不立即关闭模式）
    WiFi.disconnect(true, false);  // disconnect(wifioff=true, eraseap=false)
    
    // 2. 等待100ms让lwIP处理断开事件和清理活跃连接
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 3. 再关闭WiFi模式
    WiFi.mode(WIFI_OFF);
    
    extern bool g_wifi_sta_connected;
    g_wifi_sta_connected = false;

#if DBG_WIFI_HOTSPOT
    Serial.println("[WIFI_HOTSPOT] WiFi已断开");
#endif
}

static void wifi_disconnect_task(void *param) {
    uint32_t delay_ms = param ? *static_cast<uint32_t *>(param) : 0;
    if (param) {
        delete static_cast<uint32_t *>(param);
    }
    if (delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    if (g_wifi_hotspot) {
        g_wifi_hotspot->disconnectWiFi();
    }
    vTaskDelete(nullptr);
}

void WiFiHotspotManager::disconnectWiFiDeferred(uint32_t delay_ms) {
    uint32_t *payload = new uint32_t(delay_ms);
    // 增加堆栈到4096字节，优先级提高到2，固定在核心1上执行
    if (xTaskCreatePinnedToCore(wifi_disconnect_task, "WiFiDisc", 4096, payload, 2, nullptr, 1) != pdPASS) {
        delete payload;
        disconnectWiFi();
    }
}

bool WiFiHotspotManager::ensureWebdavReadpaperDir() {
    if (g_config.webdav_url[0] == '\0') {
#if DBG_WIFI_HOTSPOT
        Serial.println("[WIFI_HOTSPOT] WebDAV 未配置，跳过连接");
#endif
        return false;
    }

        String base = String(g_config.webdav_url);
    base.trim();
            if (!(base.startsWith("http://") || base.startsWith("https://"))) {
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] WebDAV 地址无效: %s\n", base.c_str());
#endif
        return false;
    }

    if (!base.endsWith("/")) {
        base += "/";
    }
    String base_url = base;
    String target = base + "readpaper/";

    String webdav_user = String(g_config.webdav_user);
    String webdav_pass = String(g_config.webdav_pass);
    webdav_user.trim();
    webdav_pass.trim();
    bool has_auth = (webdav_user.length() > 0 || webdav_pass.length() > 0);

#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] WebDAV Base: %s\n", base_url.c_str());
    Serial.printf("[WIFI_HOTSPOT] WebDAV Target: %s\n", target.c_str());
    Serial.printf("[WIFI_HOTSPOT] WebDAV User: %s\n", webdav_user.c_str());
    Serial.printf("[WIFI_HOTSPOT] WebDAV Pass: %s\n", webdav_pass.c_str());
    Serial.printf("[WIFI_HOTSPOT] WebDAV Auth: %s\n", has_auth ? "on" : "off");
#endif

    auto build_absolute_url = [](const String &base_url, const String &location) -> String {
        if (location.startsWith("http://") || location.startsWith("https://")) {
            return location;
        }
        String base = base_url;
        int scheme_pos = base.indexOf("://");
        if (scheme_pos < 0) {
            return location;
        }
        int host_start = scheme_pos + 3;
        int host_end = base.indexOf('/', host_start);
        String origin = (host_end > 0) ? base.substring(0, host_end) : base;
        if (location.startsWith("/")) {
            return origin + location;
        }
        if (!origin.endsWith("/")) {
            origin += "/";
        }
        return origin + location;
    };

    auto send_request = [&](const String &url, esp_http_client_method_t method, bool add_depth, int &outCode) -> bool {
        String current_url = url;
        const int max_redirects = 5;

        auto do_request = [&](const String &req_url, esp_http_client_auth_type_t auth_type, bool add_basic_header, int &code_out, String &redirect_out) -> bool {
            esp_http_client_config_t cfg = {};
            cfg.url = req_url.c_str();
            cfg.timeout_ms = 8000;
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
            cfg.disable_auto_redirect = true;
            cfg.method = method;

            if (auth_type != HTTP_AUTH_TYPE_NONE && has_auth) {
                cfg.auth_type = auth_type;
                cfg.username = webdav_user.c_str();
                cfg.password = webdav_pass.c_str();
            }

            esp_http_client_handle_t client = esp_http_client_init(&cfg);
            if (!client) {
                return false;
            }

            esp_http_client_set_method(client, method);
            if (add_depth) {
                esp_http_client_set_header(client, "Depth", "0");
            }
            esp_http_client_set_header(client, "User-Agent", "ReadPaper-WebDAV");

            if (add_basic_header && has_auth) {
                char auth_raw[160] = {0};
                snprintf(auth_raw, sizeof(auth_raw), "%s:%s", webdav_user.c_str(), webdav_pass.c_str());
                unsigned char b64[256] = {0};
                size_t out_len = 0;
                if (mbedtls_base64_encode(b64, sizeof(b64) - 1, &out_len,
                                          reinterpret_cast<const unsigned char *>(auth_raw),
                                          strlen(auth_raw)) == 0) {
                    b64[out_len] = '\0';
                    String auth_header = String("Basic ") + reinterpret_cast<const char *>(b64);
                    esp_http_client_set_header(client, "Authorization", auth_header.c_str());
#if DBG_WIFI_HOTSPOT
                    // Log first few chars to verify encoding occurred
                    Serial.printf("[WIFI_HOTSPOT] Gen Auth Header: Basic %.5s... (Len: %d)\n", (char*)b64, out_len);
#endif
                }
            }

            if (method == HTTP_METHOD_MKCOL) {
                esp_http_client_set_post_field(client, "", 0);
            }

            esp_err_t err = esp_http_client_open(client, 0);
            if (err != ESP_OK) {
                esp_http_client_cleanup(client);
                return false;
            }

            err = esp_http_client_fetch_headers(client);
            if (err < 0) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return false;
            }

            code_out = esp_http_client_get_status_code(client);
            if (code_out == 401) {
#if DBG_WIFI_HOTSPOT
                char body[256] = {0};
                int n = esp_http_client_read_response(client, body, sizeof(body) - 1);
                if (n > 0) {
                    body[n] = '\0';
                    Serial.printf("[WIFI_HOTSPOT] WebDAV 401 body: %s\n", body);
                }
                char *auth_hdr = nullptr;
                if (esp_http_client_get_header(client, "WWW-Authenticate", &auth_hdr) == ESP_OK && auth_hdr) {
                    Serial.printf("[WIFI_HOTSPOT] WebDAV 401 WWW-Authenticate: %s\n", auth_hdr);
                } else if (esp_http_client_get_header(client, "www-authenticate", &auth_hdr) == ESP_OK && auth_hdr) {
                    Serial.printf("[WIFI_HOTSPOT] WebDAV 401 www-authenticate: %s\n", auth_hdr);
                } else {
                    Serial.println("[WIFI_HOTSPOT] WebDAV 401 (no WWW-Authenticate header)");
                }
#endif
            }
            redirect_out = "";
            if (code_out >= 300 && code_out < 400) {
                char *loc = nullptr;
                if (esp_http_client_get_header(client, "Location", &loc) == ESP_OK && loc) {
                    redirect_out = String(loc);
                }
            }

            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return true;
        };

        for (int i = 0; i <= max_redirects; ++i) {
            String redirect;
            if (!do_request(current_url, HTTP_AUTH_TYPE_NONE, has_auth, outCode, redirect)) {
                return false;
            }

            if (outCode == 401 && has_auth) {
                if (!do_request(current_url, HTTP_AUTH_TYPE_DIGEST, false, outCode, redirect)) {
                    return false;
                }
            }

            if (outCode >= 300 && outCode < 400) {
                if (redirect.length() == 0) {
                    return false;
                }
                current_url = build_absolute_url(current_url, redirect);
                continue;
            }

            return true;
        }

        return false;
    };

    int mkCode = 0;
    int optCode = 0;
    int propCode = 0;
    // OPTIONS to get auth challenge
    if (!send_request(base_url, HTTP_METHOD_OPTIONS, false, optCode)) {
#if DBG_WIFI_HOTSPOT
        Serial.println("[WIFI_HOTSPOT] WebDAV OPTIONS 请求失败");
#endif
        return false;
    }
#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] WebDAV OPTIONS 返回: %d\n", optCode);
#endif
    if (optCode == 401 && !has_auth) {
        return false;
    }

    // PROPFIND to verify path and challenge response
    if (!send_request(base_url, HTTP_METHOD_PROPFIND, true, propCode)) {
#if DBG_WIFI_HOTSPOT
        Serial.println("[WIFI_HOTSPOT] WebDAV PROPFIND 请求失败");
#endif
        return false;
    }
#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] WebDAV PROPFIND 返回: %d\n", propCode);
#endif
    if (propCode == 401 && !has_auth) {
        return false;
    }

    if (!send_request(target, HTTP_METHOD_MKCOL, false, mkCode)) {
#if DBG_WIFI_HOTSPOT
        Serial.println("[WIFI_HOTSPOT] WebDAV MKCOL 请求失败");
#endif
        return false;
    }
#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] WebDAV MKCOL 返回: %d\n", mkCode);
#endif
    if (mkCode == 200 || mkCode == 201 || mkCode == 204 || mkCode == 405) {
        return true;
    }
    return false;
}

// 将前端发送的 rdt 和 png_base64 保存到 SD 卡的 /rdt 目录
void WiFiHotspotManager::handleUpdateDisplay() {
    // CORS headers
    webServer->sendHeader("Access-Control-Allow-Origin", "*");
    webServer->sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS, DELETE");
    webServer->sendHeader("Access-Control-Allow-Headers", "Content-Type, X-Requested-With");

    String body = webServer->arg("plain");
    if (body.length() == 0) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Empty body\"}");
        return;
    }

    // 手工从 body 中提取大字段，避免为完整 JSON 分配大量堆内存（导致 NoMemory）
    auto extractJsonString = [](const String &src, const char* key, String &out)->bool{
        String pattern = String("\"") + String(key) + String("\"");
        int idx = src.indexOf(pattern);
        if (idx < 0) return false;
        int col = src.indexOf(':', idx + pattern.length());
        if (col < 0) return false;
        int i = col + 1;
        // skip spaces
        while (i < src.length() && (src[i] == ' ' || src[i] == '\n' || src[i] == '\r' || src[i] == '\t')) i++;
        // expect starting quote
        if (i >= src.length() || src[i] != '"') return false;
        i++; // move past opening quote
        String acc;
        bool escape = false;
        for (; i < src.length(); i++) {
            char c = src[i];
            if (escape) {
                // handle common escapes
                if (c == '"') acc += '"';
                else if (c == '\\') acc += '\\';
                else if (c == '/') acc += '/';
                else if (c == 'b') acc += '\b';
                else if (c == 'f') acc += '\f';
                else if (c == 'n') acc += '\n';
                else if (c == 'r') acc += '\r';
                else if (c == 't') acc += '\t';
                else {
                    // unknown escape, copy as-is
                    acc += c;
                }
                escape = false;
            } else {
                if (c == '\\') {
                    escape = true;
                } else if (c == '"') {
                    out = acc;
                    return true;
                } else {
                    acc += c;
                }
            }
        }
        return false;
    };

    String rdtStr;
    String png_b64_str;

    if (!extractJsonString(body, "png_base64", png_b64_str)) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Missing png_base64\"}");
        return;
    }

    if (!extractJsonString(body, "rdt", rdtStr)) {
        // rdt 可以为空或缺失视为错误
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Missing rdt\"}");
        return;
    }

#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] Extracted rdt len=%d, png_b64 len=%d\n", rdtStr.length(), png_b64_str.length());
    if (png_b64_str.length() > 0) {
        Serial.printf("[WIFI_HOTSPOT] png_b64 first 60 chars: %.60s\n", png_b64_str.c_str());
    }
#endif

    // 清理 base64 字符串：移除所有空白和非 base64 字符
    String cleaned_b64;
    cleaned_b64.reserve(png_b64_str.length());
    for (int i = 0; i < png_b64_str.length(); i++) {
        char c = png_b64_str[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=') {
            cleaned_b64 += c;
        }
        // 忽略其他字符（空白、换行等）
    }
    png_b64_str = cleaned_b64;

#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] Cleaned png_b64 len=%d\n", png_b64_str.length());
#endif

    // 确保 /rdt 目录存在
    if (!SDW::SD.exists("/rdt")) {
        SDW::SD.mkdir("/rdt");
    }

    // RDT 将在处理完 PNG 后以原子方式保存（见函数后部）
    const char* rdt_path = "/rdt/readpaper.rdt";
    const char* rdt_tmp = "/rdt/readpaper.rdt.tmp";
    if (SDW::SD.exists(rdt_tmp)) SDW::SD.remove(rdt_tmp);

    // 解码 PNG base64 并写入文件
    const char* png_path = "/rdt/readpaper.png";
    const char* png_tmp = "/rdt/readpaper.png.tmp";
    if (SDW::SD.exists(png_tmp)) SDW::SD.remove(png_tmp);

    size_t b64len = (size_t)png_b64_str.length();
    // 防护：拒绝过大的上传（例如超过 1.5MB base64，约 1.1MB 二进制）
    const size_t MAX_B64 = 1500 * 1024; // 1.5MB
    if (b64len == 0 || b64len > MAX_B64) {
        webServer->send(413, "application/json", "{\"ok\":false,\"message\":\"png_base64 too large\"}");
        return;
    }
    size_t out_len = (b64len * 3) / 4 + 16;
    uint8_t* outbuf = (uint8_t*)malloc(out_len);
    if (!outbuf) {
        webServer->send(500, "application/json", "{\"ok\":false,\"message\":\"Out of memory\"}");
        return;
    }
    size_t dec_len = 0;
    int decode_ret = mbedtls_base64_decode(outbuf, out_len, &dec_len, (const unsigned char*)png_b64_str.c_str(), b64len);
    if (decode_ret != 0) {
        free(outbuf);
#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] Base64 decode failed: ret=%d, b64len=%d, out_len=%d\n", decode_ret, b64len, out_len);
#endif
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Base64 decode failed\"}");
        return;
    }

#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] Base64 decoded %d bytes -> %d bytes\n", b64len, dec_len);
#endif

    File pf = SDW::SD.open(png_tmp, "w");
    if (!pf) {
        free(outbuf);
        webServer->send(500, "application/json", "{\"ok\":false,\"message\":\"Failed to open png tmp\"}");
        return;
    }
    pf.write(outbuf, dec_len);
    pf.close();
    free(outbuf);

    if (SDW::SD.exists(png_path)) SDW::SD.remove(png_path);
    SDW::SD.rename(png_tmp, png_path);

    // 保存 rdt
    if (SDW::SD.exists(rdt_tmp)) SDW::SD.remove(rdt_tmp);
    File rf2 = SDW::SD.open(rdt_tmp, "w");
    if (!rf2) {
        webServer->send(500, "application/json", "{\"ok\":false,\"message\":\"Failed to open rdt tmp\"}");
        return;
    }
    rf2.print(rdtStr);
    rf2.close();
    if (SDW::SD.exists(rdt_path)) SDW::SD.remove(rdt_path);
    SDW::SD.rename(rdt_tmp, rdt_path);

    webServer->send(200, "application/json", "{\"ok\":true,\"message\":\"Saved\"}");
}

// 分块上传：开始会话，清空临时文件
void WiFiHotspotManager::handleUpdateDisplayStart() {
    webServer->sendHeader("Access-Control-Allow-Origin", "*");
    webServer->sendHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
    webServer->sendHeader("Access-Control-Allow-Headers", "Content-Type");

    String body = webServer->arg("plain");
    if (body.length() == 0) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Empty body\"}");
        return;
    }

    // 手工解析 type 字段
    int idx = body.indexOf("\"type\"");
    if (idx < 0) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Missing type\"}");
        return;
    }
    int col = body.indexOf(':', idx);
    if (col < 0) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Invalid json\"}");
        return;
    }
    int i = col + 1;
    while (i < body.length() && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' || body[i] == '\r')) i++;
    if (i >= body.length() || body[i] != '"') {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Invalid type format\"}");
        return;
    }
    i++;
    String type;
    while (i < body.length() && body[i] != '"') {
        type += body[i];
        i++;
    }

    if (!SDW::SD.exists("/rdt")) SDW::SD.mkdir("/rdt");

    const char* upload_path = nullptr;
    if (type == "rdt") {
        upload_path = "/rdt/readpaper.rdt.upload";
    } else if (type == "png") {
        upload_path = "/rdt/readpaper.png.upload";
    } else {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Invalid type\"}");
        return;
    }

    // 清空或创建上传临时文件
    if (SDW::SD.exists(upload_path)) SDW::SD.remove(upload_path);
    File uf = SDW::SD.open(upload_path, "w");
    if (!uf) {
        webServer->send(500, "application/json", "{\"ok\":false,\"message\":\"Failed to create upload file\"}");
        return;
    }
    uf.close();

#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] Upload started for type=%s, path=%s\n", type.c_str(), upload_path);
#endif

    webServer->send(200, "application/json", "{\"ok\":true}");
}

// 分块上传：接收一块数据
void WiFiHotspotManager::handleUpdateDisplayChunk() {
    webServer->sendHeader("Access-Control-Allow-Origin", "*");
    webServer->sendHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
    webServer->sendHeader("Access-Control-Allow-Headers", "Content-Type");

    String body = webServer->arg("plain");
    if (body.length() == 0) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Empty body\"}");
        return;
    }

    // 手工解析 type 和 data 字段（复用 extractJsonString lambda）
    auto extractJsonString = [](const String &src, const char* key, String &out)->bool{
        String pattern = String("\"") + String(key) + String("\"");
        int idx = src.indexOf(pattern);
        if (idx < 0) return false;
        int col = src.indexOf(':', idx + pattern.length());
        if (col < 0) return false;
        int i = col + 1;
        while (i < src.length() && (src[i] == ' ' || src[i] == '\n' || src[i] == '\r' || src[i] == '\t')) i++;
        if (i >= src.length() || src[i] != '"') return false;
        i++;
        String acc;
        bool escape = false;
        for (; i < src.length(); i++) {
            char c = src[i];
            if (escape) {
                if (c == '"') acc += '"';
                else if (c == '\\') acc += '\\';
                else if (c == '/') acc += '/';
                else if (c == 'b') acc += '\b';
                else if (c == 'f') acc += '\f';
                else if (c == 'n') acc += '\n';
                else if (c == 'r') acc += '\r';
                else if (c == 't') acc += '\t';
                else acc += c;
                escape = false;
            } else {
                if (c == '\\') escape = true;
                else if (c == '"') { out = acc; return true; }
                else acc += c;
            }
        }
        return false;
    };

    String type, data;
    if (!extractJsonString(body, "type", type)) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Missing type\"}");
        return;
    }
    if (!extractJsonString(body, "data", data)) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Missing data\"}");
        return;
    }

    const char* upload_path = nullptr;
    if (type == "rdt") {
        upload_path = "/rdt/readpaper.rdt.upload";
    } else if (type == "png") {
        upload_path = "/rdt/readpaper.png.upload";
    } else {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Invalid type\"}");
        return;
    }

    // 检查大小限制（单块最大 16KB）
    if (data.length() > 16384) {
        webServer->send(413, "application/json", "{\"ok\":false,\"message\":\"Chunk too large\"}");
        return;
    }

    File uf = SDW::SD.open(upload_path, "a");
    if (!uf) {
        webServer->send(500, "application/json", "{\"ok\":false,\"message\":\"Failed to open upload file\"}");
        return;
    }

    if (type == "rdt") {
        // RDT 直接追加文本
        uf.print(data);
        uf.close();
    } else if (type == "png") {
        // PNG 需要先 base64 解码再追加
        // 清理 base64 字符串
        String cleaned;
        cleaned.reserve(data.length());
        for (int j = 0; j < data.length(); j++) {
            char c = data[j];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=') {
                cleaned += c;
            }
        }

        size_t b64len = cleaned.length();
        size_t out_len = (b64len * 3) / 4 + 16;
        uint8_t* outbuf = (uint8_t*)malloc(out_len);
        if (!outbuf) {
            uf.close();
            webServer->send(500, "application/json", "{\"ok\":false,\"message\":\"Out of memory\"}");
            return;
        }

        size_t dec_len = 0;
        int ret = mbedtls_base64_decode(outbuf, out_len, &dec_len, (const unsigned char*)cleaned.c_str(), b64len);
        if (ret != 0) {
            free(outbuf);
            uf.close();
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] Chunk base64 decode failed: ret=%d\n", ret);
#endif
            webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Base64 decode failed\"}");
            return;
        }

#if DBG_WIFI_HOTSPOT
        Serial.printf("[WIFI_HOTSPOT] PNG chunk: b64_len=%d, decoded_len=%d\n", b64len, dec_len);
#endif

        size_t written = uf.write(outbuf, dec_len);
        if (written != dec_len) {
#if DBG_WIFI_HOTSPOT
            Serial.printf("[WIFI_HOTSPOT] Write incomplete! Expected=%d, Written=%d\n", dec_len, written);
#endif
            free(outbuf);
            uf.close();
            webServer->send(500, "application/json", "{\"ok\":false,\"message\":\"Write failed\"}");
            return;
        }
        
        uf.flush();  // 显式刷新缓冲区到 SD 卡
        uf.close();
        free(outbuf);
    }

#if DBG_WIFI_HOTSPOT
    Serial.printf("[WIFI_HOTSPOT] Chunk appended: type=%s, data_len=%d\n", type.c_str(), data.length());
#endif

    webServer->send(200, "application/json", "{\"ok\":true}");
}

// 分块上传：完成上传，重命名临时文件
void WiFiHotspotManager::handleUpdateDisplayCommit() {
    webServer->sendHeader("Access-Control-Allow-Origin", "*");
    webServer->sendHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
    webServer->sendHeader("Access-Control-Allow-Headers", "Content-Type");

    String body = webServer->arg("plain");
    if (body.length() == 0) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Empty body\"}");
        return;
    }

    // 手工解析 type 字段
    int idx = body.indexOf("\"type\"");
    if (idx < 0) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Missing type\"}");
        return;
    }
    int col = body.indexOf(':', idx);
    if (col < 0) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Invalid json\"}");
        return;
    }
    int i = col + 1;
    while (i < body.length() && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' || body[i] == '\r')) i++;
    if (i >= body.length() || body[i] != '"') {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Invalid type format\"}");
        return;
    }
    i++;
    String type;
    while (i < body.length() && body[i] != '"') {
        type += body[i];
        i++;
    }

    const char* upload_path = nullptr;
    const char* final_path = nullptr;
    if (type == "rdt") {
        upload_path = "/rdt/readpaper.rdt.upload";
        final_path = "/rdt/readpaper.rdt";
    } else if (type == "png") {
        upload_path = "/rdt/readpaper.png.upload";
        final_path = "/rdt/readpaper.png";
    } else {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Invalid type\"}");
        return;
    }

    // 检查上传文件是否存在
    if (!SDW::SD.exists(upload_path)) {
        webServer->send(400, "application/json", "{\"ok\":false,\"message\":\"Upload file not found\"}");
        return;
    }

    // 替换最终文件
    if (SDW::SD.exists(final_path)) SDW::SD.remove(final_path);
    if (!SDW::SD.rename(upload_path, final_path)) {
        webServer->send(500, "application/json", "{\"ok\":false,\"message\":\"Rename failed\"}");
        return;
    }
    // RDT 文件已更新，清空历史缓存以与新布局保持同步
    if (type == "rdt") {
        cache_clear_history();
    }

#if DBG_WIFI_HOTSPOT
    // 检查最终文件大小
    File check_file = SDW::SD.open(final_path, "r");
    if (check_file) {
        size_t file_size = check_file.size();
        check_file.close();
        Serial.printf("[WIFI_HOTSPOT] Upload committed: type=%s -> %s (size=%d bytes)\n", type.c_str(), final_path, file_size);
    } else {
        Serial.printf("[WIFI_HOTSPOT] Upload committed: type=%s -> %s (unable to check size)\n", type.c_str(), final_path);
    }
#endif

    webServer->send(200, "application/json", "{\"ok\":true,\"message\":\"Saved\"}");
}