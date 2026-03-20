#include "globals.h"
#include "text/bin_font_print.h"

extern GlobalConfig g_config;

// Default orientation unknown
volatile int g_device_orientation = ORIENT_UNKNOWN;

// Existing globals
volatile bool g_disable_sd_access = false;
// Auto-read flag: default false
bool autoread = false;
// Auto-read speed default
uint8_t autospeed = 2;

// 字体加载位置: 0=缓存到内存, 1=按需从文件读取
int8_t fontLoadLoc = 1;

// WiFi STA连接状态，默认未连接
bool g_wifi_sta_connected = false;

// HTTP 活动期间为 true，阻止主循环同时做 SDMMC DMA
volatile bool g_wifi_http_active = false;

// 默认唤醒周期（分钟）
int refreshPeriod = 30;

uint8_t clamp_font_scale_pct(int scale_pct)
{
	if (scale_pct < FONT_SCALE_MIN_PCT)
	{
		return FONT_SCALE_MIN_PCT;
	}
	if (scale_pct > FONT_SCALE_MAX_PCT)
	{
		return FONT_SCALE_MAX_PCT;
	}
	return (uint8_t)scale_pct;
}

float get_configured_reading_font_size(uint8_t base_font_size)
{
	uint8_t resolved_base_font_size = base_font_size > 0 ? base_font_size : SYSFONTSIZE;
	uint8_t scale_pct = clamp_font_scale_pct(g_config.font_scale_pct);
	return ((float)resolved_base_font_size * (float)scale_pct) / 100.0f;
}

// 按字体缩放动态修正右边距。
// 使用“相对字体尺寸”的补偿，而不是固定像素：
// 以基础字体的大致半角字宽（约 base_font_size * 0.5）为参照，
// 再乘以缩小比例 ((100 - scale) / 100)。
// 这样字体文件基础尺寸变化时，边距补偿也会同步变化。
int16_t get_reading_effective_margin_right()
{
	uint8_t scale = clamp_font_scale_pct((int)g_config.font_scale_pct);
	if (scale == 100)
	{
		return MARGIN_RIGHT;
	}

	int16_t extra = 0;
	if (scale < 100)
	{
		uint8_t base_font_size = get_font_size_from_file();
		if (base_font_size == 0)
		{
			base_font_size = SYSFONTSIZE;
		}

		float shrink_ratio = (100.0f - (float)scale) / 100.0f;
		float reference_half_char_width = (float)base_font_size * 0.5f;
		float compensation = reference_half_char_width * shrink_ratio;
		compensation *= FONT_SCALE_MARGIN_COMPENSATION_STRENGTH;
		extra = (int16_t)lroundf(compensation);
	}
	else
	{
		// 大字号不额外增加右边距，避免可用宽度变窄。
		extra = 0;
	}

	return MARGIN_RIGHT + extra;
}
