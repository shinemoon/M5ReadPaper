#include <FS.h>
#include <SPIFFS.h>
#include "SD/SDWrapper.h"
#include <stdint.h>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <M5Unified.h>
#include "readpaper.h"
#include "readpaper.h"
#include "papers3.h"
#include "device/memory_pool.h"
#include "device/chunked_font_cache.h"
#include "test/per_file_debug.h"
#include "tasks/display_push_task.h"
#include "device/file_manager.h"
#include "../text/zh_conv.h"
// access per-book bookmark config
#include "book_handle.h"
#include "line_handle.h"
#include "font_decoder.h"
#include "bin_font_print.h"

// 检测是否为需要旋转的中文标点符号（与bin_font_print.cpp中保持一致）
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
        unicode == 0x2013 || unicode == 0x2014 ||   // –—
        unicode == 0x2015 || unicode == 0xFF0D ||   // ―－
        unicode == 0x2500 || unicode == 0x2501 ||   // ─━
        unicode == 0x003D || unicode == 0x007E ||   // =~
        (unicode >= 0x0030 && unicode <= 0x0039) || // 0-9
        (unicode >= 0x0041 && unicode <= 0x005A) || // A-Z
        (unicode >= 0x0061 && unicode <= 0x007A) || // a-z
        (unicode >= 0x00C0 && unicode <= 0x00FF) || // Latin-1 Supplement (À-ÿ)
        (unicode >= 0x0100 && unicode <= 0x017F) || // Latin Extended-A (Ā-ſ)
        (unicode >= 0x0180 && unicode <= 0x024F));  // Latin Extended-B (ƀ-ɏ)
}

// 检测是否为禁止出现在行首的标点符号
// 包括: ,.;!?>]})，。！？》"、:：』」
static bool is_forbidden_line_start_punctuation(uint32_t unicode)
{
    return (
        unicode == 0x002C || // ,
        unicode == 0x002E || // .
        unicode == 0x003B || // ;
        unicode == 0x0021 || // !
        unicode == 0x003F || // ?
        unicode == 0x003E || // >
        unicode == 0x005D || // ]
        unicode == 0x007D || // }
        unicode == 0x3015 || // 】
        unicode == 0xFF09 || // )
        unicode == 0x0029 || // )
        unicode == 0xFF0C || // ，
        unicode == 0x3002 || // 。
        unicode == 0xFF01 || // ！
        unicode == 0xFF1F || // ？
        unicode == 0x300B || // 》
        unicode == 0x201D || // "
        unicode == 0x2019 || // '
        unicode == 0x3001 || // 、
        unicode == 0x003A || // :
        unicode == 0xFF1A || // ：
        unicode == 0x300F || // 』
        unicode == 0x300D);  // 」
}

// 检测是否为更高优先级的禁止行首标点（这些标点更不应该出现在行首）
// 包括: >]})》"'』」
static bool is_high_priority_forbidden_line_start(uint32_t unicode)
{
    return (
        unicode == 0x003E || // >
        unicode == 0x005D || // ]
        unicode == 0x007D || // }
        unicode == 0x0029 || // )
        unicode == 0x300B || // 》
        unicode == 0x201D || // "
        unicode == 0x3015 || // 】
        unicode == 0xFF09 || // )
        unicode == 0x2019 || // '
        unicode == 0x300F || // 』
        unicode == 0x300D);  // 」
}

// 检测是否为配对标点的前半（开头部分），应优先推到下一行
static bool is_opening_pair_punctuation(uint32_t unicode)
{
    return (
        unicode == 0x0028 || // (
        unicode == 0x005B || // [
        unicode == 0x007B || // {
        unicode == 0x003C || // <
        unicode == 0xFF08 || // （
        unicode == 0x3010 || // 【
        unicode == 0x3008 || // 〈
        unicode == 0x300A || // 《
        unicode == 0x201C || // “ (LEFT DOUBLE QUOTATION MARK)
        unicode == 0x2018 || // ‘ (LEFT SINGLE QUOTATION MARK)
        unicode == 0x300C || // 「
        unicode == 0x300E || // 『
        unicode == 0x300A /* duplicate handled above but kept for clarity */);
}

