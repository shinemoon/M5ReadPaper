#pragma once
#include <M5Unified.h>

// 将图片推送到Canvas指定位置，不立即刷新
// canvas: 目标画布，如果为nullptr则使用全局g_canvas
// x,y: 当使用自定义canvas时为附加偏移，否则为绝对位置
void ui_push_image_to_canvas(const char* img_path, int16_t x, int16_t y, M5Canvas* canvas = nullptr, bool preClean=false);

// 将图片按比例缩放后推送到Canvas指定位置
// scale_x, scale_y: 缩放比例，1.0为原始大小
// 例如 scale_x=1.0/3.0, scale_y=1.0/3.0 将图片缩小到1/3
void ui_push_image_to_canvas_scaled(const char* img_path, int16_t x, int16_t y, float scale_x, float scale_y, M5Canvas* canvas = nullptr, bool preClean=false);

// 直接在display上推送图片，计算真实可见区域，实现最快显示
void ui_push_image_to_display_direct(const char* img_path, int16_t x, int16_t y, bool preClean = false);