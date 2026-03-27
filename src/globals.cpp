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

uint8_t clamp_font_render_tradeoff_level(int level)
{
	if (level < FONT_RENDER_TRADEOFF_FAST)
	{
		return FONT_RENDER_TRADEOFF_FAST;
	}
	if (level > FONT_RENDER_TRADEOFF_QUALITY)
	{
		return FONT_RENDER_TRADEOFF_QUALITY;
	}
	return (uint8_t)level;
}

uint8_t get_font_render_tradeoff_level()
{
	return clamp_font_render_tradeoff_level(g_config.font_render_tradeoff_level);
}

float get_font_scale_margin_compensation_strength()
{
	switch (get_font_render_tradeoff_level())
	{
	case FONT_RENDER_TRADEOFF_FAST:
		return 0.60f;
	case FONT_RENDER_TRADEOFF_BALANCED:
		return 0.90f;
	case FONT_RENDER_TRADEOFF_QUALITY:
	default:
		return FONT_SCALE_MARGIN_COMPENSATION_STRENGTH;
	}
}

float get_font_shift_damp_start_ratio()
{
	switch (get_font_render_tradeoff_level())
	{
	case FONT_RENDER_TRADEOFF_FAST:
		return 1.60f;
	case FONT_RENDER_TRADEOFF_BALANCED:
		return 1.36f;
	case FONT_RENDER_TRADEOFF_QUALITY:
	default:
		return 1.30f;
	}
}

float get_font_shift_damp_slope()
{
	switch (get_font_render_tradeoff_level())
	{
	case FONT_RENDER_TRADEOFF_FAST:
		return 0.45f;
	case FONT_RENDER_TRADEOFF_BALANCED:
		return 0.72f;
	case FONT_RENDER_TRADEOFF_QUALITY:
	default:
		return 0.85f;
	}
}

float get_font_wrap_damp_start_ratio()
{
	switch (get_font_render_tradeoff_level())
	{
	case FONT_RENDER_TRADEOFF_FAST:
		return 1.55f;
	case FONT_RENDER_TRADEOFF_BALANCED:
		return 1.32f;
	case FONT_RENDER_TRADEOFF_QUALITY:
	default:
		return 1.25f;
	}
}

float get_font_wrap_damp_slope()
{
	switch (get_font_render_tradeoff_level())
	{
	case FONT_RENDER_TRADEOFF_FAST:
		return 0.22f;
	case FONT_RENDER_TRADEOFF_BALANCED:
		return 0.36f;
	case FONT_RENDER_TRADEOFF_QUALITY:
	default:
		return 0.45f;
	}
}

bool should_prefetch_far_pages()
{
	return get_font_render_tradeoff_level() != FONT_RENDER_TRADEOFF_FAST;
}

uint32_t get_middle_refresh_threshold()
{
	switch (get_font_render_tradeoff_level())
	{
	case FONT_RENDER_TRADEOFF_FAST:
		return 16;
	case FONT_RENDER_TRADEOFF_BALANCED:
		return 10;
	case FONT_RENDER_TRADEOFF_QUALITY:
	default:
		return FIRST_REFRESH_TH;
	}
}

uint32_t get_quality_refresh_threshold()
{
	switch (get_font_render_tradeoff_level())
	{
	case FONT_RENDER_TRADEOFF_FAST:
		return 40;
	case FONT_RENDER_TRADEOFF_BALANCED:
		return 22;
	case FONT_RENDER_TRADEOFF_QUALITY:
	default:
		return SECOND_REFRESH_TH;
	}
}

uint32_t get_full_refresh_threshold_normal()
{
	switch (get_font_render_tradeoff_level())
	{
	case FONT_RENDER_TRADEOFF_FAST:
		return 40;
	case FONT_RENDER_TRADEOFF_BALANCED:
		return 28;
	case FONT_RENDER_TRADEOFF_QUALITY:
	default:
		return FULL_REFRESH_TH;
	}
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
	float target_total_margin = configured_font_size * get_font_scale_margin_compensation_strength();
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