int16_t calculate_text_width(const std::string &text, size_t start_pos, size_t end_pos)
{
    int16_t width = 0;
    const uint8_t *utf8 = (const uint8_t *)text.c_str() + start_pos;
    const uint8_t *end = (const uint8_t *)text.c_str() + std::min(end_pos, text.length());

    while (utf8 < end)
    {
        uint32_t unicode = utf8_decode(utf8, end);
        if (unicode == 0)
            break;

        bool glyph_exists = bin_font_has_glyph(unicode);
        if (glyph_exists)
        {
            width += bin_font_get_glyph_width(unicode);
        }
        else
        {
            width += bin_font_get_font_size() / 2;
        }
    }
    return width;
}

size_t find_break_position(const std::string &text, size_t start_pos, int16_t max_width, bool vertical, float scale_factor)
{
    size_t best_break = start_pos;
    int16_t best_break_width = 0;  // 新增：记录 best_break 位置时的实际显示宽度（不含空格本身）
    size_t current_pos = start_pos;
    const uint8_t *utf8 = (const uint8_t *)text.c_str() + start_pos;
    const uint8_t *end = (const uint8_t *)text.c_str() + text.length();
    int16_t current_width = 0;
    // Track last included character (start offset and unicode) so we can detect
    // if the line currently would end with an opening-pair punctuation.
    uint32_t last_included_unicode = 0;
    size_t last_included_offset = start_pos;
    bool opening_push_done = false; // only apply this once per line


    while (utf8 < end)
    {
        const uint8_t *prev_utf8 = utf8;
        uint32_t unicode = utf8_decode(utf8, end);
        if (unicode == 0)
            break;

        if (unicode == '\n')
        {
            return utf8 - (const uint8_t *)text.c_str();
        }

        bool glyph_exists = bin_font_has_glyph(unicode);

        int16_t char_dimension;
        if (vertical)
        {
            // 竖排模式下，标点符号旋转后宽高互换
            if (is_chinese_punctuation(unicode) && glyph_exists)
            {
                // 标点符号旋转90度后，使用位图宽度(bitmapW)作为竖直方向的尺寸
                char_dimension = (int16_t)(bin_font_get_glyph_bitmapW(unicode) * scale_factor);
            }
            else
            {
                char_dimension = glyph_exists ? (int16_t)(bin_font_get_glyph_bitmapH(unicode) * scale_factor) : (int16_t)(bin_font_get_font_size() * scale_factor);
            }
        }
        else
        {
            char_dimension = glyph_exists ? (int16_t)(bin_font_get_glyph_width(unicode) * scale_factor) : (int16_t)(bin_font_get_font_size() * scale_factor / 2);
        }

        if (!glyph_exists || bin_font_get_glyph_bitmap_size(unicode) == 0)
        {
            // Match rendering fallback: use half of base font size scaled by scale_factor
            char_dimension = (int16_t)(bin_font_get_font_size() * scale_factor / 2);
        }

        int16_t char_spacing = vertical ? CHAR_SPACING_VERTICAL : (int16_t)(CHAR_SPACING_HORIZONTAL * scale_factor);

        if (current_width + char_dimension + char_spacing > max_width)
        {
            // 行宽度即将用完，当前字符放不下了
            
            // 在断行前，检查当前字符（即将成为下一行行首的字符）是否为禁止行首标点
            if (unicode != 0 && unicode != '\n' && is_forbidden_line_start_punctuation(unicode))
            {
                // 当前字符是禁止行首标点，预读下一个字符（第二个字符）
                const uint8_t *next_utf8 = utf8;
                uint32_t next_unicode = utf8_decode(next_utf8, end);

                // 当且仅当第二个字符不是高优先级禁止行首标点时，才进行pullin
                if (next_unicode == 0 || next_unicode == '\n' || !is_high_priority_forbidden_line_start(next_unicode))
                {
                    // 检查强行加入该标点后是否在可接受范围内（允许稍微超出，例如不超过1.15倍）
                    int16_t total_width_with_punct = current_width + char_dimension + char_spacing;

                    if (total_width_with_punct <= max_width * 1.15f)
                    {
                        // 将该禁止行首标点强行包含进当前行
                        current_pos = utf8 - (const uint8_t *)text.c_str();
                        return current_pos;
                    }
                }
                // 如果第二个字符是高优先级禁止行首标点，或者超出太多，则放弃pullin，正常断行
            }

            if (best_break > start_pos)
            {
                // 关键修复：只有在 best_break 距离当前位置不太远时才使用
                // 如果best_break位置到当前位置的距离超过max_width的40%，说明中间有大量可放下的内容
                // 此时应该就近断行而不是回退到遥远的空格位置
                size_t distance = (size_t)(prev_utf8 - (const uint8_t *)text.c_str()) - best_break;
                int16_t distance_width = current_width - best_break_width;
                
                if (distance_width <= max_width * 0.4f)
                {
                    // 距离合理，使用best_break（空格断点）
                    size_t piece_len = best_break - start_pos;
                    if (piece_len <= 16)
                    {
                        bool all_whitespace = true;
                        const uint8_t *check_start = (const uint8_t *)text.c_str() + start_pos;
                        const uint8_t *check_end = (const uint8_t *)text.c_str() + best_break;
                        const uint8_t *check_ptr = check_start;

                        while (check_ptr < check_end && all_whitespace)
                        {
                            uint32_t check_unicode = utf8_decode(check_ptr, check_end);
                            if (check_unicode != ' ' && check_unicode != '\t')
                            {
                                all_whitespace = false;
                            }
                        }

                        if (all_whitespace)
                        {
                            size_t return_pos = prev_utf8 - (const uint8_t *)text.c_str();
                            return return_pos;
                        }
                    }
                    return best_break;
                }
                else
                {
                    // 距离太远，说明best_break后还能放很多内容，应该就近断行
                    return prev_utf8 - (const uint8_t *)text.c_str();
                }
            }
            
            // 检查开头配对标点：只有在没有找到更好的断点（best_break）时才考虑这个选项
            // 这样可以优先使用空格等自然断点，避免破坏英文单词
            if (!opening_push_done && last_included_unicode != 0 && is_opening_pair_punctuation(last_included_unicode))
            {
                // 确保不会生成空行
                if (last_included_offset > start_pos)
                {
                    opening_push_done = true;
                    return last_included_offset;
                }
            }
            
            size_t fallback_pos = prev_utf8 - (const uint8_t *)text.c_str();
            return fallback_pos;
        }

        current_width += char_dimension + char_spacing;
        current_pos = utf8 - (const uint8_t *)text.c_str();

        // 记录最后一个成功包含到当前行的字符（起始偏移和 unicode），
        // 供开头配对标点优先推送检查使用
        last_included_unicode = unicode;
        last_included_offset = (size_t)(prev_utf8 - (const uint8_t *)text.c_str());

        // 遇到空格、制表符或连字符时记录为潜在断行点
        // 关键修复：记录不含空格本身宽度的累计值，因为渲染时行尾空格不显示
        if (unicode == ' ' || unicode == '\t' || unicode == '-')
        {
            best_break = current_pos;
            // 保存去除当前空格字符宽度后的实际显示宽度
            best_break_width = current_width - char_dimension - char_spacing;
        }
    }

    return current_pos;
}

size_t find_break_position_scaled(const std::string &text, size_t start_pos, int16_t max_width, bool vertical, float font_size)
{
    float scale_factor = 1.0f;
    uint8_t base_font = get_font_size_from_file();
    if (font_size > 0 && base_font > 0)
    {
        scale_factor = font_size / (float)base_font;
    }
    return find_break_position(text, start_pos, max_width, vertical, scale_factor);
}
