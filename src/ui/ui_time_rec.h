#pragma once

#include <M5Unified.h>

// 绘制阅读时间记录界面
void draw_time_rec_screen(M5Canvas* canvas);

// 判断给定坐标是否在返回按钮区域
bool is_point_in_time_rec_back_button(int16_t x, int16_t y);
