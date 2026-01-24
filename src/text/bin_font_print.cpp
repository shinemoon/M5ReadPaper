#include <FS.h>
#include <SPIFFS.h>
#include "SD/SDWrapper.h"
#include <stdint.h>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <M5Unified.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "bin_font_print.h"
#include "readpaper.h"
#include "papers3.h"
#include "device/memory_pool.h"
#include "device/chunked_font_cache.h"
#include "text/font_decoder.h"
#include "text/font_color_mapper.h"
#include "text/progmem_font_data.h"
#include "test/per_file_debug.h"
#include "tasks/display_push_task.h"
#include "device/file_manager.h"
#include "../text/zh_conv.h"
// current book access for cache prefetch
#include "current_book.h"
// access per-book bookmark config
#include "../text/book_handle.h"
#include "text/font_buffer.h"

extern GlobalConfig g_config;
extern int8_t fontLoadLoc;
extern FontBufferManager g_font_buffer_manager;

static bool g_font_stream_mode = false;
bool g_using_progmem_font = false; // 当前是否使用PROGMEM内置字体（全局可见）

// 全局字体文件访问互斥锁，防止索引任务和UI渲染并发访问导致seek位置混乱
static SemaphoreHandle_t g_font_file_mutex = nullptr;

// PSRAM 缓存：仅存储字体头文件数据（134字节）
// 索引表会被解析到内存中的vector/map，不需要缓存原始字节
struct FontHeaderCache
{
    uint8_t *header_data; // 头文件数据（134字节）
    size_t header_size;   // 头文件大小
    bool is_cached;       // 是否已缓存

    FontHeaderCache() : header_data(nullptr), header_size(0), is_cached(false) {}

    ~FontHeaderCache()
    {
        cleanup();
    }

    void cleanup()
    {
        if (header_data)
        {
            heap_caps_free(header_data);
            header_data = nullptr;
        }
        header_size = 0;
        is_cached = false;
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT_CACHE] PSRAM 头文件缓存已清理\n");
#endif
    }

    bool allocate(size_t header_sz)
    {
        cleanup();

        // 仅分配头文件缓存（134字节）
        header_data = (uint8_t *)heap_caps_malloc(header_sz, MALLOC_CAP_SPIRAM);
        if (!header_data)
        {
#if DBG_BIN_FONT_PRINT
            Serial.printf("[FONT_CACHE] ❌ 无法分配头文件缓存 %u 字节\n", header_sz);
#endif
            return false;
        }
        header_size = header_sz;
        is_cached = true;

#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT_CACHE] ✅ PSRAM 头文件缓存分配成功: %u 字节\n", header_sz);
#endif
        return true;
    }
};

static FontHeaderCache g_font_header_cache;

// ============ 字形预读取窗口 - 优化SD卡读取性能 ============
// 为了减少SD卡seek次数，在渲染字形时预读取一个窗口的数据到PSRAM
struct GlyphReadWindow
{
    uint8_t *buffer;                                  // 预读缓冲区（PSRAM分配）
    uint32_t window_offset;                           // 窗口在文件中的起始偏移
    size_t window_size;                               // 窗口中实际有效的数据大小（≤buffer_capacity）
    static constexpr size_t BUFFER_SIZE = 256 * 1024; // 256KB 预读缓冲（可调整）

    GlyphReadWindow() : buffer(nullptr), window_offset(0), window_size(0) {}

    ~GlyphReadWindow()
    {
        cleanup();
    }

    bool allocate()
    {
        if (buffer)
            return true; // 已分配

        buffer = (uint8_t *)heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (!buffer)
        {
#if DBG_BIN_FONT_PRINT
            Serial.printf("[GLYPH_WINDOW] ⚠️  无法分配字形预读窗口 %u 字节\n", BUFFER_SIZE);
#endif
            return false;
        }
#if DBG_BIN_FONT_PRINT
        Serial.printf("[GLYPH_WINDOW] ✅ 字形预读窗口分配成功: %u KB\n", BUFFER_SIZE / 1024);
#endif
        return true;
    }

    void cleanup()
    {
        if (buffer)
        {
            heap_caps_free(buffer);
            buffer = nullptr;
        }
        window_offset = 0;
        window_size = 0;
    }

    // 检查数据是否在当前窗口内
    bool is_in_window(uint32_t offset, uint32_t size) const
    {
        if (!buffer || window_size == 0)
        {
            return false;
        }
        // 严格检查：必须完全在窗口内
        // offset 必须 >= window_offset
        // offset + size 必须 <= window_offset + window_size
        if (offset < window_offset)
        {
            return false;
        }
        if ((offset + size) > (window_offset + window_size))
        {
            return false;
        }
        return true;
    }

    // 从缓冲区读取数据（前提：必须先检查 is_in_window）
    bool read_from_window(uint32_t offset, uint8_t *out_buffer, uint32_t size) const
    {
        // 第一层验证：窗口有效性
        if (!buffer || window_size == 0)
        {
            return false;
        }

        // 第二层验证：offset必须在窗口范围内
        if (offset < window_offset)
        {
            return false;
        }

        // 第三层验证：数据必须完全在窗口内
        if ((offset + size) > (window_offset + window_size))
        {
            return false;
        }

        // 第四层验证：输出缓冲区有效
        if (!out_buffer)
        {
            return false;
        }

        // 计算相对偏移
        uint32_t relative_offset = offset - window_offset;

        // 第五层验证：相对偏移不能超出窗口大小
        if (relative_offset >= window_size)
        {
            return false;
        }

        // 第六层验证：读取范围不能超出窗口
        if ((relative_offset + size) > window_size)
        {
            return false;
        }

        // 所有验证通过，执行复制
#if DBG_BIN_FONT_PRINT
        // 调试模式：验证即将复制的数据
        static int copy_count = 0;
        if (copy_count < 5)
        {
            Serial.printf("[WINDOW_COPY] #%d: offset=%u size=%u relative_offset=%u window=[%u,%u) buffer=%p out=%p\n",
                          copy_count++, offset, size, relative_offset, window_offset, window_offset + window_size,
                          buffer, out_buffer);
            Serial.printf("[WINDOW_COPY] 将复制的前4字节: %02X %02X %02X %02X\n",
                          buffer[relative_offset], buffer[relative_offset + 1],
                          buffer[relative_offset + 2], buffer[relative_offset + 3]);
        }
#endif
        memcpy(out_buffer, buffer + relative_offset, size);
#if DBG_BIN_FONT_PRINT
        if (copy_count <= 5)
        {
            Serial.printf("[WINDOW_COPY] 已复制，out_buffer前4字节: %02X %02X %02X %02X\n",
                          out_buffer[0], out_buffer[1], out_buffer[2], out_buffer[3]);
        }
#endif
        return true;
    }

    // 重新定位窗口：读取从 new_offset 开始的数据到缓冲区
    // ✨ 关键改进：使用临时变量，成功后再原子性更新，避免中间状态
    bool reposition_window(File &fontFile, uint32_t new_offset)
    {
        if (!buffer)
        {
            return false;
        }

        // 验证文件可用性
        if (!fontFile || !fontFile.available())
        {
            return false;
        }

        // ⚠️ 关键：不要清零当前窗口！保持旧窗口有效，直到新窗口读取成功
        // 这样即使重定位失败，旧窗口仍然可用

        esp_task_wdt_reset();

        // 执行seek
        if (!fontFile.seek(new_offset))
        {
#if DBG_BIN_FONT_PRINT
            Serial.printf("[GLYPH_WINDOW] ❌ seek失败 offset=%u，保持旧窗口\n", new_offset);
#endif
            return false;
        }

        esp_task_wdt_reset();

        // 执行read（直接读到buffer中）
        size_t read_size = fontFile.read(buffer, BUFFER_SIZE);

        esp_task_wdt_reset();

        // 验证读取结果
        if (read_size == 0)
        {
#if DBG_BIN_FONT_PRINT
            Serial.printf("[GLYPH_WINDOW] ❌ read返回0字节 offset=%u\n", new_offset);
#endif
            // 读取失败时，窗口被破坏了，必须清零以防止使用错误数据
            window_size = 0;
            window_offset = 0;
            return false;
        }

        // ⚠️ 关键检查：如果读取size小于BUFFER_SIZE，可能是文件末尾或读取失败
#if DBG_BIN_FONT_PRINT
        if (read_size < BUFFER_SIZE)
        {
            Serial.printf("[GLYPH_WINDOW] ⚠️  读取不完整 期望=%u 实际=%zu (可能是文件末尾)\n",
                          BUFFER_SIZE, read_size);
        }
#endif

        // ✅ 读取成功，原子性更新窗口状态
        window_offset = new_offset;
        window_size = read_size;

#if DBG_BIN_FONT_PRINT
        Serial.printf("[GLYPH_WINDOW] ✅ 重定位成功 offset=%u size=%zu (%.1f KB)\n",
                      window_offset, window_size, window_size / 1024.0f);
#endif
        return true;
    }
};

static GlyphReadWindow g_glyph_read_window;

// 检测是否为需要旋转的中文标点符号
static bool is_chinese_punctuation(uint32_t unicode)
{
    return (
        // unicode == 0x3001 || unicode == 0x3002 ||  // 、。
        //            unicode == 0xFF0C || unicode == 0xFF0E ||  // ，。
        unicode == 0xFF1A || unicode == 0xFF1B ||   // ：；
                                                    //            unicode == 0xFF1F || unicode == 0xFF01 ||  // ？！
        unicode == 0x300C || unicode == 0x300D ||   // 「」
        unicode == 0x300E || unicode == 0x300F ||   // 『』
        unicode == 0xFF08 || unicode == 0xFF09 ||   // （）
        unicode == 0x3010 || unicode == 0x3011 ||   // 【】
        unicode == 0x2018 || unicode == 0x2019 ||   // ''
        unicode == 0x201C || unicode == 0x201D ||   // ""
        unicode == 0x3008 || unicode == 0x3009 ||   // 〈〉
        unicode == 0x300A || unicode == 0x300B ||   // 《》
        unicode == 0x003C || unicode == 0x003E ||   // <>
        unicode == 0xFF1C || unicode == 0xFF1E ||   // ＜＞
        unicode == 0x2026 || unicode == 0x22EF ||   // …⋯
        unicode == 0x2025 || unicode == 0xFE19 ||   // ‥︙(两点/六点省略号)
        unicode == 0x005B || unicode == 0x005D ||   // []
        unicode == 0x0028 || unicode == 0x0029 ||   // ()
        unicode == 0x0024 || unicode == 0x0040 ||   // $@
        unicode == 0x002D || unicode == 0x005F ||   // -_
        unicode == 0x2014 || unicode == 0x003D ||   // —=
        unicode == 0x007E ||                        // ~
        (unicode >= 0x0030 && unicode <= 0x0039) || // 0-9
        (unicode >= 0x0041 && unicode <= 0x005A) || // A-Z
        (unicode >= 0x0061 && unicode <= 0x007A) || // a-z
        (unicode >= 0x00C0 && unicode <= 0x00FF) || // Latin-1 Supplement (À-ÿ)
        (unicode >= 0x0100 && unicode <= 0x017F) || // Latin Extended-A (Ā-ſ)
        (unicode >= 0x0180 && unicode <= 0x024F));  // Latin Extended-B (ƀ-ɏ)
}

// 检测是否需要镜像的括号
// 我自己搞错了 不需要flip
static bool needs_horizontal_flip(uint32_t unicode)
{
    return false;
    //    return (unicode == 0x300C || unicode == 0x300D ||  // 「」
    //            unicode == 0x300E || unicode == 0x300F);   // 『』
}

static bool needs_minor_shift(uint32_t unicode)
{
    switch (unicode)
    {
    case 0x3001: // 、
    case 0x3002: // 。
    case 0xFF0C: // ，
    case 0xFF0E: // ．
    case 0xFF01: // ！
    case 0xFF1F: // ？
    case 0xFF61: // ｡
    case 0xFF64: // ､
    case 0x002C: // ,
    case 0x002E: // .
    case 0x0021: // !
    case 0x003F: // ?
        return true;
    default:
        break;
    }
    return false;
}

// 90度顺时针旋转位图
static void rotate_bitmap_90_cw(const uint16_t *src, uint16_t *dst, int16_t src_w, int16_t src_h)
{
    // 旋转后: dst[y][x] = src[src_h-1-x][y]
    // 线性索引: dst[x * src_h + (src_h-1-y)] = src[y * src_w + x]
    for (int16_t y = 0; y < src_h; y++)
    {
        for (int16_t x = 0; x < src_w; x++)
        {
            int16_t dst_x = src_h - 1 - y;
            int16_t dst_y = x;
            dst[dst_y * src_h + dst_x] = src[y * src_w + x];
        }
    }
}

// 水平镜像位图（左右翻转）
static void flip_bitmap_horizontal(uint16_t *bitmap, int16_t w, int16_t h)
{
    for (int16_t y = 0; y < h; y++)
    {
        for (int16_t x = 0; x < w / 2; x++)
        {
            int16_t left_idx = y * w + x;
            int16_t right_idx = y * w + (w - 1 - x);
            uint16_t temp = bitmap[left_idx];
            bitmap[left_idx] = bitmap[right_idx];
            bitmap[right_idx] = temp;
        }
    }
}

/**
 * V3 字体灰度感知缩放渲染
 * 使用区域采样和加权平均来保持抗锯齿效果
 *
 * @param canvas 目标画布
 * @param bitmap 源位图(RGB565格式，已根据dark_mode映射好颜色)
 * @param orig_w 原始宽度
 * @param orig_h 原始高度
 * @param scaled_w 缩放后宽度
 * @param scaled_h 缩放后高度
 * @param canvas_x 画布X坐标
 * @param canvas_y 画布Y坐标
 * @param scale_factor 缩放因子
 * @param dark_mode 暗黑模式
 */
static void render_v3_scaled(M5Canvas *canvas, uint16_t *bitmap,
                             int16_t orig_w, int16_t orig_h,
                             int16_t scaled_w, int16_t scaled_h,
                             int16_t canvas_x, int16_t canvas_y,
                             float scale_factor, bool dark_mode)
{
    if (!canvas || !bitmap)
        return;

    // 注意：bitmap 中的颜色已经根据 dark_mode 映射好了
    // 正常模式: fg=0x0000(黑), gray=GREY_MAP_COLOR, bg=0xFFFF(白)
    // Dark模式: fg=0xFFFF(白), gray=GREY_LEVEL_DARK, bg=0x0000(黑)
    uint16_t bg_color = FontColorMapper::get_background_color(dark_mode);
    uint16_t fg_color = dark_mode ? 0xFFFF : 0x0000;
    uint16_t gray_out = dark_mode ? GREY_LEVEL_MID : GREY_MAP_COLOR;

    // 遍历缩放后的每个像素
    for (int16_t sy = 0; sy < scaled_h; sy++)
    {
        for (int16_t sx = 0; sx < scaled_w; sx++)
        {
            // 计算原图对应区域的浮点坐标范围
            float orig_x_start = sx / scale_factor;
            float orig_y_start = sy / scale_factor;
            float orig_x_end = (sx + 1) / scale_factor;
            float orig_y_end = (sy + 1) / scale_factor;

            // 采样区域的整数边界
            int16_t ox_min = (int16_t)orig_x_start;
            int16_t oy_min = (int16_t)orig_y_start;
            int16_t ox_max = (int16_t)orig_x_end;
            int16_t oy_max = (int16_t)orig_y_end;

            // 累加权重和"墨水浓度"
            // 墨水浓度: 1.0=前景色(文字), 0.5=灰色, 0.0=背景色
            float total_weight = 0.0f;
            float ink_sum = 0.0f;
            bool has_content = false;

            // 遍历覆盖区域内的所有原图像素
            for (int16_t oy = oy_min; oy <= oy_max && oy < orig_h; oy++)
            {
                for (int16_t ox = ox_min; ox <= ox_max && ox < orig_w; ox++)
                {
                    // 计算重叠面积作为权重
                    float x_overlap = fminf(orig_x_end, ox + 1.0f) - fmaxf(orig_x_start, (float)ox);
                    float y_overlap = fminf(orig_y_end, oy + 1.0f) - fmaxf(orig_y_start, (float)oy);
                    if (x_overlap <= 0.0f || y_overlap <= 0.0f)
                        continue;

                    float weight = x_overlap * y_overlap;

                    uint16_t pixel = bitmap[oy * orig_w + ox];
                    if (pixel == bg_color)
                        continue; // 跳过背景色

                    has_content = true;

                    // 将像素转换为"墨水浓度"
                    // 前景色 = 1.0, 灰色 = 0.5, 背景色 = 0.0
                    float ink_val;
                    if (pixel == fg_color)
                    {
                        ink_val = 1.0f; // 前景色（文字）
                    }
                    else
                    {
                        ink_val = 0.5f; // 灰色（抗锯齿）
                    }

                    ink_sum += ink_val * weight;
                    total_weight += weight;
                }
            }

            // 如果该区域有内容，根据加权平均墨水浓度决定输出颜色
            if (has_content && total_weight > 0.0f)
            {
                float avg_ink = ink_sum / total_weight;

                uint16_t output_color;

                // 根据墨水浓度映射到三阶颜色
                if (avg_ink > 0.75f)
                {
                    // 浓度高 → 前景色
                    output_color = fg_color;
                }
                else if (avg_ink > 0.25f)
                {
                    // 中等浓度 → 灰色
                    output_color = gray_out;
                }
                else
                {
                    // 浓度低 → 跳过（视为背景）
                    continue;
                }

                canvas->drawPixel(canvas_x + sx, canvas_y + sy, output_color);
            }
        }
    }
}

