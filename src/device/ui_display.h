#pragma once
#include <M5Unified.h>
#include "readpaper.h"
#include "../text/bin_font_print.h"

// 封装显示函数
void display_print(const char* text, float text_size = SYSFONTSIZE, uint16_t color = TFT_BLACK, uint8_t alignment = TL_DATUM, 
                     int16_t margin_top = 30, int16_t margin_bottom = 30, 
                     int16_t margin_left = 20, int16_t margin_right = 20,
                     uint16_t bgcolor = WHITE, bool fastmode = true, bool dark=false);

/**
 * @brief 带自动换行的文本打印函数
 * 
 * @param text 要打印的文本
 * @param x 起始 x 坐标
 * @param y 起始 y 坐标
 * @param area_width 可用宽度/高度（根据 vertical 参数决定）
 * @param area_height 可用高度（0 表示不限制高度）
 * @param font_size 字体大小（0 表示使用当前字体的默认大小）
 * @param color 文本颜色（0-15 灰度级别）
 * @param bg_color 背景颜色（"transparent" 或 0-15 灰度级别，-1 表示透明）
 * @param align 对齐方式（0=左对齐，1=居中，2=右对齐）
 * @param vertical 是否垂直排版
 * @param skip 是否跳过繁简转换
 */
void display_print_wrapped(const char* text, int16_t x, int16_t y, int16_t area_width,
                          int16_t area_height = 0, uint8_t font_size = 24, uint8_t color = 0, 
                          int16_t bg_color = -1, uint8_t align = 0, bool vertical = false, bool skip = false);

//Debug, just one warpper for display
void initDisplay();
void fontLoad();
// Wrapper to set display rotation while handling EPD power-save around the call.
// Ensures M5.Display.powerSaveOff() is called before setRotation and
// M5.Display.powerSaveOn() is called after to avoid partial-update issues.
void display_set_rotation(int rotation);