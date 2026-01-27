#pragma once

#include <cstdint>
#include <string>

// 全局版本字符串（从/version文件读取）
extern std::string ver;

/**
 * 显示启动屏幕
 */
void show_start_screen(const char *subtitle = nullptr);

/**
 * 显示锁屏界面
 * @param area_width 显示区域宽度
 * @param area_height 显示区域高度  
 * @param font_size 字体大小
 */
// show_lockscreen: display lock screen with optional text
// text: message to display on lock screen (default: "坐看云起时")
void show_lockscreen(int16_t area_width, int16_t area_height, float font_size, const char *text = "坐看云起时", bool isshutdown=false, const char* labelpos = "bottom");

// 清理锁屏随机图片的候选缓存（例如 SD 卡内容变更时调用）
void lockscreen_image_cache_invalidate();