// Helper: ensure a fixed-size UTF-8 buffer does not end with a truncated multi-byte sequence
static void utf8_trim_tail(char *buf, size_t bufsize)
{
    if (!buf || bufsize == 0)
        return;
    // ensure null-termination within buffer
    buf[bufsize - 1] = '\0';
    size_t len = strnlen(buf, bufsize);
    if (len == 0 || len == bufsize - 1)
    {
        // if buffer full we still may have partial trailing bytes; continue
    }

    // Walk backwards past continuation bytes (0x80..0xBF)
    int i = (int)len - 1;
    if (i < 0)
        return;

    while (i >= 0 && ((unsigned char)buf[i] & 0xC0) == 0x80)
    {
        --i;
    }

    if (i < 0)
    {
        // no valid leading byte found, truncate to empty
        buf[0] = '\0';
        return;
    }

    unsigned char lead = (unsigned char)buf[i];
    size_t expected_len = 1;
    if ((lead & 0x80) == 0x00)
        expected_len = 1; // ASCII
    else if ((lead & 0xE0) == 0xC0)
        expected_len = 2; // 110x xxxx
    else if ((lead & 0xF0) == 0xE0)
        expected_len = 3; // 1110 xxxx
    else if ((lead & 0xF8) == 0xF0)
        expected_len = 4; // 11110 xxx
    else
    {
        // invalid leading byte, truncate it away
        buf[i] = '\0';
        return;
    }

    size_t available = (size_t)len - (size_t)i;
    if (available < expected_len)
    {
        // truncated sequence at end, cut it off at leading byte
        buf[i] = '\0';
    }
}

// Forward declaration in case header inclusion path differs in some build setups
bool resolve_fake_path(const std::string &fake_path, std::string &out_real_path, bool &out_use_spiffs);

// 缩放算法选择
// 0: 原始最近邻算法（快速但质量一般）
// 1: 超采样算法（适合灰度图像）
// 2: 双线性插值算法（适合灰度图像）
// 3: 二值图像专用算法（适合黑白二值图像）⭐
#define SCALING_ALGORITHM 3

// 灰度判断函数：参考bin_font_generator.py的处理逻辑
// 输入：16位值（实际是4位量化灰度0-15）-> 黑白判断
// ！实际上已经没有必要了 ，字体已经双色化
bool isBlack(uint16_t quantized_gray)
{
    // 从FontDecoder解码后，char_bitmap中每个像素是4位量化灰度值（0-15）
    // 参考bin_font_generator.py的量化逻辑：
    // - quantized == 15: 白色像素（对应原始<white_threshold）
    // - quantized == 0:  黑色像素（对应原始>black_threshold）
    // - quantized == 1-14: 中间灰度

    uint8_t gray4 = (uint8_t)(quantized_gray & 0x0F); // 提取低4位量化值

    if (gray4 == 15)
    {
        return false; // 白色/透明，不绘制
    }
    else if (gray4 == 0)
    {
        return true; // 黑色，绘制
    }
    else
    {
        // 中间灰度：将4位值(1-14)映射到0-31范围，然后与GRAY_THRESHOLD比较
        // gray4=1->2, gray4=14->30 (线性映射)
        //        uint8_t gray31 = (gray4 * 31) / 15;
        //       return gray31 < GRAY_THRESHOLD;  // 使用readpaper.h中定义的门限
        return gray4 < GRAY_THRESHOLD;
    }
}

// 全局变量定义
BinFont g_bin_font;
static int16_t g_cursor_x = 0;
static int16_t g_cursor_y = 0;
int16_t g_line_height = 0;
static int16_t g_screen_width = 400;
static int16_t g_margin_left = 10;
static int16_t g_margin_top = 10;
// g_canvas 已移到 main.cpp 作为全局唯一实例
extern M5Canvas *g_canvas;
File g_font_file;

// 当前加载的字体名称
static std::string g_current_font_name = "";

// 在轻量级索引中查找字形 - O(1) Hash 查找
const GlyphIndex *find_glyph_index(uint32_t unicode)
{
    if (unicode > 0xFFFF)
    {
        return nullptr;
    }

    uint16_t unicode16 = (uint16_t)unicode;

    // 使用 hash map 进行 O(1) 查找
    auto it = g_bin_font.indexMap.find(unicode16);
    if (it != g_bin_font.indexMap.end())
    {
        return it->second;
    }

    return nullptr;
}

// 统一获取字形信息的辅助结构和函数
struct GlyphInfo
{
    bool found;
    uint16_t width;
    uint8_t bitmapW;
    uint8_t bitmapH;
    int8_t x_offset;
    int8_t y_offset;
    uint32_t bitmap_offset;
    uint32_t bitmap_size;
};

// 统一的字形查找函数（支持两种模式）
GlyphInfo get_glyph_info(uint32_t unicode)
{
    GlyphInfo info = {false, 0, 0, 0, 0, 0, 0, 0};

    if (g_font_stream_mode)
    {
        const GlyphIndex *idx = find_glyph_index(unicode);
        if (idx)
        {
            info.found = true;
            info.width = idx->width;
            info.bitmapW = idx->bitmapW;
            info.bitmapH = idx->bitmapH;
            info.x_offset = idx->x_offset;
            info.y_offset = idx->y_offset;
            info.bitmap_offset = idx->bitmap_offset;
            info.bitmap_size = idx->bitmap_size;
        }
    }
    else
    {
        const BinFontChar *ch = find_char(unicode);
        if (ch)
        {
            info.found = true;
            info.width = ch->width;
            info.bitmapW = ch->bitmapW;
            info.bitmapH = ch->bitmapH;
            info.x_offset = ch->x_offset;
            info.y_offset = ch->y_offset;
            info.bitmap_offset = ch->bitmap_offset;
            info.bitmap_size = ch->bitmap_size;
        }
    }

    return info;
}

// 任务局部字形缓存：使用简单的数组+线性查找替代map（避免STL兼容性问题）
struct TaskGlyphEntry
{
    TaskHandle_t task;
    BinFontChar glyph;
};
static std::vector<TaskGlyphEntry> g_task_temp_glyphs;
static SemaphoreHandle_t g_temp_glyph_mutex = nullptr;

static void ensure_temp_glyph_mutex()
{
    if (!g_temp_glyph_mutex)
    {
        g_temp_glyph_mutex = xSemaphoreCreateMutex();
    }
}

static BinFontChar *get_task_glyph_storage()
{
    ensure_temp_glyph_mutex();
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    if (xSemaphoreTake(g_temp_glyph_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        return nullptr;
    }

    // 查找当前任务的存储
    for (auto &entry : g_task_temp_glyphs)
    {
        if (entry.task == current_task)
        {
            xSemaphoreGive(g_temp_glyph_mutex);
            return &entry.glyph;
        }
    }

    // 为新任务创建存储
    TaskGlyphEntry new_entry;
    new_entry.task = current_task;
    g_task_temp_glyphs.push_back(new_entry);
    BinFontChar *result = &g_task_temp_glyphs.back().glyph;

    xSemaphoreGive(g_temp_glyph_mutex);
    return result;
}

const BinFontChar *find_char(uint32_t unicode)
{
    // 检查Unicode范围，如果超出uint16_t范围，直接返回null
    if (unicode > 0xFFFF)
    {
        return nullptr;
    }

    uint16_t unicode16 = (uint16_t)unicode;

    // 流式模式：从索引查找并构造临时 BinFontChar
    if (g_font_stream_mode)
    {
        auto it = std::lower_bound(g_bin_font.index.begin(), g_bin_font.index.end(),
                                   unicode16, [](const GlyphIndex &idx, uint16_t unicode)
                                   { return idx.unicode < unicode; });

        if (it != g_bin_font.index.end() && it->unicode == unicode16)
        {
            // 获取当前任务的临时 glyph 存储
            BinFontChar *task_glyph = get_task_glyph_storage();
            if (!task_glyph)
            {
                return nullptr; // 互斥锁超时或分配失败
            }

            // 将索引数据复制到任务局部 BinFontChar
            task_glyph->unicode = it->unicode;
            task_glyph->width = it->width;
            task_glyph->bitmapW = it->bitmapW;
            task_glyph->bitmapH = it->bitmapH;
            task_glyph->x_offset = it->x_offset;
            task_glyph->y_offset = it->y_offset;
            task_glyph->bitmap_offset = it->bitmap_offset;
            task_glyph->bitmap_size = it->bitmap_size;
            task_glyph->cached_bitmap = 0;
            return task_glyph;
        }
        return nullptr;
    }

    // 缓存模式：从完整字符表查找
    auto it = std::lower_bound(g_bin_font.chars.begin(), g_bin_font.chars.end(),
                               unicode16, [](const BinFontChar &c, uint16_t unicode)
                               { return c.unicode < unicode; });

    if (it != g_bin_font.chars.end() && it->unicode == unicode16)
    {
        return &(*it);
    }
    return nullptr;
}

bool bin_font_has_glyph(uint32_t unicode)
{
    return find_char(unicode) != nullptr;
}

int16_t bin_font_get_glyph_width(uint32_t unicode)
{
    const BinFontChar *g = find_char(unicode);
    if (!g)
        return (int16_t)(g_bin_font.font_size / 2);
    return (int16_t)g->width;
}

int16_t bin_font_get_glyph_bitmapW(uint32_t unicode)
{
    const BinFontChar *g = find_char(unicode);
    if (!g)
        return (int16_t)(g_bin_font.font_size / 2);
    return (int16_t)g->bitmapW;
}

int16_t bin_font_get_glyph_bitmapH(uint32_t unicode)
{
    const BinFontChar *g = find_char(unicode);
    if (!g)
        return (int16_t)g_bin_font.font_size;
    return (int16_t)g->bitmapH;
}

uint32_t bin_font_get_glyph_bitmap_size(uint32_t unicode)
{
    const BinFontChar *g = find_char(unicode);
    if (!g)
        return 0;
    return g->bitmap_size;
}

uint8_t bin_font_get_font_size()
{
    return g_bin_font.font_size;
}

// 智能字体位图加载 - 根据 fontLoadLoc 配置选择策略
// fontLoadLoc == 1: 从 PROGMEM 读取（编译进代码的字体）
// fontLoadLoc == 0: 从文件缓存读取
// 优化：使用预读窗口加速SD卡字形读取
// 全局变量用于累计文件读取时间（用于性能统计）
static uint32_t g_total_glyph_read_us = 0;

bool load_glyph_bitmap_smart(uint32_t offset, uint8_t *buffer, uint32_t size)
{
#if DBG_BIN_FONT_PRINT
    static int call_count = 0;
    if (call_count < 5)
    {
        Serial.printf("[GLYPH] 🔍 调用 #%d: offset=%u size=%u\n", call_count++, offset, size);
    }
#endif

    if (g_font_stream_mode)
    {
        if (g_using_progmem_font)
        {
            // 从 PROGMEM 读取（Flash）
            progmem_read_buffer(offset, buffer, size);
            return true;
        }
        else
        {
            // 从SD卡文件读取（使用预读窗口优化）
            if (!g_bin_font.fontFile || !g_bin_font.fontFile.available())
            {
                return false;
            }

            // 获取互斥锁，防止并发访问导致seek位置混乱
            bool need_lock = (g_font_file_mutex != nullptr);
            if (need_lock)
            {
                if (xSemaphoreTake(g_font_file_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
                {
#if DBG_BIN_FONT_PRINT
                    Serial.printf("[GLYPH] ⚠️  获取字体文件互斥锁超时\n");
#endif
                    return false;
                }
            }

#if ENABLE_GLYPH_READ_WINDOW
            // ✨ 预读窗口机制：三层策略
            // 阶段1：检查是否在已有窗口内
            if (g_glyph_read_window.buffer && g_glyph_read_window.window_size > 0)
            {
                uint32_t window_start = g_glyph_read_window.window_offset;
                uint32_t window_end = window_start + g_glyph_read_window.window_size;

                if (offset >= window_start && offset + size <= window_end)
                {
                    // 命中！从预读窗口中复制数据
                    uint32_t offset_in_window = offset - window_start;
#if DBG_BIN_FONT_PRINT
                    uint32_t start_time = micros();
#endif
                    memcpy(buffer, g_glyph_read_window.buffer + offset_in_window, size);
#if DBG_BIN_FONT_PRINT
                    uint32_t copy_time = micros() - start_time;
                    g_total_glyph_read_us += copy_time; // 累计窗口命中的复制时间（很小）
#endif
                    // 释放互斥锁
                    if (need_lock)
                    {
                        xSemaphoreGive(g_font_file_mutex);
                    }
                    return true;
                }
            }

            // 阶段2：窗口未命中，尝试重新定位
            if (g_glyph_read_window.buffer && size < g_glyph_read_window.BUFFER_SIZE)
            {
                // 窗口足够大且还有空间，尝试重新定位以包含请求的数据
                if (g_glyph_read_window.reposition_window(g_bin_font.fontFile, offset))
                {
                    // 重定位成功，现在应该在窗口内了
                    uint32_t window_start = g_glyph_read_window.window_offset;
                    uint32_t offset_in_window = offset - window_start;

#if DBG_BIN_FONT_PRINT
                    uint32_t start_time = micros();
#endif
                    memcpy(buffer, g_glyph_read_window.buffer + offset_in_window, size);
#if DBG_BIN_FONT_PRINT
                    uint32_t copy_time = micros() - start_time;
                    g_total_glyph_read_us += copy_time; // 累计重定位后的复制时间
#endif
                    // 释放互斥锁
                    if (need_lock)
                    {
                        xSemaphoreGive(g_font_file_mutex);
                    }
                    return true;
                }
            }
#endif

            // 阶段3：预读窗口不可用或未启用，直接从SD卡读取
#if DBG_BIN_FONT_PRINT
            uint32_t start_time = micros(); // 使用微秒精度
#endif
            size_t read_bytes = SDW::SD.readAtOffset(g_bin_font.fontFile, offset, buffer, size);

#if DBG_BIN_FONT_PRINT
            uint32_t read_time = micros() - start_time;
            g_total_glyph_read_us += read_time; // 累计直接读取时间
#endif

            // 释放互斥锁
            if (need_lock)
            {
                xSemaphoreGive(g_font_file_mutex);
            }

            if (read_bytes != size)
            {
#if DBG_BIN_FONT_PRINT
                Serial.printf("[GLYPH] ❌ read失败 期望=%u 实际=%zu\n", size, read_bytes);
#endif
                return false;
            }
            return true;
        }
    }
    else
    {
        // 缓存模式：从分块缓存读取
        return g_chunked_font_cache.read_data(offset, buffer, size);
    }
}

// 从 PROGMEM 加载字体（fontLoadLoc == 1）
bool load_bin_font_from_progmem()
{
#if DBG_BIN_FONT_PRINT
    Serial.println("[FONT_PROGMEM] 开始从 PROGMEM 加载字体");
#endif

    if (!g_has_progmem_font || g_progmem_font_size < 134)
    {
#if DBG_BIN_FONT_PRINT
        Serial.println("[FONT_PROGMEM] 错误: PROGMEM 字体数据无效");
#endif
        return false;
    }

    // 读取头部（134字节）
    uint32_t char_count = progmem_read_uint32(0);
    uint8_t font_height = progmem_read_byte(4);
    uint8_t version = progmem_read_byte(5);

    // 读取字体族名和样式名
    char family_name[65] = {0};
    char style_name[65] = {0};
    progmem_read_buffer(6, (uint8_t *)family_name, 64);
    progmem_read_buffer(70, (uint8_t *)style_name, 64);

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT_PROGMEM] 字符数: %u, 高度: %u, 版本: %u\n", char_count, font_height, version);
    Serial.printf("[FONT_PROGMEM] 族名: %s, 样式: %s\n", family_name, style_name);
#endif

    // 初始化全局字体结构
    g_bin_font.char_count = char_count;
    g_bin_font.font_size = font_height;
    g_bin_font.version = version;
    strncpy(g_bin_font.family_name, family_name, sizeof(g_bin_font.family_name) - 1);
    strncpy(g_bin_font.style_name, style_name, sizeof(g_bin_font.style_name) - 1);

    // 根据 version 字段设置字体格式
    if (version == 2)
    {
        g_bin_font.format = FONT_FORMAT_1BIT;
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT_PROGMEM] 字体格式: V2 (FONT_FORMAT_1BIT)\n");
#endif
    }
    else if (version == 3)
    {
        g_bin_font.format = FONT_FORMAT_HUFFMAN;
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT_PROGMEM] 字体格式: V3 (FONT_FORMAT_HUFFMAN)\n");
#endif
    }
    else
    {
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT_PROGMEM] ⚠️ 未知版本 %u，默认使用 1bit\n", version);
#endif
        g_bin_font.format = FONT_FORMAT_1BIT;
    }

    // 清空现有数据
    g_bin_font.chars.clear();
    g_bin_font.index.clear();
    g_bin_font.indexMap.clear();

    // 读取字符表（每个条目20字节）
    uint32_t char_table_offset = 134; // 头部大小
    uint32_t entry_size = 20;

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT_PROGMEM] 开始读取 %u 个字符条目\n", char_count);
#endif

    // 流式模式：只加载轻量级索引
    g_bin_font.index.reserve(char_count);

    for (uint32_t i = 0; i < char_count; i++)
    {
        uint32_t entry_offset = char_table_offset + i * entry_size;

        // 读取条目数据
        uint16_t unicode = progmem_read_uint16(entry_offset);
        uint16_t width = progmem_read_uint16(entry_offset + 2);
        uint8_t bitmapW = progmem_read_byte(entry_offset + 4);
        uint8_t bitmapH = progmem_read_byte(entry_offset + 5);
        int8_t x_offset = progmem_read_int8(entry_offset + 6);
        int8_t y_offset = progmem_read_int8(entry_offset + 7);
        uint32_t bitmap_offset = progmem_read_uint32(entry_offset + 8);
        uint32_t bitmap_size = progmem_read_uint32(entry_offset + 12);

        // 创建轻量级索引
        GlyphIndex idx;
        idx.unicode = unicode;
        idx.width = width;
        idx.bitmapW = bitmapW;
        idx.bitmapH = bitmapH;
        idx.x_offset = x_offset;
        idx.y_offset = y_offset;
        idx.bitmap_offset = bitmap_offset;
        idx.bitmap_size = bitmap_size;

        g_bin_font.index.push_back(idx);

        // 喂狗
        if (i % 1000 == 0)
        {
            esp_task_wdt_reset();
        }
    }

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT_PROGMEM] 索引加载完成: %u 个字符\n", g_bin_font.index.size());
#endif

    // 构建 Hash Map
    g_bin_font.indexMap.clear();
    for (size_t i = 0; i < g_bin_font.index.size(); i++)
    {
        g_bin_font.indexMap[g_bin_font.index[i].unicode] = &g_bin_font.index[i];
    }

    // 设置为流式模式，并标记为使用PROGMEM字体
    g_font_stream_mode = true;
    g_using_progmem_font = true;

    // 计算行高等参数
    g_line_height = g_bin_font.font_size + LINE_MARGIN;
    g_cursor_x = g_margin_left;
    g_cursor_y = g_margin_top;

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT_PROGMEM] ✅ PROGMEM 字体加载成功\n");
    Serial.printf("[FONT_PROGMEM] Flash 占用: %u 字节\n", g_progmem_font_size);
    Serial.printf("[FONT_PROGMEM] RAM 占用: %u 字节（仅索引）\n",
                  g_bin_font.index.size() * sizeof(GlyphIndex));
