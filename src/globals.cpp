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

static int16_t get_reading_total_horizontal_margin()
{
	uint8_t base_font_size = get_font_size_from_file();
	if (base_font_size == 0)
	{
		base_font_size = SYSFONTSIZE;
	}

	float configured_font_size = get_configured_reading_font_size(base_font_size);
	float target_total_margin = configured_font_size * FONT_SCALE_MARGIN_COMPENSATION_STRENGTH;
	int16_t total_margin = (int16_t)lroundf(target_total_margin);

	if (total_margin < 2)
	{
		total_margin = 2;
	}

	if (total_margin > PAPER_S3_WIDTH - 2)
	{
		total_margin = PAPER_S3_WIDTH - 2;
	}

	// 为了保证左右边距严格相等，将总边距调整为偶数。
	if ((total_margin & 0x1) != 0)
	{
		total_margin += 1;
	}

	return total_margin;
}

int16_t get_reading_effective_margin_left()
{
	return get_reading_total_horizontal_margin() / 2;
}

int16_t get_reading_effective_margin_right()
{
	return get_reading_total_horizontal_margin() / 2;
}
