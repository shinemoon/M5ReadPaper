#pragma once
#include <string>
#include <stdint.h>
#include "readpaper.h"
#include <M5Unified.h>
#include <vector>
#include <unordered_map>
#include <FS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// PSRAM 自定义分配器模板
// 强制 STL 容器使用 PSRAM 而不是内部 DRAM
template <typename T>
struct PSRAMAllocator {
    using value_type = T;
    
    PSRAMAllocator() = default;
    
    template <typename U>
    constexpr PSRAMAllocator(const PSRAMAllocator<U>&) noexcept {}
    
    T* allocate(std::size_t n) {
        if (n > std::size_t(-1) / sizeof(T)) {
            return nullptr;
        }
        // 强制使用 PSRAM
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
        if (!p) {
            // PSRAM 分配失败，尝试内部 RAM
            p = malloc(n * sizeof(T));
        }
        return static_cast<T*>(p);
    }
    
    void deallocate(T* p, std::size_t) noexcept {
        heap_caps_free(p);
    }
};

template <typename T, typename U>
bool operator==(const PSRAMAllocator<T>&, const PSRAMAllocator<U>&) { return true; }

template <typename T, typename U>
bool operator!=(const PSRAMAllocator<T>&, const PSRAMAllocator<U>&) { return false; }

// 字体格式枚举
enum FontFormat {
    FONT_FORMAT_UNKNOWN = 0,
    FONT_FORMAT_HUFFMAN = 1,  // bin_font_generator.py (霍夫曼编码)
    FONT_FORMAT_1BIT = 2      // generate_1bit_font_bin.py (1bit打包)
};

// 文本对齐方式枚举
enum TextAlign {
    TEXT_ALIGN_LEFT = 0,    // 左对齐
    TEXT_ALIGN_CENTER = 1,  // 居中对齐
    TEXT_ALIGN_RIGHT = 2    // 右对齐
};

// 加载 bin font 文件，返回 true/false
bool load_bin_font(const char* path);

// 卸载 bin font
void unload_bin_font();

// 获取当前加载的字体名称
const char* get_current_font_name();

// 获取字体族名
const char* get_font_family_name();

// 获取字体样式名
const char* get_font_style_name();

// 获取字体版本
uint8_t get_font_version();

// 获取字体文件中的基础字体大小
uint8_t get_font_size_from_file();

// 检查当前加载字体是否包含指定的 Unicode 字形
bool bin_font_has_glyph(uint32_t unicode);

// 灰度判断函数：4位量化灰度值转黑白判断
bool isBlack(uint16_t quantized_gray);

// 统一的字体渲染函数：透明背景，支持真实字体大小和16级灰度颜色
// canvas: 目标画布，如果为nullptr则使用全局g_canvas
// font_size: 真实字体大小（像素），如果为0则使用字体文件的原始大小
// color: 0-15 (0=黑色, 15=白色)
// fast_mode: 如果为 true，则采用原先 text_font_print 的高性能路径（不支持缩放和透明）
// text_align: 文本对齐方式，支持左、中、右对齐，适用于单行文本
// max_length: 最大显示长度限制（像素），0表示不限制，非0时仅显示单行且限制显示长度
void bin_font_print(const std::string& text, uint8_t font_size = 0, uint8_t color = 0, int16_t area_width = PAPER_S3_WIDTH, int16_t margin_left = 10, int16_t margin_top = 10, bool fast_mode = false, M5Canvas* canvas = nullptr, TextAlign text_align = TEXT_ALIGN_LEFT, int16_t max_length = 0, bool skipConv = false, bool drawBottom=false, bool vertical=false, bool dark=false);

// 设置光标位置
void bin_font_set_cursor(int16_t x, int16_t y);

// 重置光标到左上
void bin_font_reset_cursor();

// Canvas管理函数
void bin_font_flush_canvas(bool trans=false, bool invert=false, bool quality=false, display_type effect=NOEFFECT);
void bin_font_clear_canvas(bool dark=false);

int16_t bin_font_get_cursor_y();

void bin_font_test_rendering();

// 供分页算法调用
extern int16_t g_line_height;
size_t find_break_position(const std::string &text, size_t start_pos, int16_t max_width, bool vertical = false, float scale_factor = 1.0f);
// Convenience wrapper: provide font_size (pixels) and compute internal scale_factor
size_t find_break_position_scaled(const std::string &text, size_t start_pos, int16_t max_width, bool vertical, float font_size);

// 字体数据结构
struct BinFontChar
{
    uint16_t unicode;
    uint16_t width;
    uint8_t bitmapW;
    uint8_t bitmapH;
    int8_t x_offset;
    int8_t y_offset;
    uint32_t bitmap_offset;
    uint32_t bitmap_size;
    uint32_t cached_bitmap; // reserved
} __attribute__((packed));

// 轻量级字形索引结构（仅用于流式模式）
// 每项只需 10 字节，2万字符只占 200KB
struct GlyphIndex
{
    uint16_t unicode;        // 2 bytes
    uint16_t width;          // 2 bytes  
    uint8_t bitmapW;         // 1 byte
    uint8_t bitmapH;         // 1 byte
    int8_t x_offset;         // 1 byte
    int8_t y_offset;         // 1 byte
    uint32_t bitmap_offset;  // 4 bytes - 文件中位图数据的偏移
    uint32_t bitmap_size;    // 4 bytes - 位图数据大小
} __attribute__((packed));  // 总共 16 字节

struct BinFont
{
    uint32_t char_count;
    uint8_t font_size;
    uint8_t version;
    FontFormat format;
    char family_name[64];
    char style_name[64];
    char font_path[256];                             // 保存字体文件路径，用于重新打开
    bool use_spiffs;                                  // true=SPIFFS, false=SD
    std::vector<BinFontChar, PSRAMAllocator<BinFontChar>> chars;                    // 缓存模式使用（PSRAM）
    std::vector<GlyphIndex, PSRAMAllocator<GlyphIndex>> index;                     // 流式模式使用（PSRAM）
    std::unordered_map<uint16_t, GlyphIndex*, std::hash<uint16_t>, std::equal_to<uint16_t>, 
                       PSRAMAllocator<std::pair<const uint16_t, GlyphIndex*>>> indexMap;  // 流式模式 O(1) 查找表（PSRAM）
    File fontFile;
};

// Global font instance (defined in bin_font_print.cpp)
extern BinFont g_bin_font;

// Flag indicating if currently using PROGMEM font (true) or SD/SPIFFS font (false)
extern bool g_using_progmem_font;

// Find a glyph; returns const pointer
const BinFontChar *find_char(uint32_t unicode);

// Safe accessors (avoid exposing internal struct in other translation units)
int16_t bin_font_get_glyph_width(uint32_t unicode);
int16_t bin_font_get_glyph_bitmapW(uint32_t unicode);
int16_t bin_font_get_glyph_bitmapH(uint32_t unicode);
uint32_t bin_font_get_glyph_bitmap_size(uint32_t unicode);
uint8_t bin_font_get_font_size();