#pragma once

#include <M5Unified.h>

// 在 canvas 上绘制快速阅读菜单（底部 540x200 白色矩形）
void draw_reading_quick_menu(M5Canvas* canvas, uint8_t preview_font_scale_pct, bool has_pending_scale_change);

// 判断给定坐标是否在快速菜单矩形区域内
bool is_point_in_reading_quick_menu(int16_t x, int16_t y);