#endif

    return true;
}

// 检测字体格式
FontFormat detect_font_format(File &f)
{
    // 保存当前位置
    size_t original_pos = f.position();

    f.seek(0);
    if (f.size() < 134) // 新格式需要至少134字节的头部
    {
        f.seek(original_pos);
        return FONT_FORMAT_UNKNOWN;
    }

    // 读取前6字节
    uint8_t header[6];
    f.read(header, 6);
    f.seek(original_pos);

    // 检查新的头部格式（支持 V2(1bit) 与 V3(2bit Huffman)）
    // 格式：char_count(4) + font_height(1) + version(1) + family_name(64) + style_name(64)
    uint32_t char_count = *(uint32_t *)header;
    uint8_t font_height = header[4];
    uint8_t version = header[5];

    // 版本2: 1bit 格式
    if (version == 2 && font_height >= 20 && font_height <= 50 &&
        char_count > 0 && char_count <= 50000)
    {
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT_DETECT] 检测到1bit格式 v2: height=%d, chars=%u\n",
                      font_height, char_count);
#endif
        return FONT_FORMAT_1BIT;
    }

    // 版本3: V3 字体，使用 2bit Huffman 编码
    if (version == 3 && font_height >= 8 && font_height <= 200 &&
        char_count > 0 && char_count <= 50000)
    {
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT_DETECT] 检测到 V3 字体 (2bit Huffman): height=%d, chars=%u\n",
                      font_height, char_count);
#endif
        return FONT_FORMAT_HUFFMAN;
    }

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT_DETECT] 不支持的格式: chars=%u, height=%d, version=%d (仅支持版本2)\n",
                  char_count, font_height, version);
#endif
    return FONT_FORMAT_UNKNOWN;
}

bool load_bin_font(const char *path)
{
#if DBG_BIN_FONT_PRINT
    unsigned long fontLoadStart = millis();
    Serial.printf("[FONT] 开始加载字体: %s\n", path);
#endif

    // 卸载旧字体时会自动清理缓冲区
    // 这里显式清理以确保切换字体前状态干净
    g_font_buffer_manager.clearAll();

    if (strcmp(path, "default") == 0)
        path = "/spiffs/lite.bin";

    // 检查是否应该使用 PROGMEM（默认字体）
    // 条件：
    // 1. fontLoadLoc == 1（启用流式模式）
    // 2. g_has_progmem_font == true（有内置字体）
    // 3. path 为 "default" 或空或 "/spiffs/lite.bin"（默认字体请求）
    bool is_default_font_request = false;
    if (path == nullptr || path[0] == '\0' ||
        strcmp(path, "default") == 0 ||
        strcmp(path, "/spiffs/lite.bin") == 0)
    {
        is_default_font_request = true;
    }

    // 如果是默认字体请求，且有PROGMEM字体，优先使用PROGMEM
    if (is_default_font_request && fontLoadLoc == 1 && g_has_progmem_font)
    {
#if DBG_BIN_FONT_PRINT
        Serial.println("[FONT_LOAD] === 使用 PROGMEM 模式（编译进代码的字体）===");
        Serial.printf("[FONT_LOAD] PROGMEM 字体大小: %u 字节 (%.2f KB)\n",
                      g_progmem_font_size, g_progmem_font_size / 1024.0f);
#endif
        return load_bin_font_from_progmem();
    }

    // 使用统一的解析函数去掉伪前缀（/sd, /spiffs）并决定使用哪种文件系统
    std::string real_path_str;
    bool use_spiffs = false;
    if (!resolve_fake_path(std::string(path), real_path_str, use_spiffs))
    {
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] 路径解析失败: %s\n", path);
#endif
        return false;
    }
#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 解析后路径: %s (来源: %s)\n", real_path_str.c_str(), use_spiffs ? "SPIFFS" : "SD");
#endif
    const char *real_path = real_path_str.c_str();

    // 根据判断结果打开文件
    File f;
    if (use_spiffs)
    {
        f = SPIFFS.open(real_path, "r");
    }
    else
    {
        f = SDW::SD.open(real_path, "r");
    }

    if (!f)
    {
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] 错误: 无法打开字体文件 (%s 从 %s)\n", real_path, use_spiffs ? "SPIFFS" : "SD");
#endif
        return false;
    }

    size_t sz = f.size();
#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 文件大小: %u 字节 (来源: %s)\n", sz, use_spiffs ? "SPIFFS" : "SD");
#endif

    if (sz < 6)
    {
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] 错误: 文件过小，需要至少6字节头部\n");
#endif
        f.close();
        return false;
    }

    // 检测字体格式
    FontFormat format = detect_font_format(f);
    if (format == FONT_FORMAT_UNKNOWN)
    {
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] 错误: 未知的字体格式\n");
#endif
        f.close();
        return false;
    }

    g_bin_font.format = format;
#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 检测到1bit格式v2\n");
#endif

    // =============== 新增：缓存头文件到 PSRAM（仅134字节） ===============
    // 计算头文件大小
    const size_t header_size = 134; // 6字节基础头 + 64字节族名 + 64字节样式名

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT_CACHE] 准备缓存头文件: %u 字节\n", header_size);
#endif

    // 分配 PSRAM 缓存（仅头文件）
    if (!g_font_header_cache.allocate(header_size))
    {
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT_CACHE] ⚠️ PSRAM 缓存分配失败，将使用传统方式加载\n");
#endif
        // 降级到传统方式，继续执行
        g_font_header_cache.is_cached = false;
    }
    else
    {
        // 读取头文件到缓存
        f.seek(0);
        size_t read_bytes = f.read(g_font_header_cache.header_data, header_size);
        if (read_bytes != header_size)
        {
#if DBG_BIN_FONT_PRINT
            Serial.printf("[FONT_CACHE] ⚠️ 头文件读取失败，期望 %u 字节，实际 %u 字节\n",
                          header_size, read_bytes);
#endif
            g_font_header_cache.cleanup();
        }
        else
        {
#if DBG_BIN_FONT_PRINT
            Serial.printf("[FONT_CACHE] ✅ 头文件已缓存到 PSRAM\n");
#endif
        }
    }
    // =============== 头文件缓存完成 ===============

    // 从缓存或文件读取头文件（134字节）
    f.seek(0);
    uint8_t header[6];
    if (g_font_header_cache.is_cached)
    {
        // 从缓存读取
        memcpy(header, g_font_header_cache.header_data, 6);
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT_CACHE] 从 PSRAM 缓存读取头文件\n");
#endif
    }
    else
    {
        // 从文件读取
        f.read(header, 6);
    }

    // 读取版本2格式头部: char_count(4) + font_height(1) + version(1) + family_name(64) + style_name(64)
    g_bin_font.char_count = *(uint32_t *)&header[0];
    g_bin_font.font_size = header[4];
    g_bin_font.version = header[5];

    // 读取字体族名（64字节）
    if (g_font_header_cache.is_cached)
    {
        memcpy(g_bin_font.family_name, g_font_header_cache.header_data + 6, 64);
    }
    else
    {
        f.read((uint8_t *)g_bin_font.family_name, 64);
    }
    // 确保以null结尾并修剪可能的截断UTF-8尾部
    g_bin_font.family_name[63] = '\0';
    utf8_trim_tail(g_bin_font.family_name, sizeof(g_bin_font.family_name));

    // 读取字体样式名（64字节）
    if (g_font_header_cache.is_cached)
    {
        memcpy(g_bin_font.style_name, g_font_header_cache.header_data + 70, 64);
    }
    else
    {
        f.read((uint8_t *)g_bin_font.style_name, 64);
    }
    // 同样确保样式名以null结尾并修剪可能的截断UTF-8尾部
    g_bin_font.style_name[63] = '\0';
    utf8_trim_tail(g_bin_font.style_name, sizeof(g_bin_font.style_name));

#if DBG_BIN_FONT_PRINT
    // 诊断：打印原始64字节（hex）以及使用 utf8_decode 解析得到的 codepoint 列表
    Serial.print("[FONT_DBG] family raw bytes: ");
    const uint8_t *family_raw = (const uint8_t *)g_bin_font.family_name;
    for (size_t i = 0; i < 64; ++i)
    {
        Serial.printf("%02X", family_raw[i]);
        if (i + 1 < 64)
            Serial.print(" ");
    }
    Serial.println();

    Serial.print("[FONT_DBG] family decoded codepoints: ");
    const uint8_t *utf8ptr = (const uint8_t *)g_bin_font.family_name;
    const uint8_t *utf8end = (const uint8_t *)g_bin_font.family_name + 64;
    while (utf8ptr < utf8end && *utf8ptr)
    {
        const uint8_t *prev = utf8ptr;
        uint32_t cp = utf8_decode(utf8ptr, utf8end);
        if (cp == 0)
        {
            // 如果解码失败，打印出失败位置的原始字节并跳出
            Serial.print("[FONT_DBG] <invalid utf8> ");
            break;
        }
        Serial.printf("U+%04X ", cp);
        // 保护性避免无限循环
        if (utf8ptr <= prev)
            break;
    }
    Serial.println();

    Serial.print("[FONT_DBG] style raw bytes: ");
    const uint8_t *style_raw = (const uint8_t *)g_bin_font.style_name;
    for (size_t i = 0; i < 64; ++i)
    {
        Serial.printf("%02X", style_raw[i]);
        if (i + 1 < 64)
            Serial.print(" ");
    }
    Serial.println();

    Serial.print("[FONT_DBG] style decoded codepoints: ");
    utf8ptr = (const uint8_t *)g_bin_font.style_name;
    utf8end = (const uint8_t *)g_bin_font.style_name + 64;
    while (utf8ptr < utf8end && *utf8ptr)
    {
        const uint8_t *prev = utf8ptr;
        uint32_t cp = utf8_decode(utf8ptr, utf8end);
        if (cp == 0)
        {
            Serial.print("[FONT_DBG] <invalid utf8> ");
            break;
        }
        Serial.printf("U+%04X ", cp);
        if (utf8ptr <= prev)
            break;
    }
    Serial.println();
#endif

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 1bit格式v%u - 字符数: %u, 字体大小: %u, 族名: %s, 样式: %s\n",
                  g_bin_font.version, g_bin_font.char_count, g_bin_font.font_size,
                  g_bin_font.family_name, g_bin_font.style_name);
#endif

    if (g_bin_font.char_count == 0 || g_bin_font.char_count > 65534)
    {
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] 错误: 字符数量异常: %u\n", g_bin_font.char_count);
#endif
        return false;
    }

    // 读取字符表 - 先清空，reserve操作在后面进行
#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 清空字符向量，准备加载\n");
#endif
    g_bin_font.chars.clear();

    const size_t struct_size = 20;
    size_t total_chars_size = g_bin_font.char_count * struct_size;

    // 计算字符表的正确起始位置
    // 对于新的格式（版本2/3），头部包含 6 字节基础头 + 64 字节族名 + 64 字节样式名
    // 旧的霍夫曼格式（version==1）使用 5 字节头部
    size_t char_table_offset;
    if (g_bin_font.version >= 2)
    {
        char_table_offset = 6 + 64 + 64; // 基础头部6字节 + 族名64字节 + 样式名64字节
    }
    else
    {
        // 旧霍夫曼格式 (version 1)
        char_table_offset = 5;
    }

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 字符表起始位置: %zu\n", char_table_offset);
#endif

    // 定位到字符表起始位置
    f.seek(char_table_offset);

    // =============== 从文件读取索引表（总是从文件读取，不缓存原始字节） ===============
    uint8_t *chars_buffer = nullptr;

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 需要分配字符表缓冲区: %u 字节 (%u KB)\n", total_chars_size, total_chars_size / 1024);
    Serial.printf("[FONT] 当前内存状态 - 堆: %u bytes, PSRAM: %u bytes\n",
                  esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif

    // 使用PSRAM分配大型临时缓冲区，避免内部RAM耗尽
#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 尝试分配PSRAM缓冲区...\n");
#endif
    chars_buffer = (uint8_t *)heap_caps_malloc(total_chars_size, MALLOC_CAP_SPIRAM);
    if (!chars_buffer)
    {
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] PSRAM分配失败，尝试内部RAM\n");
#endif
        // PSRAM不可用时回退到普通内存，但增加错误处理
        chars_buffer = (uint8_t *)malloc(total_chars_size);
        if (!chars_buffer)
        {
#if DBG_BIN_FONT_PRINT
            Serial.printf("[FONT] 错误: 无法分配字符表缓冲区 %u 字节\n", total_chars_size);
#endif
            f.close();
            return false;
        }
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] 警告: 使用内部RAM分配字符表缓冲区\n");
#endif
    }
    else
    {
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] 使用PSRAM分配字符表缓冲区成功 %u KB\n", total_chars_size / 1024);
#endif
    }

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 开始读取字符表数据...\n");
    unsigned long read_start = micros();
#endif

    // 优化：一次读取或大块读取索引表，避免过多的seek操作
    // SD卡小数据多次seek的开销很大，建议尽量一次读取
    size_t bytes_read = 0;

    // 首先尝试一次读取全部
    bytes_read = f.read(chars_buffer, total_chars_size);

    // 喂看门狗
    esp_task_wdt_reset();

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 一次读取结果: 期望 %u 字节，实际 %u 字节\n", total_chars_size, bytes_read);
#endif

    // 如果一次读取不完整，才进行分块补读
    if (bytes_read < total_chars_size)
    {
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] 警告: 一次读取不完整，尝试分块补读\n");
#endif

        // 分块读取剩余部分（每次 64KB，更大的块以减少seek次数）
        const size_t chunk_size = 65536; // 64KB per chunk
        while (bytes_read < total_chars_size)
        {
            // 确保定位到正确的文件位置
            size_t file_offset = char_table_offset + bytes_read;
            if (!f.seek(file_offset))
            {
#if DBG_BIN_FONT_PRINT
                Serial.printf("[FONT] 错误: 无法seek到 %u\n", file_offset);
#endif
                break;
            }

            size_t to_read = (total_chars_size - bytes_read) < chunk_size ? (total_chars_size - bytes_read) : chunk_size;
            size_t actual_read = f.read(chars_buffer + bytes_read, to_read);
            bytes_read += actual_read;

            // 喂看门狗
            esp_task_wdt_reset();

            if (actual_read != to_read)
            {
#if DBG_BIN_FONT_PRINT
                Serial.printf("[FONT] 警告: 分块读取不完整，期望 %u 字节，实际 %u 字节\n", to_read, actual_read);
#endif
                break;
            }

#if DBG_BIN_FONT_PRINT
            if (bytes_read % (chunk_size * 2) == 0 || bytes_read == total_chars_size)
            {
                Serial.printf("[FONT] 补读进度: %u/%u 字节 (%.1f%%)\n",
                              bytes_read, total_chars_size, (float)bytes_read * 100.0f / total_chars_size);
            }
#endif
        }
    }

#if DBG_BIN_FONT_PRINT
    unsigned long read_end = micros();
    Serial.printf("[FONT_READ] 读取字符表完成: %u字节 (%u个字符), 耗时 %.1f ms\n",
                  bytes_read, g_bin_font.char_count, (read_end - read_start) / 1000.0f);

    // 验证读取的数据完整性
    if (bytes_read != total_chars_size)
    {
        Serial.printf("[FONT_READ] ❌ 数据不完整: 期望 %u 字节，实际 %u 字节\n", total_chars_size, bytes_read);
    }
#endif

    // 预留字符向量容量，避免多次重分配 - 改为不预分配，让vector自然增长
#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 步骤1: 清空字符向量\n");
#endif
    g_bin_font.chars.clear();

    // 显示内存状态
#if DBG_BIN_FONT_PRINT
    size_t free_heap = esp_get_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    Serial.printf("[FONT] 步骤2: 开始解析前内存 - 堆: %u bytes, PSRAM: %u bytes\n", free_heap, free_psram);
#endif

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 步骤3: 跳过reserve，直接解析字符 (让vector自然增长)\n");
#endif

    size_t offset = 0;

    // 根据模式决定构建完整字符表还是轻量级索引
    bool use_stream_index = (fontLoadLoc == 1);

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 开始解析字符数据，总计 %u 个字符 (模式: %s)\n",
                  g_bin_font.char_count, use_stream_index ? "轻量级索引" : "完整加载");
