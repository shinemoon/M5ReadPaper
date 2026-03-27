// Global symbols used across the project
#pragma once

#include "readpaper.h"

// Use the compatibility wrapper which exposes `g_current_book` as a macro
#include "current_book.h"

// When true, tasks and helpers must avoid any SD access (used when switching to USB MSC)
extern volatile bool g_disable_sd_access;

// Device orientation enum and global variable (四方向)
enum DeviceOrientation
{
	ORIENT_UNKNOWN = 0,
	ORIENT_UP = 1,     // 设备顶部朝上（竖屏）
	ORIENT_DOWN = 2,   // 设备顶部朝下（倒竖）
	ORIENT_LEFT = 3,   // 屏幕向左（横向）
	ORIENT_RIGHT = 4,  // 屏幕向右（横向）
};

extern volatile int g_device_orientation; // holds DeviceOrientation

// 小工具: 将 orientation 转为可打印字符串
static inline const char* DeviceOrientationToString(int d)
{
	switch (d)
	{
	case ORIENT_UP:
		return "UP";
	case ORIENT_DOWN:
		return "DOWN";
	case ORIENT_LEFT:
		return "LEFT";
	case ORIENT_RIGHT:
		return "RIGHT";
	default:
		return "UNKNOWN";
	}
}

// 当为 true 时，打开书籍后会尝试自动进入阅读/跳转行为
// 全局默认值在 `globals.cpp` 中初始化为 false
extern bool autoread;

// 自动翻页速度（1..255），单位为秒的倍数/自定义含义由界面使用者决定
// 默认初始值为 3
#include <stdint.h>
extern uint8_t autospeed;

// WiFi STA连接状态标志
// true = 已连接到WiFi AP，false = 未连接
extern bool g_wifi_sta_connected;

// WiFi HTTP 活动标志：当任一 HTTP/HTTPS 请求正在进行时为 true
// 主循环在此期间跳过 SDMMC 后台索引，避免两路 DMA 并发冲突
extern volatile bool g_wifi_http_active;

// Wakeup cause from esp_sleep (global for access across modules)
#include <esp_sleep.h>
extern esp_sleep_wakeup_cause_t g_wake_cause;

// 默认唤醒周期（分钟），可被其他模块读取/修改
extern int refreshPeriod;

uint8_t clamp_font_scale_pct(int scale_pct);
uint8_t clamp_font_render_tradeoff_level(int level);
uint8_t get_font_render_tradeoff_level();
float get_configured_reading_font_size(uint8_t base_font_size);
float get_font_scale_margin_compensation_strength();
float get_font_shift_damp_start_ratio();
float get_font_shift_damp_slope();
float get_font_wrap_damp_start_ratio();
float get_font_wrap_damp_slope();
bool should_prefetch_far_pages();
uint32_t get_middle_refresh_threshold();
uint32_t get_quality_refresh_threshold();
uint32_t get_full_refresh_threshold_normal();
// 根据当前字体缩放比例计算有效左边距，和右边距保持一致。
int16_t get_reading_effective_margin_left();
// 根据当前字体缩放比例计算有效右边距：小字号时适当增加右边距以保持视觉平衡
int16_t get_reading_effective_margin_right();
