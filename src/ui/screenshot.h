#pragma once

#include <M5Unified.h>

/**
 * @brief 截取当前 g_canvas 的内容并保存为 PNG 图片到 SD 卡
 * 
 * 图片会保存到 /screenshot 目录，命名格式为 readpaper_screen_YYYY_MM_DD_HH_MM_SS.png
 * 图片会按照真实灰度输出（16级灰度）
 * 
 * @return true 截图成功
 * @return false 截图失败
 */
bool screenShot();

/**
 * @brief 确保SD卡上存在 /screenshot 文件夹
 * 
 * @return true 文件夹存在或创建成功
 * @return false 创建失败
 */
bool ensureScreenshotFolder();

/**
 * @brief 检查触摸点是否在截图区域内（230,0）到（310,80）
 * 
 * @param x 触摸点X坐标
 * @param y 触摸点Y坐标
 * @return true 在截图区域内
 * @return false 不在截图区域内
 */
inline bool isInScreenshotArea(int16_t x, int16_t y)
{
    return (x >= 230 && x <= 310 && y >= 0 && y <= 80);
}