#endif

    if (use_stream_index)
    {
        // 流式模式：只构建轻量级索引（节省内存）
        g_bin_font.index.clear();
        g_bin_font.chars.clear(); // 确保不占用内存

        for (uint32_t i = 0; i < g_bin_font.char_count; ++i)
        {
            GlyphIndex idx;
            idx.unicode = *(uint16_t *)&chars_buffer[offset];
            offset += 2;
            idx.width = *(uint16_t *)&chars_buffer[offset];
            offset += 2;
            idx.bitmapW = chars_buffer[offset];
            offset += 1;
            idx.bitmapH = chars_buffer[offset];
            offset += 1;
            idx.x_offset = *(int8_t *)&chars_buffer[offset];
            offset += 1;
            idx.y_offset = *(int8_t *)&chars_buffer[offset];
            offset += 1;
            idx.bitmap_offset = *(uint32_t *)&chars_buffer[offset];
            offset += 4;
            idx.bitmap_size = *(uint32_t *)&chars_buffer[offset];
            offset += 4;
            offset += 4; // 跳过 cached_bitmap 字段

            g_bin_font.index.push_back(idx);

            if ((i + 1) % 5000 == 0 || i == 0 || i == g_bin_font.char_count - 1)
            {
                // 喂看门狗
                esp_task_wdt_reset();

#if DBG_BIN_FONT_PRINT
                Serial.printf("[FONT] 已解析索引 %u/%u (%.1f%%), 堆内存: %u bytes\n",
                              i + 1, g_bin_font.char_count, (float)(i + 1) * 100.0 / g_bin_font.char_count,
                              esp_get_free_heap_size());
#endif
            }
        }

#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] 轻量级索引构建完成，索引大小: %u 项 (%u KB)\n",
                      g_bin_font.index.size(),
                      (g_bin_font.index.size() * sizeof(GlyphIndex)) / 1024);
        Serial.printf("[FONT] 开始构建 Hash Map 用于 O(1) 查找...\n");
#endif

        // 构建 Hash Map 用于 O(1) 查找
        g_bin_font.indexMap.clear();
        g_bin_font.indexMap.reserve(g_bin_font.index.size());

        for (auto &idx : g_bin_font.index)
        {
            g_bin_font.indexMap[idx.unicode] = &idx;
        }

#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] Hash Map 构建完成，映射表大小: %u 项\n", g_bin_font.indexMap.size());
#endif
    }
    else
    {
        // 缓存模式：构建完整字符表
        g_bin_font.chars.clear();
        g_bin_font.index.clear(); // 确保不占用内存

        for (uint32_t i = 0; i < g_bin_font.char_count; ++i)
        {
            BinFontChar c;
            c.unicode = *(uint16_t *)&chars_buffer[offset];
            offset += 2;
            c.width = *(uint16_t *)&chars_buffer[offset];
            offset += 2;
            c.bitmapW = chars_buffer[offset];
            offset += 1;
            c.bitmapH = chars_buffer[offset];
            offset += 1;
            c.x_offset = *(int8_t *)&chars_buffer[offset];
            offset += 1;
            c.y_offset = *(int8_t *)&chars_buffer[offset];
            offset += 1;
            c.bitmap_offset = *(uint32_t *)&chars_buffer[offset];
            offset += 4;
            c.bitmap_size = *(uint32_t *)&chars_buffer[offset];
            offset += 4;
            c.cached_bitmap = *(uint32_t *)&chars_buffer[offset];
            offset += 4;

            g_bin_font.chars.push_back(c);

            if ((i + 1) % 5000 == 0 || i == 0 || i == g_bin_font.char_count - 1)
            {
                // 喂看门狗
                esp_task_wdt_reset();

#if DBG_BIN_FONT_PRINT
                Serial.printf("[FONT] 已解析 %u/%u 个字符 (%.1f%%), 堆内存: %u bytes\n",
                              i + 1, g_bin_font.char_count, (float)(i + 1) * 100.0 / g_bin_font.char_count,
                              esp_get_free_heap_size());
#endif
            }
        }

#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] 完整字符表构建完成，大小: %u 项\n", g_bin_font.chars.size());
#endif
    }

    // 释放临时缓冲区（索引表从文件读取，读完后即可释放）
    free(chars_buffer);
#if DBG_BIN_FONT_PRINT
    Serial.printf("[MEM] free chars_buffer (size=%u): heap_free=%u, psram_free=%u\n", total_chars_size, esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    Serial.printf("[FONT] 临时字符表缓冲区已释放\n");
#endif

    // 输出内存使用情况
#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 堆内存剩余: %u bytes, PSRAM剩余: %u bytes\n",
                  esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif

    // 根据模式进行排序
    if (use_stream_index)
    {
        // 流式模式：不需要排序（使用 Hash Map O(1) 查找）
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] 流式模式：使用 Hash Map，无需排序\n");
#endif
    }
    else
    {
        // 缓存模式：对完整字符表排序以支持二分搜索
        std::sort(g_bin_font.chars.begin(), g_bin_font.chars.end(),
                  [](const BinFontChar &a, const BinFontChar &b)
                  {
                      return a.unicode < b.unicode;
                  });
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] 缓存模式：字符表已排序\n");
#endif
    }

#if DBG_BIN_FONT_PRINT
    // 诊断：检查 family/style 中的字符是否存在于字符表中，并打印其位图信息
    auto print_lookup_info = [](const char *name, const char *buf)
    {
        Serial.printf("[FONT_DBG] 查找名字 '%s' 中的字符...\n", name);
        const uint8_t *p = (const uint8_t *)buf;
        const uint8_t *end = p + 64;
        while (p < end && *p)
        {
            const uint8_t *prev = p;
            uint32_t cp = utf8_decode(p, end);
            if (cp == 0)
                break;
            const BinFontChar *ch = find_char(cp);
            if (ch)
            {
                Serial.printf("  U+%04X -> found, bitmap_size=%u, offset=%u, width=%u\n", cp, ch->bitmap_size, ch->bitmap_offset, ch->width);
            }
            else
            {
                Serial.printf("  U+%04X -> NOT FOUND\n", cp);
            }
            if (p <= prev)
                break;
        }
    };

    print_lookup_info("family", g_bin_font.family_name);
    print_lookup_info("style", g_bin_font.style_name);
#endif

    f.close();

    // 重新打开字体文件用于后续访问，使用相同的存储设备判断逻辑
    if (use_spiffs)
    {
        g_bin_font.fontFile = SPIFFS.open(real_path, "r");
    }
    else
    {
        g_bin_font.fontFile = SDW::SD.open(real_path, "r");
    }

    if (!g_bin_font.fontFile)
    {
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT] 错误: 无法重新打开字体文件 (%s 从 %s)\n", real_path, use_spiffs ? "SPIFFS" : "SD");
#endif
        return false;
    }

    // 保存字体文件路径供后续重新打开使用
    strncpy(g_bin_font.font_path, real_path, sizeof(g_bin_font.font_path) - 1);
    g_bin_font.font_path[sizeof(g_bin_font.font_path) - 1] = '\0';
    g_bin_font.use_spiffs = use_spiffs;

    // 文件路径加载：明确不是PROGMEM
    g_using_progmem_font = false;
    // 根据 fontLoadLoc 全局变量决定加载模式
    g_font_stream_mode = (fontLoadLoc == 1);

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT_LOAD] 加载模式: %s (fontLoadLoc=%d)\n",
                  g_font_stream_mode ? "流式读取" : "分块缓存", fontLoadLoc);
#endif

    if (g_font_stream_mode)
    {
        // 流式模式：保持文件打开，字形按需读取
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT_LOAD] ✅ 流式模式已启用，字体文件保持打开状态\n");
        Serial.printf("[FONT_LOAD] 字体文件: %s, 大小: %u 字节\n", real_path, g_bin_font.fontFile.size());
#endif
    }
    else
    {
        // 缓存模式：加载全部字体数据到分块缓存
#if DBG_BIN_FONT_PRINT
        Serial.printf("[FONT_LOAD] === 分块缓存系统启动 ===\n");
#endif
        bool chunked_cache_ok = g_chunked_font_cache.load_entire_font_chunked(g_bin_font.fontFile, CACHE_BLOCK_SIZE);

        if (chunked_cache_ok)
        {
#if DBG_BIN_FONT_PRINT
            Serial.printf("[FONT_LOAD] ✅ 分块缓存加载成功，字体数据已分布式存储\n");
#endif
            g_chunked_font_cache.print_stats();
        }
        else
        {
#if DBG_BIN_FONT_PRINT
            Serial.printf("[FONT_LOAD] ❌ 分块缓存失败，字体加载不可用\n");
#endif
            return false;
        }
    }

    // 分析字符分布
    uint16_t min_unicode = 0xFFFF, max_unicode = 0;
    uint32_t total_bitmap_bytes = 0;
    for (const auto &ch : g_bin_font.chars)
    {
        if (ch.unicode < min_unicode)
            min_unicode = ch.unicode;
        if (ch.unicode > max_unicode)
            max_unicode = ch.unicode;
        total_bitmap_bytes += ch.bitmap_size;
    }

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT_LOAD] 字符范围: U+%04X - U+%04X (%u个字符)\n",
                  min_unicode, max_unicode, g_bin_font.chars.size());
    Serial.printf("[FONT_LOAD] 位图数据总量: %u字节 (%.2fMB)\n",
                  total_bitmap_bytes, total_bitmap_bytes / (1024.0 * 1024.0));
    Serial.printf("[FONT_LOAD] 平均字符大小: %.1f字节\n",
                  total_bitmap_bytes / (float)g_bin_font.chars.size());
#endif

    g_line_height = g_bin_font.font_size + LINE_MARGIN;
    g_cursor_x = g_margin_left;
    g_cursor_y = g_margin_top;

    // 创建字体文件访问互斥锁
    if (g_font_file_mutex == nullptr)
    {
        g_font_file_mutex = xSemaphoreCreateMutex();
        if (g_font_file_mutex == nullptr)
        {
#if DBG_BIN_FONT_PRINT
            Serial.printf("[FONT_LOAD] ⚠️  创建字体文件互斥锁失败\n");
#endif
        }
        else
        {
#if DBG_BIN_FONT_PRINT
            Serial.printf("[FONT_LOAD] ✅ 字体文件互斥锁创建成功\n");
#endif
        }
    }

    // 构建通用字符缓存（UI/菜单常用字符）
    if (g_font_stream_mode)
    {
        buildCommonCharCache();

        // 初始化通用回收池（空池）
        initCommonRecyclePool();

        // 如果有当前打开的书籍，初始化其5页字体缓存
        if (g_current_book)
        {
            g_current_book->tryInitializeFontCache();
#if DBG_BIN_FONT_PRINT
            Serial.printf("[FONT_LOAD] Initialized 5-page font cache for current book (page %u)\n",
                          (unsigned)g_current_book->getCurrentPageIndex());
#endif

            // 重建TOC字体缓存（如果当前书籍有TOC文件）
            std::string book_path = g_current_book->filePath();
            std::string toc_path = book_path;
            size_t dotpos = toc_path.find_last_of('.');
            if (dotpos != std::string::npos)
                toc_path = toc_path.substr(0, dotpos) + ".idx";
            else
                toc_path += ".idx";

            bool toc_exists = false;
            if (book_path.rfind("/spiffs/", 0) == 0)
            {
                std::string rel = std::string("/") + toc_path.substr(8);
                toc_exists = SPIFFS.exists(rel.c_str());
                if (toc_exists)
                    toc_path = std::string("/spiffs") + rel;
            }
            else
            {
                if (SDW::SD.exists(toc_path.c_str()))
                {
                    toc_exists = true;
                }
                else if (toc_path.rfind("/sd/", 0) == 0)
                {
                    std::string rel = toc_path.substr(3);
                    if (SDW::SD.exists(rel.c_str()))
                    {
                        toc_exists = true;
                        toc_path = std::string("/sd") + rel;
                    }
                }
            }

            if (toc_exists)
            {
                buildTocCharCache(toc_path.c_str());
#if DBG_BIN_FONT_PRINT
                Serial.printf("[FONT_LOAD] TOC font cache rebuilt from: %s\n", toc_path.c_str());
#endif
            }
        }
    }

    // 记录当前加载的字体名称
    g_current_font_name = std::string(path);

#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT] 字体加载完成: %u 字符, 总耗时: %lu ms\n",
                  g_bin_font.chars.size(), millis() - fontLoadStart);
    Serial.printf("[FONT_LOAD] === 加载完成 ===\n");

    Serial.printf("[MEM] 堆内存剩余: %u bytes, PSRAM剩余: %u bytes\n",
                  esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif

    // 根据模式决定是否关闭文件
    if (g_font_stream_mode)
    {
        // 流式模式：保持文件打开以便后续字形读取
#if DBG_BIN_FONT_PRINT
        Serial.println("[FONT_LOAD] 流式模式：字体文件保持打开状态");
#endif
    }
    else
    {
        // 缓存模式：数据已加载到内存，可以关闭文件
        if (g_bin_font.fontFile)
        {
            g_bin_font.fontFile.close();
#if DBG_BIN_FONT_PRINT
            Serial.println("[FONT_LOAD] 缓存模式：字体文件已关闭");
#endif
        }
    }

    // ✨ 优化：为SD卡字体分配字形预读窗口，加速实时渲染
    // 预读窗口机制：三层缓存策略（窗口命中 → 重定位 → 直接读）
    // 策略：静态初始化窗口 + 智能重定位（不破坏旧窗口的安全重定位）
    // 仅在流式模式下分配（缓存模式已有分块缓存）
#if ENABLE_GLYPH_READ_WINDOW
    if (g_font_stream_mode && !g_using_progmem_font)
    {
        // 先清理旧的窗口
        g_glyph_read_window.cleanup();

        if (g_glyph_read_window.allocate())
        {
            // 计算字形数据的起始位置（在索引表之后）
            uint32_t glyph_start_offset = 134 + (g_bin_font.char_count * 20);

            // 初始化窗口为字形数据的起始位置
            if (g_glyph_read_window.reposition_window(g_bin_font.fontFile, glyph_start_offset))
            {
#if DBG_BIN_FONT_PRINT
                Serial.printf("[FONT_LOAD] ✨ 字形预读窗口已初始化 (256KB @ offset %u)\n", glyph_start_offset);
                Serial.printf("[FONT_LOAD] ✅ 启用三层缓存策略（窗口命中 → 重定位 → 直接读）\n");
#endif
            }
            else
            {
#if DBG_BIN_FONT_PRINT
                Serial.printf("[FONT_LOAD] ⚠️  字形预读窗口初始化失败\n");
#endif
                g_glyph_read_window.cleanup();
            }
        }
        else
        {
#if DBG_BIN_FONT_PRINT
            Serial.printf("[FONT_LOAD] ⚠️  字形预读窗口分配失败\n");
#endif
        }
    }
#else
    // 预读窗口已禁用 - 强制使用直接读取模式
    g_glyph_read_window.cleanup();
#if DBG_BIN_FONT_PRINT
    Serial.printf("[FONT_LOAD] 📌 预读窗口已禁用（ENABLE_GLYPH_READ_WINDOW=0），使用直接读取\n");
#endif
#endif

    return true;
}

