// Global symbols used across the project
#pragma once

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

// Wakeup cause from esp_sleep (global for access across modules)
#include <esp_sleep.h>
extern esp_sleep_wakeup_cause_t g_wake_cause;

// 默认唤醒周期（分钟），可被其他模块读取/修改
extern int refreshPeriod;
