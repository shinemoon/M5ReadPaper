#pragma once

#include <M5Unified.h>

/**
 * @brief 显示 TRMNL 界面（主入口函数）
 * 
 * 尝试从 WebDAV /readpaper/readpaper.rdt 读取配置并显示
 * 如果失败则显示默认的 WiFi/WebDAV 信息界面
 * 
 * @param canvas 画布指针，如果为 nullptr 则使用全局画布
 * @return true 显示成功，false 显示失败
 */
bool trmnl_display(M5Canvas *canvas = nullptr);

/**
 * @brief 显示默认的 WEBDAV TRMNL 风格界面
 * 
 * 显示 WiFi 连接信息、WebDAV 访问地址等信息
 * 使用类似终端显示器(TRMNL)的风格
 * 
 * @param canvas 画布指针，如果为 nullptr 则使用全局画布
 * @return true 显示成功，false 显示失败
 */
bool show_default_trmnl(M5Canvas *canvas = nullptr);
