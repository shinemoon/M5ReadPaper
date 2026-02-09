#pragma once

#include <WiFi.h>
#include <WebServer.h>
#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include <M5Unified.h>
#include <nvs_flash.h>

/**
 * @brief WiFi热点和Web服务器管理器
 * 
 * 提供WiFi热点功能和Web文件管理服务器
 * 用于在WIRE CONNECT状态下进行文件上传和管理
 */
class WiFiHotspotManager {
public:
    /**
     * @brief 构造函数
     */
    WiFiHotspotManager();

    /**
     * @brief 析构函数
     */
    ~WiFiHotspotManager();

    /**
     * @brief 启动WiFi热点和Web服务器
     * @param ssid 热点名称，如果为nullptr则使用默认名称
     * @param password 热点密码，如果为nullptr则使用默认密码
     * @return true 启动成功，false 启动失败
     */
    bool start(const char* ssid = nullptr, const char* password = nullptr);

    /**
     * @brief 停止WiFi热点和Web服务器
     */
    void stop();

    /**
     * @brief 检查热点是否正在运行
     * @return true 运行中，false 未运行
     */
    bool isRunning() const;

    /**
     * @brief 处理Web服务器请求（需要在主循环中定期调用）
     */
    void handleClient();

    /**
     * @brief 获取热点SSID
     * @return SSID字符串
     */
    const char* getSSID() const;

    /**
     * @brief 获取热点密码
     * @return 密码字符串
     */
    const char* getPassword() const;

    /**
     * @brief 获取热点IP地址
     * @return IP地址字符串
     */
    String getIPAddress() const;

    /**
     * @brief 获取连接的客户端数量
     * @return 客户端数量
     */
    int getConnectedClients() const;

    /**
     * @brief 检查是否正在上传文件
     * @return true 正在上传，false 未在上传
     */
    bool isUploadInProgress() const;

    /**
     * @brief 从token.json文件读取WiFi AP配置并尝试连接
     * @return true 连接成功，false 连接失败或配置文件不存在
     */
    bool connectToWiFiFromToken();

    /**
     * @brief 断开WiFi STA连接
     */
    void disconnectWiFi();

    // 允许 API 路由器访问私有处理函数进行路由绑定
    friend class ApiRouter;

private:
    static constexpr const char* DEFAULT_SSID = "ReaderPaper-AP";
    static constexpr const char* DEFAULT_PASSWORD = "readpaper123";
    static constexpr int DEFAULT_CHANNEL = 1;
    static constexpr int MAX_CONNECTIONS = 4;
    
    WebServer* webServer;
    bool running;
    String currentSSID;
    String currentPassword;
    volatile bool uploadInProgress; // 上传状态标志

    // Web服务器处理函数
    void handleRoot();
    void handleFileList(String category);
    void handleFileUpload();
    void handleFileDelete();
    void handleFileDownload();
    void handleNotFound();
    void handleFileUploadPost();
    void handleReadingRecords();

    // 辅助函数
    String formatFileSize(size_t bytes);
    String getContentType(String filename);
    bool handleFileRead(String path);
    void sendDirectoryList(String path);
    
    // HTML页面生成
    String generateWebPage();
    String generateUploadForm();
    String generateFileListHTML(String dirPath);
    String generateFallbackTemplate(); // 备用轻量级模板
};

// 全局WiFi热点管理器实例
extern WiFiHotspotManager* g_wifi_hotspot;

/**
 * @brief 初始化WiFi热点管理器
 */
void wifi_hotspot_init();

/**
 * @brief 清理WiFi热点管理器
 */
void wifi_hotspot_cleanup();