void unload_bin_font()
{
    // 清理PSRAM缓存
    g_font_header_cache.cleanup();
    g_glyph_read_window.cleanup(); // 清理字形预读窗口

    // 清理页面字体缓冲区
    g_font_buffer_manager.clearAll();

    // 清理通用/书名/TOC/回收池缓存，防止残留旧字形位图
    g_common_char_cache.clear();
    clearBookNameCache();
    clearTocCache();
    clearCommonRecyclePool();

    // 清理任务局部临时 glyph 缓存
    if (g_temp_glyph_mutex)
    {
        if (xSemaphoreTake(g_temp_glyph_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            g_task_temp_glyphs.clear();
            xSemaphoreGive(g_temp_glyph_mutex);
        }
    }

    // 清空并释放底层内存（swap with empty）以避免残留在 PSRAM/DRAM 中
    {
        std::vector<BinFontChar, PSRAMAllocator<BinFontChar>>().swap(g_bin_font.chars);
        std::vector<GlyphIndex, PSRAMAllocator<GlyphIndex>>().swap(g_bin_font.index);
        decltype(g_bin_font.indexMap)().swap(g_bin_font.indexMap);
    }
    g_using_progmem_font = false;
    if (g_bin_font.fontFile)
    {
        g_bin_font.fontFile.close();
    }
    g_memory_pool.cleanup();

    // 只在缓存模式下清理缓存
    if (!g_font_stream_mode)
    {
        g_chunked_font_cache.cleanup();
    }

    // 释放字体文件互斥锁
    if (g_font_file_mutex != nullptr)
    {
        vSemaphoreDelete(g_font_file_mutex);
        g_font_file_mutex = nullptr;
#if DBG_STATE_MACHINE_TASK
        Serial.printf("[BIN_FONT] 字体文件互斥锁已释放\n");
#endif
    }

    g_cursor_x = g_margin_left;
    g_cursor_y = g_margin_top;
    g_current_font_name.clear();
    g_font_stream_mode = false; // 重置模式标志

#if DBG_STATE_MACHINE_TASK
    Serial.printf("[BIN_FONT] 字体已卸载 (模式: %s)\n",
                  g_font_stream_mode ? "流式" : "缓存");
#endif
}

// 获取当前加载的字体名称
const char *get_current_font_name()
{
    static char full_name[130]; // 64 + 1 + 64 + 1 = 130 (family + space + style + null)
    snprintf(full_name, sizeof(full_name), "%s %s", g_bin_font.family_name, g_bin_font.style_name);
    return full_name;
}

// 获取字体族名
const char *get_font_family_name()
{
    return g_bin_font.family_name;
}

// 获取字体样式名
const char *get_font_style_name()
{
    return g_bin_font.style_name;
}

// 获取字体版本
uint8_t get_font_version()
{
    return g_bin_font.version;
}

// 获取字体文件中的基础字体大小
uint8_t get_font_size_from_file()
{
    return g_bin_font.font_size;
}

// 获取字体文件访问互斥锁（供外部模块使用）
SemaphoreHandle_t bin_font_get_file_mutex()
{
    extern SemaphoreHandle_t g_font_file_mutex;
    return g_font_file_mutex;
}

#include "text/line_handle.h"

void bin_font_set_cursor(int16_t x, int16_t y)
{
#if DBG_BIN_FONT_PRINT
    Serial.printf("[BIN_FONT] 设置光标: (%d,%d) -> (%d,%d)\n", g_cursor_x, g_cursor_y, x, y);
#endif
    g_cursor_x = x;
    g_cursor_y = y;
}

void bin_font_reset_cursor()
{
    g_cursor_x = 0;
    g_cursor_y = 0;
#if DBG_BIN_FONT_PRINT
    Serial.printf("[BIN_FONT] 重置光标位置: x=%d, y=%d\n", g_cursor_x, g_cursor_y);
#endif
}

void bin_font_flush_canvas(bool trans, bool invert, bool quality)
{
    if (g_canvas)
    {
        uint8_t pushType = DISPLAY_PUSH_MSG_TYPE_FLUSH;
        if (trans)
        {
            if (invert)
                pushType = DISPLAY_PUSH_MSG_TYPE_FLUSH_INVERT_TRANS;
            else
                pushType = DISPLAY_PUSH_MSG_TYPE_FLUSH_TRANS;
        }
        else if (quality)
        {
            pushType = DISPLAY_PUSH_MSG_TYPE_FLUSH_QUALITY;
        }
        // 先尝试克隆当前 canvas 并放入 canvas FIFO（阻塞直到有空位）
        // 重要：必须先 setColorDepth 再 createSprite，否则会触发二次分配/重建，导致明显卡顿。
        if (g_canvas)
        {
            M5Canvas *clone = new M5Canvas(&M5.Display);
            if (clone)
            {
                // 尽量使用 PSRAM，降低内部 RAM 压力（若底层不支持也不会影响编译）
                clone->setPsram(true);
                clone->setColorDepth(g_canvas->getColorDepth());
                clone->createSprite(PAPER_S3_WIDTH, PAPER_S3_HEIGHT);

                // 复制内部缓冲区
                void *src_buf = g_canvas->getBuffer();
                void *dst_buf = clone->getBuffer();
                size_t buf_len = g_canvas->bufferLength();
                if (src_buf && dst_buf && buf_len > 0)
                {
                    memcpy(dst_buf, src_buf, buf_len);
                    // 阻塞推入 FIFO，直到有空位（符合需求）
                    if (!enqueueCanvasCloneBlocking(clone))
                    {
                        delete clone;
                    }
                }
                else
                {
                    delete clone;
                }
            }
        }

        // 无论 clone 是否成功，都保留原有的信号队列行为（通知显示任务）
        if (!enqueueDisplayPush(pushType))
        {
#if DBG_BIN_FONT_PRINT
            Serial.println("[BIN_FONT] enqueueDisplayPush failed (queue not ready)");
#endif
        }
    }
}

void bin_font_clear_canvas(bool dark)
{
    if (g_canvas)
    {
        bin_font_reset_cursor();
        g_canvas->fillSprite(dark ? TFT_BLACK : TFT_WHITE);
#if DBG_BIN_FONT_PRINT
        Serial.println("[BIN_FONT] Canvas已清空(仅内存)");
#endif
    }
    else
    {
#if DBG_BIN_FONT_PRINT
        Serial.println("[BIN_FONT] 错误: Canvas尚未初始化！应在main.cpp中创建。");
#endif
    }
}

int16_t bin_font_get_cursor_y()
{
    if (g_canvas)
    {
        return g_canvas->getCursorY();
    }
    return 0;
}

// Note: font_size & Color only available for non-fast mode, and can't handle right line break, only for short text fine controling
void bin_font_print(const std::string &text, uint8_t font_size, uint8_t color, int16_t area_width, int16_t margin_left, int16_t margin_top, bool fast_mode, M5Canvas *canvas, TextAlign text_align, int16_t max_length, bool skipConv, bool drawBottom, bool vertical, bool dark)
{
    // 确定要使用的canvas
    M5Canvas *target_canvas = canvas ? canvas : g_canvas;

    // Workaround for current 3-step grey display: Only for V3!
    if (color != TFT_BLACK && g_bin_font.version == 3)
        dark = true;

    // 计算缩放比例：如果font_size为0，使用原始大小；否则根据字体文件的基础大小计算比例
    float scale_factor = 1.0f;
    if (font_size > 0)
    {
        scale_factor = (float)font_size / (float)g_bin_font.font_size;
    }

    // 缓存命中统计（每次渲染重置）
    uint32_t cache_hits = 0;
    uint32_t cache_misses = 0;
    bool cache_initialized = g_font_buffer_manager.isInitialized();
    g_font_buffer_manager.resetStats();

#if DBG_BIN_FONT_PRINT
    unsigned long printStartTime = millis();
    Serial.printf("[BIN_FONT] 开始渲染文本: \"%s\" (长度=%u, 字体大小=%d->缩放=%.2f, 颜色=%d, canvas=%s, 对齐=%d, 最大长度=%d)\n",
                  text.c_str(), (unsigned int)text.length(), font_size, scale_factor, color,
                  canvas ? "自定义" : "全局g_canvas", text_align, max_length);
#endif
    // 限制参数范围
    if (scale_factor < PAPERS3_SCALE_MIN)
        scale_factor = PAPERS3_SCALE_MIN;
    if (scale_factor > PAPERS3_SCALE_MAX)
        scale_factor = PAPERS3_SCALE_MAX;
    if (color > 15)
        color = 15;

    g_screen_width = area_width;
    g_margin_left = margin_left;
    g_margin_top = margin_top;

    // 计算缩放后的行距
    int16_t scaled_line_height = (int16_t)(g_line_height * scale_factor);

    // 垂直文本模式的变量
    bool is_vertical = vertical;
    int16_t column_spacing = scaled_line_height; // 列间距，等于行距

    // 在垂直模式下交换宽度和高度的概念
    // 使用固定的屏幕高度作为垂直渲染的高度参数
    int16_t screen_height = PAPER_S3_HEIGHT;
    int16_t effective_width = is_vertical ? screen_height : area_width;  // 垂直模式下用高度作为"宽度"
    int16_t effective_height = is_vertical ? area_width : screen_height; // 垂直模式下用宽度作为"高度"
    int16_t effective_margin_left = is_vertical ? 540 - margin_top - VERTICAL_RIGHT_DELTA : margin_left;
    int16_t effective_margin_top = is_vertical ? margin_left - VERTICAL_TOP_DELTA : margin_top;

    // 垂直模式下从右边开始的列位置
    // 在垂直模式下，第一列应该从设备真实宽度 - margin_right - column_width 开始
    int16_t char_width = (int16_t)(g_bin_font.font_size * scale_factor); // 字符宽度
    (void)char_width;
    int16_t current_column_x;

    if (is_vertical)
    {
        // 垂直模式：从设备真实宽度的右边开始，预留字符宽度的空间
        // 使用设备真实宽度540，而不是文本断行的"有效宽度"960
        current_column_x = effective_margin_left; // 540是设备真实宽度
    }
    else
    {
        // 水平模式：正常逻辑
        current_column_x = area_width - margin_left;
    }

#if DBG_BIN_FONT_PRINT
    if (is_vertical)
    {
        Serial.printf("[BIN_FONT] 垂直文本模式，列间距=%d, 起始列x=%d, 有效宽度=%d, 有效高度=%d\n",
                      column_spacing, current_column_x, effective_width, effective_height);

        Serial.printf("[BIN_FONT] 原始参数: area_width=%d, screen_height=%d\n", area_width, screen_height);
        Serial.printf("[BIN_FONT] 字符宽度=%d, 设备真实宽度=540, MARGIN_RIGHT=%d, 计算的起始列=%d\n",
                      char_width, MARGIN_RIGHT, current_column_x);
        Serial.printf("[BIN_FONT] 边距转换: margin_left=%d->effective_margin_top=%d, margin_top=%d->effective_margin_left=%d\n",
                      margin_left, effective_margin_top, margin_top, effective_margin_left);
    }
#endif

    (void)target_canvas; // silence unused warnings when target_canvas not used in some builds
#if DBG_BIN_FONT_PRINT
    Serial.printf("[BIN_FONT] 绘制模式: %s (缩放=%.2f, 颜色=%d)\n", fast_mode ? "快速模式" : "透明背景模式", scale_factor, color);
    unsigned long setupEndTime = millis(); // 计时点：setup完成
    g_total_glyph_read_us = 0;             // 重置累计读取时间
    Serial.printf("[TIMING] setup 耗时: %lu ms\n", setupEndTime - printStartTime);
#endif

    int16_t y = g_margin_top;
    int line_count = 0;
    int char_count = 0; // 计数实际渲染的字符数

    // 计算16级灰度对应的RGB565颜色值
    // 0=黑色(0x0000), 15=白色(0xFFFF)
    uint16_t text_color = (color * 0x1111) | (color << 12) | ((color & 0xF) << 7) | ((color & 0xF) << 1);
    if (color == 0)
        text_color = dark ? 0xFFFF : 0x0000; // 纯黑
    if (color == 15)
        text_color = dark ? 0x0000 : 0xFFFF; // 纯白

    // 如果设置了最大长度限制，则截断文本
    std::string display_text = text;
    bool text_truncated = false;
    // 应用繁简转换：如果当前有打开的书并且其书签配置 keepOrg == false，则忽略全局 zh_conv 设置（不做转换）
    bool apply_conversion = (g_config.zh_conv_mode != 0);
    // skipConv 参数可强制跳过转换
    if (skipConv)
    {
        apply_conversion = false;
        // 空白格替换
        display_text = zh_conv_utf8(display_text, 0);
    }
    if (apply_conversion)
    {
        display_text = zh_conv_utf8(display_text, g_config.zh_conv_mode);
    }
    if (max_length > 0 && !is_vertical) // 在垂直模式下跳过水平长度限制
    {
        // 找到第一个换行符位置，如果有的话
        size_t first_newline = display_text.find('\n');
        if (first_newline != std::string::npos)
        {
            display_text = display_text.substr(0, first_newline); // 只保留第一行
        }

        // 计算省略号的宽度 实际仅仅会在特殊手写出现
        const BinFontChar *ellipsis_glyph = find_char(0x2026); // Unicode的省略号…
        int16_t ellipsis_width = 0;
        if (ellipsis_glyph)
        {
            ellipsis_width = (int16_t)(ellipsis_glyph->width * scale_factor);
        }
        else
        {
            // 如果没有省略号字符，使用三个点的宽度
            const BinFontChar *dot_glyph = find_char('.');
            if (dot_glyph)
            {
                ellipsis_width = (int16_t)(dot_glyph->width * scale_factor * 3);
            }
            else
            {
                ellipsis_width = (int16_t)(g_bin_font.font_size * scale_factor / 2 * 3);
            }
        }

        // 计算在限制长度内能显示的字符数（为省略号预留空间）
        size_t char_end = 0;
        int16_t current_width = 0;
        int16_t available_width = max_length - ellipsis_width;
        const uint8_t *utf8 = (const uint8_t *)display_text.c_str();
        const uint8_t *end = (const uint8_t *)display_text.c_str() + display_text.length();

        while (utf8 < end)
        {
            const uint8_t *prev_utf8 = utf8;
            uint32_t unicode = utf8_decode(utf8, end);
            if (unicode == 0)
                break;

            const BinFontChar *glyph = find_char((uint16_t)unicode);
            int16_t char_width = glyph ? glyph->width : (g_bin_font.font_size / 2);
            char_width = (int16_t)(char_width * scale_factor);

            if (current_width + char_width > available_width)
            {
                char_end = prev_utf8 - (const uint8_t *)display_text.c_str();
                text_truncated = true;
                break;
            }

            current_width += char_width;
            char_end = utf8 - (const uint8_t *)display_text.c_str();
        }

        if (char_end > 0 && char_end < display_text.length())
        {
            display_text = display_text.substr(0, char_end);
            text_truncated = true;
#if DBG_BIN_FONT_PRINT
            Serial.printf("[BIN_FONT] 长度限制: 原文本长度=%zu, 截断后长度=%zu, 显示宽度=%d/%d\n",
                          text.length(), display_text.length(), current_width, available_width);
#endif
        }

        // 如果文本被截断，添加省略号
        if (text_truncated)
        {
            if (ellipsis_glyph)
            {
                display_text += "…";
            }
            else
            {
                display_text += "...";
            }
#if DBG_BIN_FONT_PRINT
            Serial.printf("[BIN_FONT] 文本截断，添加省略号: %s\n", display_text.c_str());
#endif
        }
    }

    // 按照处理后的文本进行渲染
    size_t line_start = 0;

    // 垂直文本模式：从右到左，从上到下
    if (is_vertical)
    {
        // 垂直文本渲染逻辑
        int16_t x = current_column_x;     // 从右边开始
        int16_t y = effective_margin_top; // 使用交换后的边距

        // 垂直模式下的文本对齐支持
        if (text_align != TEXT_ALIGN_LEFT)
        {
            // 计算文本总高度用于对齐
            int16_t total_text_height = 0;
            const uint8_t *temp_utf8 = (const uint8_t *)display_text.c_str();
            const uint8_t *temp_end = temp_utf8 + display_text.length();

            while (temp_utf8 < temp_end)
            {
                uint32_t temp_unicode = utf8_decode(temp_utf8, temp_end);
                if (temp_unicode == 0)
                    break;
                if (temp_unicode == '\n')
                    continue; // 跳过换行符计算

                const BinFontChar *temp_glyph = find_char(temp_unicode);
                if (temp_glyph && temp_glyph->bitmap_size > 0)
                {
                    // 竖排模式下字符间距保持固定值，不随字体缩放
                    // 标点符号旋转后使用宽度作为高度
                    int16_t char_height = is_chinese_punctuation(temp_unicode) ? temp_glyph->bitmapW : temp_glyph->bitmapH;
                    total_text_height += (int16_t)(char_height * scale_factor) + CHAR_SPACING_VERTICAL;
                }
            }

            // 根据对齐方式调整起始y位置
            switch (text_align)
            {
            case TEXT_ALIGN_CENTER:
                y = effective_margin_top + (effective_height - total_text_height) / 2;
                break;
            case TEXT_ALIGN_RIGHT: // 在垂直模式下，RIGHT对应底部对齐
                y = effective_height - total_text_height - effective_margin_top;
                break;
            default:
                break;
            }

            // 确保y不会超出边界
            if (y < effective_margin_top)
                y = effective_margin_top;
        }

        // 抑制未使用变量的警告
        (void)effective_width;

        const uint8_t *utf8 = (const uint8_t *)display_text.c_str();
        const uint8_t *text_end = utf8 + display_text.length();

        int16_t column_start_y = effective_margin_top; // 记录当前列的开始y位置

        while (utf8 < text_end)
        {
            // SPIFFS 文件操作很慢，每个字符后都喂狗
            esp_task_wdt_reset();

            uint32_t unicode = utf8_decode(utf8, text_end);
            if (unicode == 0)
                break;

            // 竖排模式下，替换引号为中文引号
            if (unicode == 0x201C || unicode == 0x201D) // " "
            {
                unicode = (unicode == 0x201C) ? 0x300E : 0x300F; // 替换为『』
            }
            else if (unicode == 0x2018 || unicode == 0x2019) // ' '
            {
                unicode = (unicode == 0x2018) ? 0x300C : 0x300D; // 替换为「」
            }

            // 处理换行：垂直模式下换行意味着新列
            if (unicode == '\n')
            {
                // 在换列之前，绘制当前列的下划线
                // NOTE: include empty columns (y == column_start_y) so that an explicit
                // newline (空行) still gets an underline just like non-empty columns.
                if (drawBottom && target_canvas && y >= column_start_y)
                {
                    int16_t line_x = x - column_spacing / 2; // 在列的左侧绘制
                    target_canvas->drawFastVLine(line_x - LINE_MARGIN / 2 - 2, 20, 920, TFT_DARKGREY);
                }

                x -= column_spacing;                   // 向左移一列
                y = effective_margin_top;              // 重置到顶部
                column_start_y = effective_margin_top; // 重置列开始位置
                continue;
            }

            const BinFontChar *glyph_ptr = find_char(unicode);
            if (!glyph_ptr || glyph_ptr->bitmap_size == 0)
            {
                // 对于缺失的字符，也要添加字符间距以保持一致性
                int16_t char_spacing = CHAR_SPACING_VERTICAL;
                y += (int16_t)(g_bin_font.font_size * scale_factor / 2) + char_spacing;
                continue;
            }

            // ⚠️ 关键：立即复制 glyph 数据到栈，避免后续 find_char 调用覆盖任务局部存储
            BinFontChar glyph_copy = *glyph_ptr;
            const BinFontChar *glyph = &glyph_copy;

            char_count++; // 记录实际渲染的字符

            // 不要在渲染阶段再次判断换列！
            // read_text_page 已经处理了正确的断行，我们只需要按照换行符来换列

            // 渲染字符（简化版本，类似于水平模式的逻辑）
            // 使用任务局部内存池（避免并发访问冲突）
            MemoryPool *task_pool = MemoryPool::get_task_pool();
            uint8_t *raw_data = task_pool->get_raw_buffer(glyph->bitmap_size);
            uint16_t *char_bitmap = nullptr;
            bool bitmap_loaded = false;
            if (raw_data)
            {
                // 优先从缓存加载（仅SD卡字体）
                if (!g_using_progmem_font && g_font_buffer_manager.isInitialized())
                {
                    const uint8_t *cached_bitmap = g_font_buffer_manager.getCharBitmap((uint16_t)unicode);
                    if (cached_bitmap)
                    {
                        memcpy(raw_data, cached_bitmap, glyph->bitmap_size);
                        bitmap_loaded = true;
                        cache_hits++;
#if DBG_BIN_FONT_PRINT
                        static int cache_log_count = 0;
                        if (cache_log_count < 8)
                        {
                            Serial.printf("[BIN_FONT] Cache HIT (vert) U+%04X page=%u\n", (unsigned)unicode, g_font_buffer_manager.getCurrentPageIndex());
                            cache_log_count++;
                        }
#endif
                    }
                    else
                    {
                        cache_misses++;
#if DBG_BIN_FONT_PRINT
                        static int cache_miss_log_count = 0;
                        if (cache_miss_log_count < 8)
                        {
                            Serial.printf("[BIN_FONT] Cache MISS (vert) U+%04X\n", (unsigned)unicode);
                            cache_miss_log_count++;
                        }
#endif
                    }
                }

                // 缓存未命中，从SD/PROGMEM加载
                if (!bitmap_loaded)
                {
                    bitmap_loaded = load_glyph_bitmap_smart(glyph->bitmap_offset, raw_data, glyph->bitmap_size);
                }

                char_bitmap = g_memory_pool.get_bitmap_buffer(glyph->bitmapW * glyph->bitmapH);
                if (char_bitmap && bitmap_loaded)
                {
                    // 根据字体版本和格式选择解码器
                    if (g_bin_font.version == 3)
                    {
                        // V3字体: 2bit Huffman编码
                        FontDecoder::decode_bitmap_v3(raw_data, glyph->bitmap_size, char_bitmap,
                                                      glyph->bitmapW, glyph->bitmapH, dark, false);
                    }
                    else if (g_bin_font.format == FONT_FORMAT_1BIT)
                    {
                        // V2字体: 1bit格式
                        FontDecoder::decode_bitmap_1bit(raw_data, glyph->bitmap_size, char_bitmap,
                                                        glyph->bitmapW, glyph->bitmapH);
                    }
                    else
                    {
                        // V2字体: Huffman格式（旧版）
                        FontDecoder::decode_bitmap(raw_data, glyph->bitmap_size, char_bitmap,
                                                   glyph->bitmapW, glyph->bitmapH);
                    }

                    // 竖排模式下，对中文标点符号进行90度顺时针旋转
                    if (is_chinese_punctuation(unicode))
                    {
                        uint16_t *temp_bitmap = new uint16_t[glyph->bitmapW * glyph->bitmapH];
                        if (temp_bitmap)
                        {
                            memcpy(temp_bitmap, char_bitmap, glyph->bitmapW * glyph->bitmapH * sizeof(uint16_t));
                            rotate_bitmap_90_cw(temp_bitmap, char_bitmap, glyph->bitmapW, glyph->bitmapH);
                            delete[] temp_bitmap;

                            // 对「」『』进行水平镜像
                            if (needs_horizontal_flip(unicode))
                            {
                                // 注意：旋转后宽高已互换
                                flip_bitmap_horizontal(char_bitmap, glyph->bitmapH, glyph->bitmapW);
                            }
                        }
                    }
                }
                task_pool->release_raw_buffer();
            }

            // ⚠️ 关键修复：立即复制位图到独立缓冲区，避免在渲染期间池被复用导致数据损坏
            uint16_t *local_bitmap = nullptr;
            if (char_bitmap)
            {
                size_t bitmap_pixels = (size_t)glyph->bitmapW * (size_t)glyph->bitmapH;
                local_bitmap = new uint16_t[bitmap_pixels];
                if (local_bitmap)
                {
                    memcpy(local_bitmap, char_bitmap, bitmap_pixels * sizeof(uint16_t));
                }
                // 立即释放池缓冲，允许其他操作复用
                task_pool->release_bitmap_buffer();
                // 后续渲染使用 local_bitmap
                char_bitmap = local_bitmap;
            }

            if (char_bitmap && target_canvas)
            {
                // 竖排模式下，标点符号旋转后需要交换宽高
                int16_t render_width = glyph->bitmapW;
                int16_t render_height = glyph->bitmapH;
                bool is_rotated_punct = is_chinese_punctuation(unicode);
                if (is_rotated_punct)
                {
                    // 旋转后宽高互换
                    render_width = glyph->bitmapH;
                    render_height = glyph->bitmapW;
                }

                // 垂直模式下的字符渲染 - 确保同一列的字符在X轴上对齐
                int16_t scaled_width = (int16_t)(render_width * scale_factor);
                int16_t scaled_height = (int16_t)(render_height * scale_factor);
                (void)scaled_width;
                (void)scaled_height;
                (void)scaled_width;
                (void)scaled_height;

                // 关键修正：在竖排模式下，同一列的所有字符应该基于统一的X基准线对齐
                // 类似于横排模式下基于Y基线对齐的原理
                int16_t column_baseline_x = x - (int16_t)(g_bin_font.font_size * scale_factor); // 统一的列基准线

                // 基于列基准线和字符的水平对齐偏移来计算最终X坐标
                // 这样确保同一列的字符在X轴上对齐
                int16_t char_offset_x = (int16_t)(glyph->x_offset * scale_factor);
                int16_t canvas_x = column_baseline_x + char_offset_x; // 基于统一基准线对齐

                // 标点符号旋转后需要居中对齐：原来的Y轴中心变成新的X轴中心
                if (is_rotated_punct)
                {
                    // 原始字符的Y轴中心位置（相对于bitmapH）
                    int16_t orig_center_y = glyph->bitmapH / 2;
                    // 字体框的X轴中心位置
                    int16_t font_center_x = (int16_t)(g_bin_font.font_size * scale_factor) / 2;
                    // 旋转后，原Y中心变成新X中心，调整offset让它对齐字体框中心
                    int16_t center_offset = font_center_x - (int16_t)(orig_center_y * scale_factor);
                    canvas_x = column_baseline_x + center_offset;
                }

                if (needs_minor_shift(unicode))
                {
                    // float shift_f = static_cast<float>(render_width) * scale_factor * 1.0f;
                    float shift_f = g_bin_font.font_size * scale_factor * 0.6f;
                    int16_t shift_px = static_cast<int16_t>(std::lround(shift_f));
                    if (shift_px == 0 && render_width > 0)
                    {
                        shift_px = 1;
                    }
                    canvas_x += shift_px;
                }

                // Y坐标不应该包含y_offset，因为y_offset是用于微调字符基线的
                // 在竖排模式下，我们只需要简单的从上到下排列
                // 重要：在竖排模式下，y是在逻辑坐标系中的值，需要应用margin_top偏移
                int16_t canvas_y = y; // 补偿坐标转换差异

#if DBG_BIN_FONT_PRINT
                if (unicode >= 0x4E00 && unicode <= 0x9FFF)
                { // 仅对中文字符打印调试信息
                    Serial.printf("[VERTICAL_ALIGN] 字符U+%04X: x=%d, baseline_x=%d, char_offset_x=%d, canvas_x=%d, y=%d, canvas_y=%d, margin_top=%d, margin_left=%d\n",
                                  unicode, x, column_baseline_x, char_offset_x, canvas_x, y, canvas_y, margin_top, margin_left);
                }
#endif

                // 使用现有的渲染逻辑，参考水平模式的实现
                if (fast_mode)
                {
                    M5.Display.setColorDepth(TEXT_COLORDEPTH);
                    if (scale_factor == 1.0f)
                    {
                        // 无缩放：直接使用pushImage
                        size_t pixels = (size_t)render_width * (size_t)render_height;

                        if (g_bin_font.version == 3)
                        {
                            // V3字体：解码后已经是正确的颜色（包括灰度），直接渲染
                            target_canvas->pushImage(canvas_x, canvas_y, render_width, render_height, char_bitmap);
                        }
                        else
                        {
                            // V2字体：需要根据前景/背景转换颜色
                            // ⚠️ 修复：不能复用内存池！char_bitmap 还在使用中！
                            uint16_t *rgb_buf = new uint16_t[pixels];
                            if (rgb_buf)
                            {
                                // 填充RGB565缓冲
                                for (size_t i = 0; i < pixels; ++i)
                                {
                                    uint16_t p = char_bitmap[i];
                                    rgb_buf[i] = (p != 0xFFFF) ? text_color : dark ? 0x0000
                                                                                   : 0xFFFF;
                                }
                                target_canvas->pushImage(canvas_x, canvas_y, render_width, render_height, rgb_buf);
                                delete[] rgb_buf;
                            }
                        }
                    }
                    else
                    {
                        // 缩放版本：需要处理颜色和缩放
                        if (g_bin_font.version == 3)
                        {
                            // V3字体：使用灰度感知缩放算法保持抗锯齿效果
                            render_v3_scaled(target_canvas, char_bitmap,
                                             render_width, render_height,
                                             scaled_width, scaled_height,
                                             canvas_x, canvas_y,
                                             scale_factor, dark);
                        }
                        else
                        {
                            // V2字体：使用像素级绘制来处理颜色和缩放
                            for (int16_t sy = 0; sy < scaled_height; sy++)
                            {
                                for (int16_t sx = 0; sx < scaled_width; sx++)
                                {
                                    int16_t orig_x = (int16_t)(sx / scale_factor);
                                    int16_t orig_y = (int16_t)(sy / scale_factor);

                                    if (orig_x < render_width && orig_y < render_height)
                                    {
                                        uint16_t pixel = char_bitmap[orig_y * render_width + orig_x];
                                        if (pixel != 0xFFFF)
                                        {
                                            target_canvas->drawPixel(canvas_x + sx, canvas_y + sy, text_color);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                else
                {
                    // 质量模式：需要正确处理缩放
                    if (scale_factor == 1.0f)
                    {
                        // 无缩放：使用原始尺寸
                        for (int16_t py = 0; py < render_height; py++)
                        {
                            for (int16_t px = 0; px < render_width; px++)
                            {
                                uint16_t pixel = char_bitmap[py * render_width + px];

                                if (g_bin_font.version == 3)
                                {
                                    // V3字体：直接使用解码后的颜色
                                    uint16_t bg_color = FontColorMapper::get_background_color(dark);
                                    if (pixel != bg_color)
                                    {
                                        target_canvas->drawPixel(canvas_x + px, canvas_y + py, pixel);
                                    }
                                }
                                else
                                {
                                    // V2字体
                                    if (pixel != 0xFFFF)
                                    {
                                        target_canvas->drawPixel(canvas_x + px, canvas_y + py, text_color);
                                    }
                                }
                            }
                        }
                    }
                    else
                    {
                        // 有缩放：使用缩放后的尺寸
                        if (g_bin_font.version == 3)
                        {
                            // V3字体：使用灰度感知缩放
                            render_v3_scaled(target_canvas, char_bitmap,
                                             render_width, render_height,
                                             scaled_width, scaled_height,
                                             canvas_x, canvas_y,
                                             scale_factor, dark);
                        }
                        else
                        {
                            // V2字体：像素级绘制
                            for (int16_t sy = 0; sy < scaled_height; sy++)
                            {
                                for (int16_t sx = 0; sx < scaled_width; sx++)
                                {
                                    int16_t orig_x = (int16_t)(sx / scale_factor);
                                    int16_t orig_y = (int16_t)(sy / scale_factor);

                                    if (orig_x < render_width && orig_y < render_height)
                                    {
                                        uint16_t pixel = char_bitmap[orig_y * render_width + orig_x];
                                        if (pixel != 0xFFFF)
                                        {
                                            target_canvas->drawPixel(canvas_x + sx, canvas_y + sy, text_color);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // 清理本地位图缓冲
            if (local_bitmap)
            {
                delete[] local_bitmap;
                local_bitmap = nullptr;
                char_bitmap = nullptr;
            }

            // 向下移动到下一个字符位置
            // 在竖排模式下，使用与断行计算相同的字符间距
            int16_t char_spacing = CHAR_SPACING_VERTICAL; // 竖排模式使用垂直间距
            // 竖排模式下字符间距保持固定值，不随字体缩放（避免过于紧密）
            int16_t old_y = y;
            (void)old_y;

            // 标点符号旋转后使用宽度作为高度
            int16_t char_height = is_chinese_punctuation(unicode) ? glyph->bitmapW : glyph->bitmapH;
            y += (int16_t)(char_height * scale_factor) + char_spacing;

            // 垂直模式下的max_length支持：限制垂直方向的字符数量
            if (max_length > 0)
            {
                static int char_count_in_column = 0;
                char_count_in_column++;

                // 如果达到最大长度，停止渲染
                if (char_count_in_column >= max_length)
                {
                    break;
                }

                // 换列时重置计数器
                if (unicode == '\n')
                {
                    char_count_in_column = 0;
                }
            }

#if DBG_BIN_FONT_PRINT
            if (unicode >= 0x4E00 && unicode <= 0x9FFF)
            { // 仅对中文字符打印调试信息
                Serial.printf("[Y_UPDATE] 字符U+%04X: old_y=%d, char_height=%d, spacing=%d, new_y=%d\n",
                              unicode, old_y, (int16_t)(char_height * scale_factor), char_spacing, y);
            }
#endif
        }

        // 渲染完成后，为最后一列绘制下划线
        // 同样包含空列的情况（y == column_start_y）
        if (drawBottom && target_canvas && y >= column_start_y)
        {
            int16_t line_x = x - column_spacing / 2; // 在列的左侧绘制
            target_canvas->drawFastVLine(line_x - LINE_MARGIN / 2 - 2, 20, 920, TFT_DARKGREY);
        }

        // 垂直模式渲染完成，更新光标位置
        // 在垂直模式下，x表示当前列，y表示当前列中的行位置
        g_cursor_x = x; // 当前列的x坐标
        g_cursor_y = y; // 当前列中的y坐标
        return;
    }

    // 水平文本模式（原有逻辑）
    while (line_start < display_text.length())
    {
        size_t line_end = display_text.find('\n', line_start);
        if (line_end == std::string::npos)
        {
            line_end = display_text.length();
        }
        line_count++;

        // 计算当前行的起始x坐标
        int16_t x = g_margin_left;

        // 对于单行文本，根据对齐方式计算位置（适用于快速模式和质量模式）
        if (line_count == 1 && display_text.find('\n') == std::string::npos)
        {
            // 计算当前行的宽度
            int16_t line_width = calculate_text_width(display_text, line_start, line_end);
            line_width = (int16_t)(line_width * scale_factor); // 考虑缩放因子

            // 统一使用area_width作为对齐宽度（快速模式和质量模式都支持）
            int16_t align_width = area_width;

            switch (text_align)
            {
            case TEXT_ALIGN_LEFT:
                // 左对齐：margin_left作为左边距
                x = margin_left;
                break;
            case TEXT_ALIGN_CENTER:
                // 居中对齐：基于area_width计算居中位置，margin_left作为额外偏移
                x = (align_width - line_width) / 2 + margin_left;
                break;
            case TEXT_ALIGN_RIGHT:
                // 右对齐：基于area_width从右边开始，margin_left作为右边距
                x = align_width - line_width - margin_left;
                break;
            default:
                // 默认左对齐
                x = margin_left;
                break;
            }

#if DBG_BIN_FONT_PRINT
            const char *align_names[] = {"左对齐", "居中", "右对齐"};
            Serial.printf("[BIN_FONT] 单行文本对齐: %s, 模式=%s, align_width=%d, line_width=%d, margin=%d, final_x=%d\n",
                          align_names[text_align], fast_mode ? "快速" : "质量", align_width, line_width, margin_left, x);
#endif
        }

        const uint8_t *utf8 = (const uint8_t *)display_text.c_str() + line_start;
        const uint8_t *line_utf8_end = (const uint8_t *)display_text.c_str() + line_end;

        while (utf8 < line_utf8_end)
        {
            // SPIFFS 文件操作很慢，每个字符后都喂狗
            esp_task_wdt_reset();

            uint32_t unicode = utf8_decode(utf8, line_utf8_end);
            if (unicode == 0)
                break;
            if (unicode == '\n')
                continue;
            const BinFontChar *glyph_ptr = find_char(unicode);
            if (!glyph_ptr || glyph_ptr->bitmap_size == 0)
            {
                x += (int16_t)(g_bin_font.font_size * scale_factor / 2);
                continue;
            }

            // ⚠️ 关键：立即复制 glyph 数据到栈，避免后续 find_char 调用覆盖任务局部存储
            BinFontChar glyph_copy = *glyph_ptr;
            const BinFontChar *glyph = &glyph_copy;

            char_count++; // 记录实际渲染的字符

            // 使用任务局部内存池（避免并发访问冲突）
            MemoryPool *task_pool = MemoryPool::get_task_pool();
            uint8_t *raw_data = task_pool->get_raw_buffer(glyph->bitmap_size);
            uint16_t *char_bitmap = nullptr;
            bool bitmap_loaded = false;
            if (raw_data)
            {
                // 优先从缓存加载（仅SD卡字体）
                if (!g_using_progmem_font && g_font_buffer_manager.isInitialized())
                {
                    const uint8_t *cached_bitmap = g_font_buffer_manager.getCharBitmap((uint16_t)unicode);
                    if (cached_bitmap)
                    {
                        memcpy(raw_data, cached_bitmap, glyph->bitmap_size);
                        bitmap_loaded = true;
                        cache_hits++;
#if DBG_BIN_FONT_PRINT
                        static int cache_log_count2 = 0;
                        if (cache_log_count2 < 8)
                        {
                            Serial.printf("[BIN_FONT] Cache HIT (horiz) U+%04X page=%u\n", (unsigned)unicode, g_font_buffer_manager.getCurrentPageIndex());
                            cache_log_count2++;
                        }
#endif
                    }
                    else
                    {
                        cache_misses++;
#if DBG_BIN_FONT_PRINT
                        static int cache_miss_log_count2 = 0;
                        if (cache_miss_log_count2 < 8)
                        {
                            Serial.printf("[BIN_FONT] Cache MISS (horiz) U+%04X\n", (unsigned)unicode);
                            cache_miss_log_count2++;
                        }
#endif
                    }
                }

                // 缓存未命中，从SD/PROGMEM加载
                if (!bitmap_loaded)
                {
                    bitmap_loaded = load_glyph_bitmap_smart(glyph->bitmap_offset, raw_data, glyph->bitmap_size);
                }

                char_bitmap = g_memory_pool.get_bitmap_buffer(glyph->bitmapW * glyph->bitmapH);
                if (char_bitmap && bitmap_loaded)
                {
                    // 根据字体版本和格式选择解码器
                    if (g_bin_font.version == 3)
                    {
                        // V3字体: 2bit Huffman编码
                        FontDecoder::decode_bitmap_v3(raw_data, glyph->bitmap_size, char_bitmap,
                                                      glyph->bitmapW, glyph->bitmapH, dark, false);
                    }
                    else if (g_bin_font.format == FONT_FORMAT_1BIT)
                    {
                        // V2字体: 1bit格式
                        FontDecoder::decode_bitmap_1bit(raw_data, glyph->bitmap_size, char_bitmap,
                                                        glyph->bitmapW, glyph->bitmapH);
                    }
                    else
                    {
                        // V2字体: Huffman格式（旧版）
                        FontDecoder::decode_bitmap(raw_data, glyph->bitmap_size, char_bitmap,
                                                   glyph->bitmapW, glyph->bitmapH);
                    }
                }
                task_pool->release_raw_buffer();
            }

            // ⚠️ 关键修复：立即复制位图到独立缓冲区，避免在渲染期间池被复用导致数据损坏
            uint16_t *local_bitmap = nullptr;
            if (char_bitmap)
            {
                size_t bitmap_pixels = (size_t)glyph->bitmapW * (size_t)glyph->bitmapH;
                local_bitmap = new uint16_t[bitmap_pixels];
                if (local_bitmap)
                {
                    memcpy(local_bitmap, char_bitmap, bitmap_pixels * sizeof(uint16_t));
                }
                // 立即释放池缓冲，允许其他操作复用
                task_pool->release_bitmap_buffer();
                // 后续渲染使用 local_bitmap
                char_bitmap = local_bitmap;
            }

            if (char_bitmap && target_canvas)
            {
                if (fast_mode)
                {
                    // 开快刷！
                    M5.Display.setColorDepth(TEXT_COLORDEPTH); // 使用快速模式

                    // 计算缩放后的位置和大小
                    int16_t scaled_width = (int16_t)(glyph->bitmapW * scale_factor);
                    int16_t scaled_height = (int16_t)(glyph->bitmapH * scale_factor);
                    int16_t canvas_x = x + (int16_t)(glyph->x_offset * scale_factor);
                    int16_t canvas_y = y + (int16_t)(glyph->y_offset * scale_factor);

                    if (scale_factor == 1.0f)
                    {
                        // 无缩放：使用原始的高性能路径
                        size_t pixels = (size_t)glyph->bitmapW * (size_t)glyph->bitmapH;

                        if (g_bin_font.version == 3)
                        {
                            // V3字体：解码后已经是正确的颜色，直接渲染
                            target_canvas->pushImage(canvas_x, canvas_y, glyph->bitmapW, glyph->bitmapH, char_bitmap);
                        }
                        else
                        {
                            // V2字体：需要转换颜色
                            uint16_t *rgb_buf = nullptr;

                            // ⚠️ 修复：不能复用内存池！char_bitmap 还在使用中！
                            // 直接使用 heap_caps_malloc 或 new
                            rgb_buf = (uint16_t *)heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
                            if (!rgb_buf)
                            {
                                // PSRAM 分配失败，尝试内部 SRAM
                                rgb_buf = new uint16_t[pixels];
                            }

                            if (rgb_buf)
                            {
                                // 填充 RGB565 缓冲：使用指定颜色或白色
                                for (size_t i = 0; i < pixels; ++i)
                                {
                                    uint16_t p = char_bitmap[i];
                                    rgb_buf[i] = (p != 0xFFFF) ? text_color : dark ? 0x0000
                                                                                   : 0xFFFF;
                                }

                                // 一次性推送到 Canvas
                                target_canvas->pushImage(canvas_x, canvas_y, glyph->bitmapW, glyph->bitmapH, rgb_buf);

                                // 释放缓冲
                                if (esp_ptr_external_ram(rgb_buf))
                                {
                                    heap_caps_free(rgb_buf);
                                }
                                else
                                {
                                    delete[] rgb_buf;
                                }
                            }
                            else
                            {
                                // 分配失败：回退到逐像素绘制以保证稳健性
                                for (int16_t py = 0; py < glyph->bitmapH; py++)
                                {
                                    for (int16_t px = 0; px < glyph->bitmapW; px++)
                                    {
                                        uint16_t pixel = char_bitmap[py * glyph->bitmapW + px];
                                        if (pixel != 0xFFFF)
                                        {
                                            target_canvas->drawPixel(canvas_x + px, canvas_y + py, text_color);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    else
                    {
                        // 需要缩放：使用优化的二值图像算法（快速模式专用）
                        if (scale_factor >= 1.0f)
                        {
                            // 放大：使用简化的最近邻算法，仅在关键位置进行覆盖率判断
                            int16_t step = (scale_factor > 1.5f) ? 1 : 2; // 大倍数缩放用更精细的步长

                            for (int16_t sy = 0; sy < scaled_height; sy += step)
                            {
                                for (int16_t sx = 0; sx < scaled_width; sx += step)
                                {
                                    // 简化的最近邻映射
                                    int16_t orig_x = (int16_t)((sx + 0.5f) / scale_factor);
                                    int16_t orig_y = (int16_t)((sy + 0.5f) / scale_factor);

                                    // 边界检查
                                    if (orig_x < 0 || orig_y < 0 || orig_x >= glyph->bitmapW || orig_y >= glyph->bitmapH)
                                        continue;

                                    uint16_t pixel = char_bitmap[orig_y * glyph->bitmapW + orig_x];
                                    if (pixel != 0xFFFF)
                                    {
                                        // 绘制方块而不是单像素，提升性能
                                        int16_t block_size = step;
                                        for (int16_t by = 0; by < block_size && (sy + by) < scaled_height; by++)
                                        {
                                            for (int16_t bx = 0; bx < block_size && (sx + bx) < scaled_width; bx++)
                                            {
                                                target_canvas->drawPixel(canvas_x + sx + bx, canvas_y + sy + by, text_color);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        else
                        {
                            // 缩小：使用防粘连的精细抽样算法
                            float inv_scale = 1.0f / scale_factor;

                            for (int16_t sy = 0; sy < scaled_height; sy++)
                            {
                                for (int16_t sx = 0; sx < scaled_width; sx++)
                                {
                                    // 计算原图对应区域的中心点
                                    float orig_x_center = (sx + 0.5f) * inv_scale - 0.5f;
                                    float orig_y_center = (sy + 0.5f) * inv_scale - 0.5f;

                                    // 动态抽样策略：小字体用最精细的抽样防止粘连
                                    int16_t sample_step = 1; // 统一使用精细抽样

                                    bool should_draw = false;
                                    (void)should_draw;
                                    int16_t samples = 0;
                                    int16_t black_samples = 0;
                                    int16_t center_black_samples = 0; // 中心区域的黑色样本

                                    // 抽样范围：较小范围防止跨笔画抽样
                                    int16_t sample_range = (int16_t)(inv_scale * 0.5f); // 减小抽样范围
                                    sample_range = (sample_range < 1) ? 1 : sample_range;
                                    // 增大采样上限以在较大缩小比例下保留更多细节（成本略增）
                                    if (sample_range > PAPERS3_SAMPLE_RANGE_MAX)
                                        sample_range = PAPERS3_SAMPLE_RANGE_MAX; // 限制最大范围

                                    // 中心权重检测：优先检测中心点
                                    int16_t center_x = (int16_t)(orig_x_center + 0.5f);
                                    int16_t center_y = (int16_t)(orig_y_center + 0.5f);
                                    bool center_is_black = false;

                                    if (center_x >= 0 && center_y >= 0 &&
                                        center_x < glyph->bitmapW && center_y < glyph->bitmapH)
                                    {
                                        uint16_t center_pixel = char_bitmap[center_y * glyph->bitmapW + center_x];
                                        center_is_black = (center_pixel != 0xFFFF);
                                    }

                                    // 扩展抽样
                                    for (int16_t dy = -sample_range; dy <= sample_range; dy += sample_step)
                                    {
                                        for (int16_t dx = -sample_range; dx <= sample_range; dx += sample_step)
                                        {
                                            int16_t check_x = (int16_t)(orig_x_center + dx);
                                            int16_t check_y = (int16_t)(orig_y_center + dy);

                                            if (check_x >= 0 && check_y >= 0 &&
                                                check_x < glyph->bitmapW && check_y < glyph->bitmapH)
                                            {
                                                samples++;
                                                uint16_t pixel = char_bitmap[check_y * glyph->bitmapW + check_x];
                                                if (pixel != 0xFFFF)
                                                {
                                                    black_samples++;
                                                    // 如果是中心区域（距离中心点较近），增加权重
                                                    if (abs(dx) <= 1 && abs(dy) <= 1)
                                                    {
                                                        center_black_samples++;
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    // 防粘连的多层判断策略
                                    bool draw_pixel = false;

                                    if (scale_factor > 0.7f)
                                    {
                                        // 轻微缩小：中心点权重策略 + 高阈值
                                        if (center_is_black)
                                        {
                                            // 中心是黑色，需要周围也有足够支持
                                            draw_pixel = (center_black_samples >= 2) || (black_samples * 10 >= samples * 6); // 60%
                                        }
                                        else
                                        {
                                            // 中心不是黑色，需要更高的周围密度
                                            draw_pixel = (black_samples * 10 >= samples * 7); // 70%
                                        }
                                    }
                                    else if (scale_factor > 0.5f)
                                    {
                                        // 中度缩小：平衡策略
                                        if (center_is_black)
                                        {
                                            draw_pixel = (center_black_samples >= 1) || (black_samples * 10 >= samples * 5); // 50%
                                        }
                                        else
                                        {
                                            draw_pixel = (black_samples * 10 >= samples * 6); // 60%
                                        }
                                    }
                                    else if (scale_factor > 0.3f)
                                    {
                                        // 较大缩小：保证可见性但防止过粗
                                        if (center_is_black)
                                        {
                                            draw_pixel = true; // 中心黑色直接绘制
                                        }
                                        else
                                        {
                                            draw_pixel = (black_samples * 10 >= samples * 4); // 40%
                                        }
                                    }
                                    else
                                    {
                                        // 大幅缩小：确保基本可见性
                                        draw_pixel = (black_samples > 0); // 有黑色就绘制
                                    }

                                    if (draw_pixel && samples > 0)
                                    {
                                        target_canvas->drawPixel(canvas_x + sx, canvas_y + sy, text_color);
                                    }
                                }
                            }
                        }
                    }
                }
                else
                {
                    // 质量
                    // color 有效
                    M5.Display.setColorDepth(TEXT_COLORDEPTH_HIGH); // 16
                    // 透明背景或缩放路径：保留原先逻辑
                    // 计算缩放后的位置和大小
                    int16_t scaled_width = (int16_t)(glyph->bitmapW * scale_factor);
                    int16_t scaled_height = (int16_t)(glyph->bitmapH * scale_factor);
                    int16_t canvas_x = x + (int16_t)(glyph->x_offset * scale_factor);
                    int16_t canvas_y = y + (int16_t)(glyph->y_offset * scale_factor);

                    // 黑白二值化渲染：使用准确的灰度计算和门限判断
                    if (scale_factor == 1.0f)
                    {
                        // 无缩放
                        for (int16_t py = 0; py < glyph->bitmapH; py++)
                        {
                            for (int16_t px = 0; px < glyph->bitmapW; px++)
                            {
                                uint16_t pixel = char_bitmap[py * glyph->bitmapW + px];

                                if (g_bin_font.version == 3)
                                {
                                    // V3字体：直接使用解码后的颜色（包括灰度）
                                    uint16_t bg_color = FontColorMapper::get_background_color(dark);
                                    if (pixel != bg_color)
                                    {
                                        target_canvas->drawPixel(canvas_x + px, canvas_y + py, pixel);
                                    }
                                }
                                else
                                {
                                    // V2字体：二值化处理
                                    if (pixel != 0xFFFF)
                                    {
                                        target_canvas->drawPixel(canvas_x + px, canvas_y + py, text_color);
                                    }
                                }
                            }
                        }
                    }
                    else
                    {
                        // 有缩放：对V3字体使用灰度感知算法
                        if (g_bin_font.version == 3)
                        {
                            // V3字体：使用灰度感知缩放保持抗锯齿效果
                            render_v3_scaled(target_canvas, char_bitmap,
                                             glyph->bitmapW, glyph->bitmapH,
                                             scaled_width, scaled_height,
                                             canvas_x, canvas_y,
                                             scale_factor, dark);
                        }
                        else
                        {
                            // V2字体：使用二值图像算法
#if SCALING_ALGORITHM == 0
                            // 原始最近邻算法（快速但质量一般）
                            for (int16_t sy = 0; sy < scaled_height; sy++)
                            {
                                for (int16_t sx = 0; sx < scaled_width; sx++)
                                {
                                    float orig_x_f = (sx + 0.5f) / scale_factor - 0.5f;
                                    float orig_y_f = (sy + 0.5f) / scale_factor - 0.5f;
                                    int16_t orig_x = (int16_t)orig_x_f;
                                    int16_t orig_y = (int16_t)orig_y_f;
                                    if (orig_x < 0 || orig_y < 0 || orig_x >= glyph->bitmapW || orig_y >= glyph->bitmapH)
                                        continue;
                                    uint16_t pixel = char_bitmap[orig_y * glyph->bitmapW + orig_x];
                                    if (pixel != 0xFFFF)
                                    {
                                        target_canvas->drawPixel(canvas_x + sx, canvas_y + sy, text_color);
                                    }
                                }
                            }

#elif SCALING_ALGORITHM == 1
                            // 超采样算法（质量高但较慢）
                            for (int16_t sy = 0; sy < scaled_height; sy++)
                            {
                                for (int16_t sx = 0; sx < scaled_width; sx++)
                                {
                                    // 计算在原图中对应的区域
                                    float orig_x_start = sx / scale_factor;
                                    float orig_y_start = sy / scale_factor;
                                    float orig_x_end = (sx + 1) / scale_factor;
                                    float orig_y_end = (sy + 1) / scale_factor;

                                    // 边界检查
                                    int16_t x_min = (int16_t)fmaxf(0, floorf(orig_x_start));
                                    int16_t y_min = (int16_t)fmaxf(0, floorf(orig_y_start));
                                    int16_t x_max = (int16_t)fminf(glyph->bitmapW - 1, ceilf(orig_x_end));
                                    int16_t y_max = (int16_t)fminf(glyph->bitmapH - 1, ceilf(orig_y_end));

                                    if (x_min > x_max || y_min > y_max)
                                        continue;

                                    // 计算黑色像素的覆盖面积
                                    float total_weight = 0.0f;
                                    float weighted_gray = 0.0f;
                                    bool has_valid_pixel = false;
                                    bool is_edge_region = false; // 边缘检测

                                    for (int16_t oy = y_min; oy <= y_max; oy++)
                                    {
                                        for (int16_t ox = x_min; ox <= x_max; ox++)
                                        {
                                            // 计算重叠面积作为权重
                                            float overlap_x_start = fmaxf(orig_x_start, ox);
                                            float overlap_x_end = fminf(orig_x_end, ox + 1);
                                            float overlap_y_start = fmaxf(orig_y_start, oy);
                                            float overlap_y_end = fminf(orig_y_end, oy + 1);

                                            if (overlap_x_end > overlap_x_start && overlap_y_end > overlap_y_start)
                                            {
                                                float weight = (overlap_x_end - overlap_x_start) * (overlap_y_end - overlap_y_start);
                                                uint16_t pixel = char_bitmap[oy * glyph->bitmapW + ox];

                                                if (pixel != 0xFFFF) // 有效像素
                                                {
                                                    has_valid_pixel = true;
                                                    bool is_black = true;
                                                    float gray_normalized = is_black ? 0.0f : 1.0f; // 二值化：黑=0，白=1
                                                    weighted_gray += gray_normalized * weight;
                                                    total_weight += weight;

                                                    // 边缘检测：检查当前像素的邻域
                                                    if (!is_edge_region)
                                                    {
                                                        for (int dy = -1; dy <= 1; dy++)
                                                        {
                                                            for (int dx = -1; dx <= 1; dx++)
                                                            {
                                                                if (dx == 0 && dy == 0)
                                                                    continue;

                                                                int16_t nx = ox + dx;
                                                                int16_t ny = oy + dy;

                                                                if (nx >= 0 && nx < glyph->bitmapW && ny >= 0 && ny < glyph->bitmapH)
                                                                {
                                                                    uint16_t neighbor = char_bitmap[ny * glyph->bitmapW + nx];
                                                                    bool neighbor_black = (neighbor != 0xFFFF);

                                                                    if (is_black != neighbor_black)
                                                                    {
                                                                        is_edge_region = true;
                                                                        break;
                                                                    }
                                                                }
                                                            }
                                                            if (is_edge_region)
                                                                break;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    // 基于加权平均决定是否绘制（改为使用和缩小相同的覆盖率逻辑）
                                    if (has_valid_pixel && total_weight > 0.0f)
                                    {
                                        float avg_gray = weighted_gray / total_weight;
                                        // 转换为覆盖率：avg_gray越小（越黑），覆盖率越高
                                        float coverage_ratio = 1.0f - avg_gray;

                                        // 调整阈值策略：与 scale_factor 成正比，缩小时降低阈值以保留细节
                                        float base_threshold = 0.25f * fmaxf(0.5f, scale_factor);
                                        // 限制阈值范围由顶层宏控制
                                        base_threshold = fmaxf(PAPERS3_BASE_THRESHOLD_MIN, fminf(PAPERS3_BASE_THRESHOLD_MAX, base_threshold));

                                        float threshold;
                                        if (is_edge_region)
                                        {
                                            // 边缘区域：使用更宽松的阈值，减少毛刺
                                            threshold = base_threshold * 0.8f;

                                            // 渐变处理：在阈值附近使用平滑过渡
                                            float gradient_range = 0.1f;
                                            if (coverage_ratio > threshold - gradient_range &&
                                                coverage_ratio < threshold + gradient_range)
                                            {
                                                float gradient_factor = (coverage_ratio - (threshold - gradient_range)) / (2 * gradient_range);
                                                gradient_factor = fmaxf(0.0f, fminf(1.0f, gradient_factor));
                                                threshold = threshold - gradient_range + (gradient_factor * 2 * gradient_range);
                                            }
                                        }
                                        else
                                        {
                                            // 非边缘区域：使用标准阈值
                                            threshold = base_threshold;
                                        }

                                        if (coverage_ratio > threshold)
                                        {
                                            target_canvas->drawPixel(canvas_x + sx, canvas_y + sy, text_color);
                                        }
                                    }
                                }
                            }

#elif SCALING_ALGORITHM == 2
                            // 双线性插值算法（平衡质量和速度）
                            for (int16_t sy = 0; sy < scaled_height; sy++)
                            {
                                for (int16_t sx = 0; sx < scaled_width; sx++)
                                {
                                    // 计算原图坐标（中心对齐）
                                    float orig_x_f = (sx + 0.5f) / scale_factor - 0.5f;
                                    float orig_y_f = (sy + 0.5f) / scale_factor - 0.5f;

                                    // 获取四个相邻像素的坐标
                                    int16_t x0 = (int16_t)floorf(orig_x_f);
                                    int16_t y0 = (int16_t)floorf(orig_y_f);
                                    int16_t x1 = x0 + 1;
                                    int16_t y1 = y0 + 1;

                                    // 边界检查
                                    if (x0 < 0 || y0 < 0 || x1 >= glyph->bitmapW || y1 >= glyph->bitmapH)
                                    {
                                        // 边界处使用最近邻
                                        int16_t nx = (int16_t)(orig_x_f + 0.5f);
                                        int16_t ny = (int16_t)(orig_y_f + 0.5f);
                                        if (nx >= 0 && ny >= 0 && nx < glyph->bitmapW && ny < glyph->bitmapH)
                                        {
                                            uint16_t pixel = char_bitmap[ny * glyph->bitmapW + nx];
                                            if (pixel != 0xFFFF)
                                            {
                                                target_canvas->drawPixel(canvas_x + sx, canvas_y + sy, text_color);
                                            }
                                        }
                                        continue;
                                    }

                                    // 获取四个像素的灰度值
                                    uint16_t p00 = char_bitmap[y0 * glyph->bitmapW + x0];
                                    uint16_t p01 = char_bitmap[y0 * glyph->bitmapW + x1];
                                    uint16_t p10 = char_bitmap[y1 * glyph->bitmapW + x0];
                                    uint16_t p11 = char_bitmap[y1 * glyph->bitmapW + x1];

                                    // 检查是否有有效像素
                                    bool valid = (p00 != 0xFFFF) || (p01 != 0xFFFF) || (p10 != 0xFFFF) || (p11 != 0xFFFF);
                                    if (!valid)
                                        continue;

                                    // 将无效像素设为白色（15）
                                    uint8_t g00 = (p00 == 0xFFFF) ? 15 : (uint8_t)(p00 & 0x0F);
                                    uint8_t g01 = (p01 == 0xFFFF) ? 15 : (uint8_t)(p01 & 0x0F);
                                    uint8_t g10 = (p10 == 0xFFFF) ? 15 : (uint8_t)(p10 & 0x0F);
                                    uint8_t g11 = (p11 == 0xFFFF) ? 15 : (uint8_t)(p11 & 0x0F);

                                    // 双线性插值权重
                                    float wx = orig_x_f - x0;
                                    float wy = orig_y_f - y0;

                                    // 插值计算
                                    float gray_interp = (1 - wx) * (1 - wy) * g00 + wx * (1 - wy) * g01 + (1 - wx) * wy * g10 + wx * wy * g11;

                                    // 转换为0-1范围并应用阈值
                                    float gray_normalized = gray_interp / 15.0f;
                                    if (gray_normalized < 0.5f) // 阈值可以调整
                                    {
                                        target_canvas->drawPixel(canvas_x + sx, canvas_y + sy, text_color);
                                    }
                                }
                            }

#elif SCALING_ALGORITHM == 3
                            // 二值图像专用算法（最适合黑白字体）
                            if (scale_factor >= 1.0f)
                            {
                                // 放大：使用区域覆盖率判断
                                for (int16_t sy = 0; sy < scaled_height; sy++)
                                {
                                    for (int16_t sx = 0; sx < scaled_width; sx++)
                                    {
                                        // 计算目标像素对应的原图区域
                                        float orig_x_start = sx / scale_factor;
                                        float orig_y_start = sy / scale_factor;
                                        float orig_x_end = (sx + 1) / scale_factor;
                                        float orig_y_end = (sy + 1) / scale_factor;

                                        // 扩展到整数边界以包含所有相关像素
                                        int16_t x_min = (int16_t)floorf(orig_x_start);
                                        int16_t y_min = (int16_t)floorf(orig_y_start);
                                        int16_t x_max = (int16_t)ceilf(orig_x_end - 0.001f); // 减小epsilon避免边界问题
                                        int16_t y_max = (int16_t)ceilf(orig_y_end - 0.001f);

                                        // 边界检查
                                        x_min = (x_min < 0) ? 0 : x_min;
                                        y_min = (y_min < 0) ? 0 : y_min;
                                        x_max = (x_max >= glyph->bitmapW) ? glyph->bitmapW - 1 : x_max;
                                        y_max = (y_max >= glyph->bitmapH) ? glyph->bitmapH - 1 : y_max;

                                        if (x_min > x_max || y_min > y_max)
                                            continue;

                                        // 计算黑色像素的覆盖面积
                                        float black_coverage = 0.0f;
                                        float total_coverage = 0.0f;

                                        for (int16_t oy = y_min; oy <= y_max; oy++)
                                        {
                                            for (int16_t ox = x_min; ox <= x_max; ox++)
                                            {
                                                // 计算像素与目标区域的重叠面积
                                                float pixel_x_start = ox;
                                                float pixel_x_end = ox + 1;
                                                float pixel_y_start = oy;
                                                float pixel_y_end = oy + 1;

                                                float overlap_x_start = fmaxf(orig_x_start, pixel_x_start);
                                                float overlap_x_end = fminf(orig_x_end, pixel_x_end);
                                                float overlap_y_start = fmaxf(orig_y_start, pixel_y_start);
                                                float overlap_y_end = fminf(orig_y_end, pixel_y_end);

                                                if (overlap_x_end > overlap_x_start && overlap_y_end > overlap_y_start)
                                                {
                                                    float overlap_area = (overlap_x_end - overlap_x_start) * (overlap_y_end - overlap_y_start);
                                                    total_coverage += overlap_area;

                                                    uint16_t pixel = char_bitmap[oy * glyph->bitmapW + ox];
                                                    if (pixel != 0xFFFF)
                                                    {
                                                        black_coverage += overlap_area;
                                                    }
                                                }
                                            }
                                        }

                                        // 基于覆盖率决定：对于放大，如果黑色覆盖率超过阈值就绘制
                                        if (total_coverage > 0.0f)
                                        {
                                            float coverage_ratio = black_coverage / total_coverage;
                                            // 动态阈值：放大倍数越大，阈值越低，保持细节
                                            float threshold = 0.3f / fmaxf(1.0f, scale_factor * 0.5f);
                                            threshold = fmaxf(0.1f, fminf(0.5f, threshold));

                                            if (coverage_ratio > threshold)
                                            {
                                                target_canvas->drawPixel(canvas_x + sx, canvas_y + sy, text_color);
                                            }
                                        }
                                    }
                                }
                            }
                            else
                            {
                                // 缩小：使用覆盖率判断，避免笔画重叠
                                for (int16_t sy = 0; sy < scaled_height; sy++)
                                {
                                    for (int16_t sx = 0; sx < scaled_width; sx++)
                                    {
                                        // 计算目标像素对应的原图区域
                                        float orig_x_start = sx / scale_factor;
                                        float orig_y_start = sy / scale_factor;
                                        float orig_x_end = (sx + 1) / scale_factor;
                                        float orig_y_end = (sy + 1) / scale_factor;

                                        // 获取需要检查的原图像素范围
                                        int16_t x_min = (int16_t)floorf(orig_x_start);
                                        int16_t y_min = (int16_t)floorf(orig_y_start);
                                        int16_t x_max = (int16_t)ceilf(orig_x_end - 0.001f);
                                        int16_t y_max = (int16_t)ceilf(orig_y_end - 0.001f);

                                        // 边界检查
                                        x_min = (x_min < 0) ? 0 : x_min;
                                        y_min = (y_min < 0) ? 0 : y_min;
                                        x_max = (x_max >= glyph->bitmapW) ? glyph->bitmapW - 1 : x_max;
                                        y_max = (y_max >= glyph->bitmapH) ? glyph->bitmapH - 1 : y_max;

                                        if (x_min > x_max || y_min > y_max)
                                            continue;

                                        // 计算黑色像素的覆盖面积（精确计算）
                                        float black_coverage = 0.0f;
                                        float total_coverage = 0.0f;
                                        bool is_edge_region = false; // 检测是否为边缘区域

                                        for (int16_t oy = y_min; oy <= y_max; oy++)
                                        {
                                            for (int16_t ox = x_min; ox <= x_max; ox++)
                                            {
                                                // 计算像素与目标区域的重叠面积
                                                float pixel_x_start = ox;
                                                float pixel_x_end = ox + 1;
                                                float pixel_y_start = oy;
                                                float pixel_y_end = oy + 1;

                                                float overlap_x_start = fmaxf(orig_x_start, pixel_x_start);
                                                float overlap_x_end = fminf(orig_x_end, pixel_x_end);
                                                float overlap_y_start = fmaxf(orig_y_start, pixel_y_start);
                                                float overlap_y_end = fminf(orig_y_end, pixel_y_end);

                                                if (overlap_x_end > overlap_x_start && overlap_y_end > overlap_y_start)
                                                {
                                                    float overlap_area = (overlap_x_end - overlap_x_start) * (overlap_y_end - overlap_y_start);
                                                    total_coverage += overlap_area;

                                                    uint16_t pixel = char_bitmap[oy * glyph->bitmapW + ox];
                                                    bool is_black = (pixel != 0xFFFF);

                                                    if (is_black)
                                                    {
                                                        black_coverage += overlap_area;
                                                    }

                                                    // 边缘检测：检查当前像素的邻域
                                                    if (!is_edge_region)
                                                    {
                                                        // 检查8邻域中是否有黑白对比
                                                        for (int dy = -1; dy <= 1; dy++)
                                                        {
                                                            for (int dx = -1; dx <= 1; dx++)
                                                            {
                                                                if (dx == 0 && dy == 0)
                                                                    continue;

                                                                int16_t nx = ox + dx;
                                                                int16_t ny = oy + dy;

                                                                if (nx >= 0 && nx < glyph->bitmapW && ny >= 0 && ny < glyph->bitmapH)
                                                                {
                                                                    uint16_t neighbor = char_bitmap[ny * glyph->bitmapW + nx];
                                                                    bool neighbor_black = (neighbor != 0xFFFF);

                                                                    // 如果当前像素和邻居像素颜色不同，则是边缘
                                                                    if (is_black != neighbor_black)
                                                                    {
                                                                        is_edge_region = true;
                                                                        break;
                                                                    }
                                                                }
                                                            }
                                                            if (is_edge_region)
                                                                break;
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        // 基于覆盖率决定是否绘制，对边缘区域使用平滑处理
                                        if (total_coverage > 0.0f)
                                        {
                                            float coverage_ratio = black_coverage / total_coverage;

                                            // 调整阈值策略：与 scale_factor 成正比，缩小时降低阈值以保留细节
                                            float base_threshold = 0.25f * fmaxf(0.5f, scale_factor);
                                            base_threshold = fmaxf(PAPERS3_BASE_THRESHOLD_MIN, fminf(PAPERS3_BASE_THRESHOLD_MAX, base_threshold));

                                            float threshold;
                                            if (is_edge_region)
                                            {
                                                // 边缘区域：使用更宽松的阈值和渐变处理，减少毛刺
                                                threshold = base_threshold * 0.50f; // 进一步降低阈值，让细线条更明显

                                                // 渐变处理：在阈值附近使用概率绘制
                                                float gradient_range = 0.15f; // 增大渐变范围，让更多细节显示
                                                if (coverage_ratio > threshold - gradient_range &&
                                                    coverage_ratio < threshold + gradient_range)
                                                {
                                                    // 在渐变区域内，根据覆盖率调整绘制概率
                                                    float gradient_factor = (coverage_ratio - (threshold - gradient_range)) / (2 * gradient_range);
                                                    gradient_factor = fmaxf(0.0f, fminf(1.0f, gradient_factor));

                                                    // 使用简单的确定性"概率"：基于像素坐标的伪随机
                                                    threshold = threshold - gradient_range + (gradient_factor * 2 * gradient_range);
                                                }
                                            }
                                            else
                                            {
                                                // 非边缘区域：也降低阈值，让细线条更容易显示
                                                threshold = base_threshold * 0.85f; // 原来直接使用base_threshold
                                            }

                                            // 限制阈值范围，降低最小阈值来保持细线条
                                            threshold = fmaxf(0.10f, fminf(0.75f, threshold)); // 进一步降低范围

                                            if (coverage_ratio > threshold)
                                            {
                                                target_canvas->drawPixel(canvas_x + sx, canvas_y + sy, text_color);
                                            }
                                        }
                                    }
                                }
                            }
#endif
                        } // 结束 V2字体的条件分支
                    } // 结束缩放分支
                } // 结束质量模式分支
            } // 结束 char_bitmap && target_canvas 检查
            // 清理本地位图缓冲
            if (local_bitmap)
            {
                delete[] local_bitmap;
                local_bitmap = nullptr;
                char_bitmap = nullptr;
            }
            // 横排模式：向右移动，包括字符宽度和字符间距
            x += (int16_t)(glyph->width * scale_factor);

            // 检查是否不是行末的最后一个字符，如果不是则添加字符间距
            const uint8_t *next_utf8 = utf8;
            if (next_utf8 < line_utf8_end)
            {                                                           // 还有下一个字符
                x += (int16_t)(CHAR_SPACING_HORIZONTAL * scale_factor); // 横排模式添加水平字符间距，应用缩放
            }
        }

        y += scaled_line_height;
        line_start = line_end;
        if (line_start < display_text.length() && display_text[line_start] == '\n')
            line_start++;
        if (line_start < display_text.length() && drawBottom)
        {
            // 在当前行底部绘制水平线 (lightgrey, y+5)
            if (canvas)
            {
                canvas->drawFastHLine(MARGIN_LEFT, y - LINE_MARGIN * 2 / 3, PAPER_S3_WIDTH - MARGIN_LEFT - MARGIN_RIGHT, TFT_DARKGREY);
            }
        }
    }

    g_cursor_x = g_margin_left;
    g_cursor_y = y;
    // 只对全局canvas设置光标，自定义canvas不影响全局光标状态
    if (!canvas && g_canvas)
        g_canvas->setCursor(g_cursor_x, g_cursor_y);
    else if (canvas)
        canvas->setCursor(g_cursor_x, g_cursor_y);

#if DBG_BIN_FONT_PRINT
    unsigned long totalTime = millis() - printStartTime;
    unsigned long totalReadTime = g_total_glyph_read_us / 1000; // 转换为毫秒
    unsigned long drawTime = totalTime - totalReadTime;
    Serial.printf("[BIN_FONT] 渲染完成: 总耗时=%lu ms [文件读取]=%lu ms [绘制推送]=%lu ms\n",
                  totalTime, totalReadTime, drawTime);
    Serial.printf("[BIN_FONT] 详情: 字符数=%d 行数=%d 字体=%dpt 缩放=%.2f 颜色=%d fast_mode=%d\n",
                  char_count, line_count, font_size, scale_factor, color, fast_mode);
    if (char_count > 0)
    {
        float avg_read_us = (float)g_total_glyph_read_us / char_count;
        float avg_draw_us = (drawTime > 0) ? (float)(drawTime * 1000.0f) / char_count : 0;
        Serial.printf("[BIN_FONT] 性能指标: 读取=%.1f us/char 绘制=%.1f us/char [总字符=%d]\n",
                      avg_read_us, avg_draw_us, char_count);
    }

    // 输出缓存命中情况（本次渲染）
    Serial.printf("[BIN_FONT] Cache hits=%u misses=%u (manager_init=%d)\n",
                  (unsigned)cache_hits, (unsigned)cache_misses, cache_initialized ? 1 : 0);
    g_font_buffer_manager.logStats();

    // 简化缓存统计
    Serial.printf("[CACHE_STATS] === 分块缓存性能统计 ===\n");
    g_chunked_font_cache.print_stats();
    Serial.printf("[CACHE_STATS] === 统计完成 ===\n");

    // 输出readAtOffset统计
    SDW::SD.print_readAtOffset_stats();
    SDW::SD.reset_readAtOffset_stats();
#endif
}

// wrapper that accepts font_size (pixels) and computes scale_factor from font file base size
// find_break_position_scaled is implemented in src/text/line_handle.cpp
