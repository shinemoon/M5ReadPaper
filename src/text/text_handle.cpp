#include "text_handle.h"
#include "book_handle.h"
#include "current_book.h"
#include "text/bin_font_print.h"
#include "zh_conv.h"
#include "gbk_unicode_table.h"
#include "test/per_file_debug.h"
#include <stddef.h>
#include <vector>
#include <deque>
#include <algorithm>
#include <array>
#include <cstring>

TextState g_text_state;

// 自动分流：SPIFFS用SPIFFS API，SD卡用SD API
#include <SPIFFS.h>
#include <SD.h>
#include "../SD/SDWrapper.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Debug flags
#define DBG_IDX_PAGINATION 0  // Debug logging for idx-aware pagination

// 编码检测函数
TextEncoding detect_text_encoding(const uint8_t *buffer, size_t size)
{
    if (size < 3)
        return TextEncoding::UTF8; // 默认UTF8

    // 检测UTF8 BOM (EF BB BF)
    if (size >= 3 && buffer[0] == 0xEF && buffer[1] == 0xBB && buffer[2] == 0xBF)
    {
#if DBG_TEXT_HANDLE
        Serial.println("[ENCODING] 检测到UTF8 BOM");
#endif
        return TextEncoding::UTF8;
    }

    // 简单启发式检测
    size_t valid_utf8_chars = 0;
    size_t total_chars = 0;
    size_t gbk_chars = 0;

    for (size_t i = 0; i < size && i < 1024; i++)
    { // 检测前1KB
        uint8_t byte = buffer[i];
        total_chars++;

        // ASCII字符
        if (byte < 0x80)
        {
            valid_utf8_chars++;
            continue;
        }

        // UTF8多字节序列检测
        if ((byte & 0xE0) == 0xC0 && i + 1 < size)
        { // 2字节UTF8
            uint8_t next = buffer[i + 1];
            if ((next & 0xC0) == 0x80)
            {
                valid_utf8_chars += 2;
                i++; // 跳过下一个字节
                continue;
            }
        }
        else if ((byte & 0xF0) == 0xE0 && i + 2 < size)
        { // 3字节UTF8
            uint8_t next1 = buffer[i + 1];
            uint8_t next2 = buffer[i + 2];
            if ((next1 & 0xC0) == 0x80 && (next2 & 0xC0) == 0x80)
            {
                valid_utf8_chars += 3;
                i += 2; // 跳过后续字节
                continue;
            }
        }

        // GBK范围检测 (A1-FE A1-FE)
        if (byte >= 0xA1 && byte <= 0xFE && i + 1 < size)
        {
            uint8_t next = buffer[i + 1];
            if (next >= 0xA1 && next <= 0xFE)
            {
                gbk_chars += 2;
                i++; // 跳过下一个字节
                continue;
            }
        }
    }

    // 防止 total_chars 为 0 导致除零
    float gbk_ratio = 0.0f;
    if (total_chars == 0)
    {
#if DBG_TEXT_HANDLE
        Serial.println("[ENCODING] 输入过短，默认UTF8");
#endif
        return TextEncoding::UTF8;
    }
    else
    {
        gbk_ratio = (float)gbk_chars / (float)total_chars;
    }
#if DBG_TEXT_HANDLE
    float utf8_ratio = (float)valid_utf8_chars / (float)total_chars;
    Serial.printf("[ENCODING] UTF8比率: %.2f, GBK比率: %.2f\n", utf8_ratio, gbk_ratio);
#endif

    if (gbk_ratio > 0.3)
    {
#if DBG_TEXT_HANDLE
        Serial.println("[ENCODING] 检测为GBK编码");
#endif
        return TextEncoding::GBK;
    }
    else
    {
#if DBG_TEXT_HANDLE
        Serial.println("[ENCODING] 检测为UTF8编码");
#endif
        return TextEncoding::UTF8;
    }
}

// Helper: detect encoding (when AUTO_DETECT) and update/create bookmark file with encoding and current position
static TextEncoding detect_encoding_and_update_bookmark(File &file, const std::string &file_path, size_t start_pos,
                                                        TextEncoding encoding, int16_t area_width, int16_t area_height, float font_size)
{
    TextEncoding detected_encoding = encoding;
    if (encoding == TextEncoding::AUTO_DETECT)
    {
        uint8_t detect_buffer[1024];
        size_t detect_size = file.readBytes((char *)detect_buffer, sizeof(detect_buffer));
        detected_encoding = detect_text_encoding(detect_buffer, detect_size);

        // reset file pointer back to start_pos - detect read advanced it
        file.seek(start_pos);

        // 保存到全局状态
        g_text_state.encoding = detected_encoding;

        // 将检测到的 encoding 写回书签（创建或更新 /bookmarks/<name>.bm），以便下次直接复用
        std::string bfn = getBookmarkFileName(file_path);
        ensureBookmarksFolder();

        if (SDW::SD.exists(bfn.c_str()))
        {
            File rf = SDW::SD.open(bfn.c_str(), "r");
            std::vector<String> lines;
            if (rf)
            {
                while (rf.available())
                {
                    String l = rf.readStringUntil('\n');
                    l.trim();
                    lines.push_back(l);
                }
                rf.close();
            }
            bool found_enc = false;
            bool found_pos = false;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                if (lines[i].startsWith("encoding="))
                {
                    lines[i] = String("encoding=") + String((int)detected_encoding);
                    found_enc = true;
                }
                if (lines[i].startsWith("current_position="))
                {
                    lines[i] = String("current_position=") + String((unsigned long)start_pos);
                    found_pos = true;
                }
            }
            if (!found_enc)
                lines.push_back(String("encoding=") + String((int)detected_encoding));
            if (!found_pos)
                lines.push_back(String("current_position=") + String((unsigned long)start_pos));

            File wf = SDW::SD.open(bfn.c_str(), "w");
            if (wf)
            {
                for (auto &ln : lines)
                {
                    wf.println(ln);
                }
                wf.close();
            }
            else
            {
#if DBG_TEXT_HANDLE
                Serial.printf("[ENCODING] 无法写回书签文件 %s\n", bfn.c_str());
#endif
            }
        }
        else
        {
            File bf = SDW::SD.open(bfn.c_str(), "w");
            if (bf)
            {
                bf.printf("file_path=%s\n", file_path.c_str());
                bf.printf("current_position=%zu\n", start_pos);
                bf.printf("area_width=%d\n", area_width);
                bf.printf("area_height=%d\n", area_height);
                bf.printf("font_size=%.2f\n", font_size);
                bf.printf("encoding=%d\n", (int)detected_encoding);
                bf.println("valid=true");
                bf.close();
            }
            else
            {
#if DBG_TEXT_HANDLE
                Serial.printf("[ENCODING] 无法创建书签文件 %s\n", bfn.c_str());
#endif
            }
        }
    }
    return detected_encoding;
}

// 使用查表方式进行编码转换
std::string convert_to_utf8(const std::string &input, TextEncoding from_encoding)
{
    // Robust conversion that tolerates mixed bytes when the file encoding was
    // determined as UTF8 or GBK. Behavior:
    // - If from_encoding==UTF8: try to parse valid UTF-8 sequences; when an
    //   invalid sequence is encountered, attempt to interpret as GBK (two bytes);
    //   if GBK maps to Unicode, emit its UTF-8; otherwise emit U+25A1 and
    //   advance by one raw byte.
    // - If from_encoding==GBK: try GBK pairs first; if pair invalid, try to
    //   interpret remaining bytes as UTF-8 sequences; otherwise emit U+25A1.

    std::string out;
    const uint8_t *buf = (const uint8_t *)input.c_str();
    size_t len = input.length();
    size_t i = 0;

    auto emit_placeholder = [&out]()
    {
        uint8_t tmp[4];
        int l = utf8_encode(0x25A1, tmp);
        out.append((const char *)tmp, l);
    };

    if (from_encoding == TextEncoding::UTF8)
    {
        while (i < len)
        {
            uint8_t b = buf[i];
            // ASCII
            if (b < 0x80)
            {
                out.push_back((char)b);
                i++;
                continue;
            }

            // try valid UTF-8 2-byte
            if ((b & 0xE0) == 0xC0 && i + 1 < len)
            {
                uint8_t n1 = buf[i + 1];
                if ((n1 & 0xC0) == 0x80)
                {
                    out.append((const char *)&buf[i], 2);
                    i += 2;
                    continue;
                }
            }
            // try valid UTF-8 3-byte
            if ((b & 0xF0) == 0xE0 && i + 2 < len)
            {
                uint8_t n1 = buf[i + 1];
                uint8_t n2 = buf[i + 2];
                if ((n1 & 0xC0) == 0x80 && (n2 & 0xC0) == 0x80)
                {
                    out.append((const char *)&buf[i], 3);
                    i += 3;
                    continue;
                }
            }

            // invalid UTF-8 sequence here; try GBK two-byte
            if (i + 1 < len)
            {
                uint8_t b2 = buf[i + 1];
                if (b >= 0xA1 && b <= 0xFE && b2 >= 0xA1 && b2 <= 0xFE)
                {
                    uint16_t gbk_code = (uint16_t(b) << 8) | uint16_t(b2);
                    uint16_t uni = gbk_to_unicode_lookup(gbk_code);
                    if (uni != 0)
                    {
                        uint8_t tmp[4];
                        int l = utf8_encode(uni, tmp);
                        out.append((const char *)tmp, l);
                        i += 2;
                        continue;
                    }
                }
            }

            // give up on this byte: emit placeholder and advance 1
            emit_placeholder();
            i += 1;
        }
        return out;
    }

    if (from_encoding == TextEncoding::GBK)
    {
        while (i < len)
        {
            uint8_t b = buf[i];
            if (b < 0x80)
            {
                out.push_back((char)b);
                i++;
                continue;
            }

            // try GBK pair first
            if (i + 1 < len)
            {
                uint8_t b2 = buf[i + 1];
                if (b >= 0xA1 && b <= 0xFE && b2 >= 0xA1 && b2 <= 0xFE)
                {
                    uint16_t gbk_code = (uint16_t(b) << 8) | uint16_t(b2);
                    uint16_t uni = gbk_to_unicode_lookup(gbk_code);
                    if (uni != 0)
                    {
                        uint8_t tmp[4];
                        int l = utf8_encode(uni, tmp);
                        out.append((const char *)tmp, l);
                        i += 2;
                        continue;
                    }
                }
            }

            // try valid UTF-8 sequence as fallback
            if ((b & 0xE0) == 0xC0 && i + 1 < len)
            {
                uint8_t n1 = buf[i + 1];
                if ((n1 & 0xC0) == 0x80)
                {
                    out.append((const char *)&buf[i], 2);
                    i += 2;
                    continue;
                }
            }
            if ((b & 0xF0) == 0xE0 && i + 2 < len)
            {
                uint8_t n1 = buf[i + 1];
                uint8_t n2 = buf[i + 2];
                if ((n1 & 0xC0) == 0x80 && (n2 & 0xC0) == 0x80)
                {
                    out.append((const char *)&buf[i], 3);
                    i += 3;
                    continue;
                }
            }

            // unknown byte: emit placeholder and advance
            emit_placeholder();
            i += 1;
        }
        return out;
    }

    // fallback: return original
    return input;
}

// 文件级辅助函数：将转换后（UTF8）的位置映射回原始raw字节已消费量（处理GBK双字节）
static size_t map_converted_pos_to_raw_consumed(const std::string &raw, TextEncoding enc, size_t converted_pos)
{
    const uint8_t *buf = (const uint8_t *)raw.c_str();
    size_t raw_len = raw.length();
    // We simulate the same tolerant conversion as convert_to_utf8 until we've
    // produced converted_pos bytes, then return how many raw bytes were consumed.
    size_t acc_converted_bytes = 0;
    size_t i = 0;

    auto emit_utf8_len_of_unicode = [](uint16_t unicode) -> int
    {
        uint8_t tmp[4];
        return utf8_encode(unicode, tmp);
    };

    if (enc == TextEncoding::UTF8)
    {
        while (i < raw_len)
        {
            uint8_t b = buf[i];
            if (b < 0x80)
            {
                acc_converted_bytes += 1;
                i += 1;
            }
            else if ((b & 0xE0) == 0xC0 && i + 1 < raw_len && (buf[i + 1] & 0xC0) == 0x80)
            {
                acc_converted_bytes += 2;
                i += 2;
            }
            else if ((b & 0xF0) == 0xE0 && i + 2 < raw_len && (buf[i + 1] & 0xC0) == 0x80 && (buf[i + 2] & 0xC0) == 0x80)
            {
                acc_converted_bytes += 3;
                i += 3;
            }
            else
            {
                // invalid UTF-8 here; try GBK pair
                if (i + 1 < raw_len)
                {
                    uint8_t b2 = buf[i + 1];
                    if (b >= 0xA1 && b <= 0xFE && b2 >= 0xA1 && b2 <= 0xFE)
                    {
                        uint16_t gbk_code = (uint16_t(b) << 8) | uint16_t(b2);
                        uint16_t uni = gbk_to_unicode_lookup(gbk_code);
                        if (uni != 0)
                        {
                            acc_converted_bytes += emit_utf8_len_of_unicode(uni);
                            i += 2;
                            if (acc_converted_bytes >= converted_pos)
                                return i;
                            continue;
                        }
                    }
                }
                // fallback: placeholder U+25A1 (3 bytes)
                acc_converted_bytes += 3;
                i += 1;
            }

            if (acc_converted_bytes >= converted_pos)
                return i;
        }
        return raw_len;
    }

    // GBK: decode GBK pairs first, fallback to UTF-8 sequences
    while (i < raw_len)
    {
        uint8_t b = buf[i];
        if (b < 0x80)
        {
            acc_converted_bytes += 1;
            i += 1;
        }
        else if (i + 1 < raw_len && b >= 0xA1 && b <= 0xFE && buf[i + 1] >= 0xA1 && buf[i + 1] <= 0xFE)
        {
            uint16_t gbk_code = (uint16_t(b) << 8) | uint16_t(buf[i + 1]);
            uint16_t uni = gbk_to_unicode_lookup(gbk_code);
            if (uni != 0)
            {
                acc_converted_bytes += emit_utf8_len_of_unicode(uni);
            }
            else
            {
                acc_converted_bytes += 3; // placeholder
            }
            i += 2;
        }
        else if ((b & 0xE0) == 0xC0 && i + 1 < raw_len && (buf[i + 1] & 0xC0) == 0x80)
        {
            acc_converted_bytes += 2;
            i += 2;
        }
        else if ((b & 0xF0) == 0xE0 && i + 2 < raw_len && (buf[i + 1] & 0xC0) == 0x80 && (buf[i + 2] & 0xC0) == 0x80)
        {
            acc_converted_bytes += 3;
            i += 3;
        }
        else
        {
            // unknown single byte -> placeholder
            acc_converted_bytes += 3;
            i += 1;
        }

        if (acc_converted_bytes >= converted_pos)
            return i;
    }
    return raw_len;
}

// 文件级辅助函数：读取一行原始内容并返回原始字节长度（保持 file 指针）
static bool read_raw_line(File &f, std::string &out_raw, size_t &out_raw_bytes)
{
    if (!f)
    {
        out_raw.clear();
        out_raw_bytes = 0;
        return false;
    }

    constexpr size_t READ_BUF = 4096;
    std::array<char, READ_BUF> buf{};
    size_t start_pos = f.position();
    out_raw.clear();

    while (true)
    {
        size_t bytes_read = f.read(reinterpret_cast<uint8_t *>(buf.data()), READ_BUF);

        if (bytes_read == 0)
        {
            // EOF 或暂无更多数据
            break;
        }

        char *newline = static_cast<char *>(memchr(buf.data(), '\n', bytes_read));
        if (newline != nullptr)
        {
            size_t to_copy = static_cast<size_t>(newline - buf.data()) + 1; // 包含换行符
            out_raw.append(buf.data(), to_copy);

            size_t surplus = bytes_read - to_copy;
            if (surplus > 0)
            {
                size_t current_pos = f.position();
                size_t target_pos = (surplus > current_pos) ? 0 : (current_pos - surplus);
                f.seek(target_pos);
            }
            break;
        }

        out_raw.append(buf.data(), bytes_read);

        // 如果本次读取少于缓冲区大小，说明到达文件尾
        if (bytes_read < READ_BUF)
        {
            break;
        }
    }

    size_t end_pos = f.position();
    out_raw_bytes = (end_pos >= start_pos) ? (end_pos - start_pos) : out_raw.size();

    if (out_raw.empty())
    {
        out_raw_bytes = 0;
        return false;
    }
    return true;
}

// 文件级辅助函数：处理单条 raw_line（包含编码转换、断行、追加到 page），返回本条实际消耗的原始字节数和新增行数
static size_t process_raw_line(const std::string &raw_line, size_t raw_bytes_read, TextEncoding enc, int16_t max_width, int max_lines_remaining, std::string &page_out, int &lines_added_out, float font_size, bool vertical = false)
{
    lines_added_out = 0;

#if DBG_TEXT_HANDLE
    Serial.printf("[PROCESS_RAW] 开始处理行: raw_bytes=%zu, max_width=%d, max_lines_remaining=%d, vertical=%s\n",
                  raw_bytes_read, max_width, max_lines_remaining, vertical ? "true" : "false");
#endif

    // convert raw bytes to UTF-8 for layout
    std::string converted_line = raw_line;
    if (enc != TextEncoding::UTF8)
        converted_line = convert_to_utf8(raw_line, enc);

    // Apply zh conversion according to global config and book-level keepOrg flag
    extern GlobalConfig g_config;
    bool apply_conversion = (g_config.zh_conv_mode != 0);
    if (g_current_book && g_current_book->getKeepOrg())
        apply_conversion = false;
    // Always run zh_conv_utf8 to ensure placeholder substitution for missing glyphs;
    // when apply_conversion is false we call mode=0 which preserves original text but replaces missing glyphs.
    if (apply_conversion)
        converted_line = zh_conv_utf8(converted_line, g_config.zh_conv_mode);
    else
        converted_line = zh_conv_utf8(converted_line, 0);

    // raw_line now includes any line separator characters (we ensure read_raw_line
    // appends the '\n' if it was consumed). For mapping we want the raw bytes
    // inside the line (excluding separator) so strip trailing CR/LF for mapping
    // purposes only.
    std::string raw_for_map = raw_line;
    // strip trailing LF
    if (!raw_for_map.empty() && raw_for_map.back() == '\n')
        raw_for_map.pop_back();
    // strip trailing CR now
    if (!raw_for_map.empty() && raw_for_map.back() == '\r')
        raw_for_map.pop_back();

    // Preserve whether the original raw line had an explicit newline (CR/LF)
    bool has_explicit_newline = false;
    if (!raw_line.empty() && raw_line.back() == '\n')
        has_explicit_newline = true;

    // For splitting into display pieces, remove trailing CR/LF so pieces do not contain newline chars
    std::string converted_for_split = converted_line;
    if (!converted_for_split.empty() && converted_for_split.back() == '\n')
        converted_for_split.pop_back();
    if (!converted_for_split.empty() && converted_for_split.back() == '\r')
        converted_for_split.pop_back();

    size_t pos_local = 0;
#if DBG_TEXT_HANDLE_RAW
    Serial.printf("[PROCESS_RAW] input raw_len=%zu conv_len=%zu has_newline=%d max_width=%d max_lines=%d\n",
                  raw_line.length(), converted_for_split.length(), has_explicit_newline, max_width, max_lines_remaining);
    // 打印转换后内容的前几个字符
    Serial.printf("[PROCESS_RAW] converted_for_split hex: ");
    for (size_t i = 0; i < std::min(converted_for_split.length(), (size_t)20); ++i)
    {
        Serial.printf("%02X ", (uint8_t)converted_for_split[i]);
    }
    Serial.println();
#endif
    while (pos_local < converted_for_split.length() && lines_added_out < max_lines_remaining)
    {
        // 竖排模式下：去除每列开头的空白字符（空格、制表符等）
        size_t original_pos_local = pos_local;
        if (vertical)
        {
            while (pos_local < converted_for_split.length())
            {
                unsigned char ch = converted_for_split[pos_local];

                // 检查是否为常见的空白字符
                if (ch == ' ' ||  // 0x20 空格
                    ch == '\t' || // 0x09 制表符
                    ch == '\r' || // 0x0D 回车
                    ch == '\n')
                { // 0x0A 换行
                    pos_local++;
#if DBG_TEXT_HANDLE
                    Serial.printf("[PROCESS_RAW] 竖排模式跳过空白字符: 0x%02X\n", ch);
#endif
                }
                else if (pos_local + 2 < converted_for_split.length() &&
                         ch == 0xE3 && converted_for_split[pos_local + 1] == 0x80 && converted_for_split[pos_local + 2] == 0x80)
                {
                    // UTF-8编码的全角空格 (U+3000: 0xE3 0x80 0x80)
                    pos_local += 3;
#if DBG_TEXT_HANDLE
                    Serial.printf("[PROCESS_RAW] 竖排模式跳过全角空格\n");
#endif
                }
                else
                {
                    break; // 非空白字符，停止跳过
                }
            }

            // 记录跳过的空白字符数量用于调试
            if (pos_local > original_pos_local)
            {
#if DBG_TEXT_HANDLE
                Serial.printf("[PROCESS_RAW] 竖排模式跳过了 %zu 字节的开头空白字符\n",
                              pos_local - original_pos_local);
#endif
            }
        }

#if DBG_TEXT_HANDLE
        Serial.printf("[PROCESS_RAW] 调用find_break_position: pos_local=%zu, max_width=%d, 剩余字符=%zu\n",
                      pos_local, max_width, converted_for_split.length() - pos_local);
#endif
        // compute scale factor matching rendering when a font_size is provided
        float scale_factor_local = 1.0f;
        uint8_t base_font_local = get_font_size_from_file();
        if (font_size > 0 && base_font_local > 0)
        {
            scale_factor_local = font_size / (float)base_font_local;
        }
    // Use shared wrapper that accepts font_size so scale computation is consistent
    size_t break_pos = find_break_position_scaled(converted_for_split, pos_local, max_width, vertical, font_size);
#if DBG_TEXT_HANDLE
        Serial.printf("[PROCESS_RAW] find_break_position返回: break_pos=%zu (前进了%zu字符)\n",
                      break_pos, break_pos - pos_local);
        // 打印断行片段的前20个字符
        if (break_pos > pos_local)
        {
            std::string break_piece = converted_for_split.substr(pos_local, std::min((size_t)60, break_pos - pos_local));
            Serial.printf("[PROCESS_RAW] 断行片段[%zu->%zu]: '%s'\n", pos_local, break_pos, break_piece.c_str());
        }
#endif
        if (break_pos == pos_local)
            break;
        std::string piece = converted_for_split.substr(pos_local, break_pos - pos_local);
#if DBG_TEXT_HANDLE
        Serial.printf("[PROCESS_RAW] piece[%d]: pos=%zu->%zu len=%zu: ", lines_added_out, pos_local, break_pos, piece.length());
        for (size_t i = 0; i < std::min(piece.length(), (size_t)10); ++i)
        {
            Serial.printf("%02X ", (uint8_t)piece[i]);
        }
        Serial.println();
        // 打印实际文本内容（限制60字节避免溢出）
        std::string piece_preview = piece.substr(0, std::min((size_t)60, piece.length()));
        Serial.printf("[PROCESS_RAW] 第%d行内容: '%s'%s\n", lines_added_out, piece_preview.c_str(), 
                      piece.length() > 60 ? "..." : "");
#endif
        page_out += piece;
        page_out += '\n';
        pos_local = break_pos;
        lines_added_out++;
    }

    // If no piece was added but original raw line had an explicit newline, preserve an empty display line
    if (lines_added_out == 0 && has_explicit_newline && lines_added_out < max_lines_remaining)
    {
        page_out += '\n';
        lines_added_out = 1;
        return raw_bytes_read;
    }

    // 计算实际消耗的原始字节数
    if (pos_local == converted_for_split.length() && !converted_for_split.empty() && lines_added_out < max_lines_remaining)
    {
        // 整行被放下，消耗整行字节（包含行尾的 '\n' 或 CRLF）
        return raw_bytes_read;
    }
    else if (pos_local < converted_for_split.length())
    {
        // 行被截断，只消耗对应的原始字节（不包含行尾换行符）
        size_t consumed_in_line = map_converted_pos_to_raw_consumed(raw_for_map, enc, pos_local);
        return consumed_in_line;
    }
    else
    {
        return raw_bytes_read;
    }
}

// 轻量版：仅计行数并返回消耗的原始字节数（不构造 page 文本）
static size_t process_raw_line_count(const std::string &raw_line, size_t raw_bytes_read, TextEncoding enc,
                                     int16_t max_width, int max_lines_remaining, int &lines_added_out, float font_size, bool vertical = false)
{
    lines_added_out = 0;

    // Convert only when needed and reuse a local buffer to avoid repeated allocations
    std::string converted_storage;
    const std::string *converted = &raw_line;

    extern GlobalConfig g_config;
    bool apply_conversion = (g_config.zh_conv_mode != 0);
    if (g_current_book && g_current_book->getKeepOrg())
        apply_conversion = false;

    if (enc != TextEncoding::UTF8)
    {
        converted_storage = convert_to_utf8(raw_line, enc);
    }
    else
    {
        // copy to local buffer so we can safely run zh_conv_utf8 which may modify string length
        converted_storage = raw_line;
    }

    // Ensure placeholder substitution or conversion is applied so width calculations match rendering
    if (apply_conversion)
        converted_storage = zh_conv_utf8(converted_storage, g_config.zh_conv_mode);
    else
        converted_storage = zh_conv_utf8(converted_storage, 0);

    converted = &converted_storage;

    // Determine trimmed length (exclude trailing CR/LF for layout/splitting)
    size_t conv_len = converted->length();
    size_t trimmed_len = conv_len;
    if (trimmed_len > 0 && (*converted)[trimmed_len - 1] == '\n')
        --trimmed_len;
    if (trimmed_len > 0 && (*converted)[trimmed_len - 1] == '\r')
        --trimmed_len;

    bool has_explicit_newline = (!raw_line.empty() && raw_line.back() == '\n');

    // If no trimming needed, operate on the converted string directly to avoid copy
    const std::string *work_str = converted;
    std::string temp_trim;
    if (trimmed_len != conv_len)
    {
        temp_trim = converted->substr(0, trimmed_len);
        work_str = &temp_trim;
    }

    size_t pos_local = 0;
    while (pos_local < work_str->length() && lines_added_out < max_lines_remaining)
    {
        // 竖排模式下：去除每列开头的空白字符
        if (vertical)
        {
            while (pos_local < work_str->length())
            {
                unsigned char ch = (*work_str)[pos_local];

                // 检查是否为常见的空白字符
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
                {
                    pos_local++;
                }
                else if (pos_local + 2 < work_str->length() &&
                         ch == 0xE3 && (*work_str)[pos_local + 1] == 0x80 && (*work_str)[pos_local + 2] == 0x80)
                {
                    // UTF-8编码的全角空格 (U+3000: 0xE3 0x80 0x80)
                    pos_local += 3;
                }
                else
                {
                    break; // 非空白字符，停止跳过
                }
            }
        }

        // compute scale factor matching rendering when a font_size is provided
        float scale_factor = 1.0f;
        uint8_t base_font = get_font_size_from_file();
        if (font_size > 0 && base_font > 0)
        {
            scale_factor = font_size / (float)base_font;
        }
    size_t break_pos = find_break_position_scaled(*work_str, pos_local, max_width, vertical, font_size);
        if (break_pos == pos_local)
            break;
        pos_local = break_pos;
        lines_added_out++;
    }

    if (lines_added_out == 0 && has_explicit_newline && lines_added_out < max_lines_remaining)
    {
        lines_added_out = 1;
        return raw_bytes_read;
    }

    if (pos_local == work_str->length() && !work_str->empty() && lines_added_out < max_lines_remaining)
    {
        return raw_bytes_read;
    }
    else if (pos_local < work_str->length())
    {
        std::string raw_for_map = raw_line;
        if (!raw_for_map.empty() && raw_for_map.back() == '\n')
            raw_for_map.pop_back();
        if (!raw_for_map.empty() && raw_for_map.back() == '\r')
            raw_for_map.pop_back();
        size_t consumed_in_line = map_converted_pos_to_raw_consumed(raw_for_map, enc, pos_local);
        return consumed_in_line;
    }
    else
    {
        return raw_bytes_read;
    }
}

// Helper: load all idx entry positions from .idx file (if exists) for this book
// Returns sorted vector of byte positions; empty if no idx or error
static std::vector<size_t> load_idx_positions(const std::string &book_file_path)
{
    std::vector<size_t> positions;
    
    // Derive idx filename: replace extension with .idx, remove /sd/ or /spiffs/ prefix
    std::string idx_name = book_file_path;
    if (idx_name.rfind("/sd/", 0) == 0)
        idx_name = idx_name.substr(4);
    else if (idx_name.rfind("/spiffs/", 0) == 0)
        idx_name = idx_name.substr(8);
    
    size_t dot = idx_name.find_last_of('.');
    if (dot != std::string::npos)
        idx_name = idx_name.substr(0, dot) + ".idx";
    else
        idx_name += ".idx";
    
    // Try to open idx file (SD or SPIFFS)
    File idx_file;
    if (book_file_path.rfind("/spiffs/", 0) == 0)
    {
        std::string spiffs_path = std::string("/") + idx_name;
        if (SPIFFS.exists(spiffs_path.c_str()))
            idx_file = SPIFFS.open(spiffs_path.c_str(), "r");
    }
    else
    {
        std::string sd_path = std::string("/") + idx_name;
        if (SDW::SD.exists(sd_path.c_str()))
            idx_file = SDW::SD.open(sd_path.c_str(), "r");
    }
    
    if (!idx_file)
        return positions; // No idx file, return empty
    
#if DBG_IDX_PAGINATION
    Serial.printf("[IDX_PAGE] Loading idx positions from: %s\n", idx_name.c_str());
#endif
    
    // Parse idx file: format is #index#, #title#, #byte_pos#, #percent#,
    std::string line;
    line.reserve(256);
    while (idx_file.available())
    {
        line.clear();
        while (idx_file.available())
        {
            int c = idx_file.read();
            if (c == -1 || c == '\n')
                break;
            if (c != '\r')
                line.push_back((char)c);
        }
        
        if (line.empty() || line[0] != '#')
            continue;
        
        // Find all # delimiters
        std::vector<size_t> hash_pos;
        for (size_t i = 0; i < line.size(); ++i)
        {
            if (line[i] == '#')
                hash_pos.push_back(i);
        }
        
        if (hash_pos.size() < 8) // Need at least 8 # for valid format
            continue;
        
        // Extract byte position field (between hash_pos[4] and hash_pos[5])
        std::string pos_str = line.substr(hash_pos[4] + 1, hash_pos[5] - hash_pos[4] - 1);
        if (!pos_str.empty())
        {
            size_t pos = strtoull(pos_str.c_str(), nullptr, 10);
            positions.push_back(pos);
        }
    }
    
    idx_file.close();
    
    // Sort positions for binary search
    std::sort(positions.begin(), positions.end());
    
#if DBG_IDX_PAGINATION
    Serial.printf("[IDX_PAGE] Loaded %d idx positions\n", positions.size());
    if (!positions.empty())
    {
        Serial.printf("[IDX_PAGE] First position: %zu, Last position: %zu\n", 
                      positions.front(), positions.back());
    }
#endif
    
    return positions;
}

// 快速生成索引：返回每一页的 start_pos（raw file offsets）
// - 如果 max_pages>0 则最多生成 max_pages 项（便于分批索引）
// - function will leave file position at end of generated pages (caller can reopen/seek as needed)
BuildIndexResult build_book_page_index(File &file, const std::string &file_path,
                                       int16_t area_width, int16_t area_height, float font_size,
                                       TextEncoding encoding, size_t max_pages, size_t start_offset, bool vertical,
                                       BookHandle* bh)
{
    BuildIndexResult result;
    std::vector<size_t> &pages = result.pages;
    if (!(bool)file)
        return result;

    // detect encoding once if AUTO_DETECT
    TextEncoding enc = encoding;
    if (encoding == TextEncoding::AUTO_DETECT)
    {
        uint8_t detect_buffer[1024];
        file.seek(start_offset);
        size_t detect_size = file.readBytes((char *)detect_buffer, sizeof(detect_buffer));
        enc = detect_text_encoding(detect_buffer, detect_size);
        file.seek(start_offset);
        g_text_state.encoding = enc;
    }

    // 应用竖排模式的分页逻辑（与read_text_page_forward_file保持一致）
    int16_t line_height;
    if (g_line_height > 0)
    {
        // If caller requests a specific font_size (non-zero), scale the global line height
        // (which is based on the font file base size) so pagination matches rendering's
        // scaled_line_height = g_line_height * (font_size / base_font_size).
        if (font_size > 0)
        {
            uint8_t base = get_font_size_from_file();
            if (base > 0)
            {
                float sf = font_size / (float)base;
                line_height = (int16_t)(g_line_height * sf);
                if (line_height <= 0)
                    line_height = 1;
            }
            else
            {
                line_height = g_line_height;
            }
        }
        else
        {
            line_height = g_line_height;
        }
    }
    else
    {
        line_height = (int16_t)(font_size + LINE_MARGIN);
    }

    int max_lines, max_width;
    if (vertical)
    {
        // 竖排模式：能显示多少列取决于总宽度，每列的高度就是总高度
        // 考虑边距和更精确的计算
        int16_t available_width = area_width; // 可用宽度
        int16_t column_width = line_height;   // 每列占用的宽度（字符宽度+间距）

        // 更精确的列数计算：向上取整，充分利用空间
        max_lines = (available_width + column_width - 1) / column_width; // 等价于 ceil(available_width / column_width)

        // 但要确保不超出实际可用空间
        if (max_lines * column_width > available_width + column_width / 2)
        {
            max_lines--; // 如果超出太多，减少一列
        }

        //=> 此处如果要动，务必联动read_text_page_forward_file
        max_width = area_height - font_size/2; // 每列的垂直高度（与read_text_page_forward_file保持一致）!!!!
    }
    else
    {
        // 横排模式：正常逻辑
        max_lines = area_height / line_height; // 纵向能放多少行
        max_width = area_width;                // 每行的水平宽度
    }

    if (max_lines <= 0)
        max_lines = 1;

#if DBG_TEXT_HANDLE
    Serial.printf("[INDEX] 分页索引参数: vertical=%s, max_lines=%d, max_width=%d, area=(%d,%d)\n",
                  vertical ? "true" : "false", max_lines, max_width, area_width, area_height);
#endif

    // Load idx positions if available (for idx-aware pagination)
    std::vector<size_t> idx_positions_local;
    const std::vector<size_t> *idx_positions = nullptr;
    if (bh)
    {
        if (bh->isIdxCached())
        {
            idx_positions = &bh->getIdxPositions();
        }
        else
        {
            // Fall back to file-based load (one-time)
            idx_positions_local = load_idx_positions(bh->filePath());
            if (!idx_positions_local.empty())
                idx_positions = &idx_positions_local;
        }
#if DBG_IDX_PAGINATION
        if (idx_positions && !idx_positions->empty())
        {
            Serial.printf("[IDX_PAGE] Idx-aware pagination enabled with %d positions\n", (int)idx_positions->size());
        }
#endif
    }

    size_t current_start = start_offset;
    file.seek(start_offset);

    pages.reserve(1024);

    while (file.available())
    {
        pages.push_back(current_start);
#if DBG_TEXT_HANDLE
        Serial.printf("[INDEX] === Starting page %zu at offset %zu, file_pos=%zu ===\n", 
                      pages.size(), current_start, file.position());
#endif
        if (max_pages > 0 && pages.size() >= max_pages)
            break;

        int lines = 0;
        size_t consumed_total = 0; // relative to current_start
        bool hit_eof_in_page = false; // 标记本页是否包含了EOF
        bool is_partial_consumption = false; // 标记是否发生了部分消耗

        // sequentially read raw lines
        while (lines < max_lines && file.available())
        {
            // allow other tasks to run and check for external stop request frequently
            // 【优化】每行都让步，确保翻页等高优先级任务能及时响应
            taskYIELD();
            if (bh && bh->getAndClearIndexingShouldStop())
            {
                // abort early without marking EOF; return what we have so caller can persist progress
                BuildIndexResult early;
                early.pages = pages;
                early.reached_eof = false;
                return early;
            }

            // Check if current position is at an idx entry (but not at page start)
            // If so, end current page here to ensure idx entry starts a new page
            if (idx_positions && !idx_positions->empty() && consumed_total > 0)
            {
                size_t current_pos = current_start + consumed_total;
                // Binary search for exact match
                auto it = std::lower_bound(idx_positions->begin(), idx_positions->end(), current_pos);
                if (it != idx_positions->end() && *it == current_pos)
                {
                    // Current position is exactly at an idx entry, end page here
#if DBG_IDX_PAGINATION
                    Serial.printf("[IDX_PAGE] Ending page before idx entry at pos=%zu\n", current_pos);
#endif
                    break;
                }
            }

            std::string raw_line;
            size_t raw_bytes = 0;
            if (!read_raw_line(file, raw_line, raw_bytes))
            {
                // 读取失败，检查是否到达EOF
                if (!file.available())
                {
                    hit_eof_in_page = true;
#if DBG_TEXT_HANDLE
                    Serial.printf("[INDEX] EOF detected in read_raw_line: pos=%zu\n", file.position());
#endif
                }
                break;
            }
            int added = 0;
            size_t consumed_here = process_raw_line_count(raw_line, raw_bytes, enc, max_width, (max_lines - lines), added, font_size, vertical);
            lines += added;
            consumed_total += consumed_here;

#if DBG_TEXT_HANDLE
            Serial.printf("[INDEX] Line processed: raw_bytes=%zu consumed=%zu added=%d total_lines=%d consumed_total=%zu file_pos=%zu\n",
                          raw_bytes, consumed_here, added, lines, consumed_total, file.position());
#endif

            if (consumed_here < raw_bytes)
            {
                // partial consumption, next page starts within this raw line
                // 【关键】这里并不是EOF！而是行太长无法在本页全部显示
                // 剩余的 (raw_bytes - consumed_here) 字节将在下一页继续处理
#if DBG_TEXT_HANDLE
                Serial.printf("[INDEX] Partial consumption: raw_bytes=%zu consumed=%zu remaining=%zu\n",
                              raw_bytes, consumed_here, raw_bytes - consumed_here);
                Serial.printf("[INDEX] This is NOT EOF - remaining content will be on next page\n");
#endif
                // 明确标记：这不是EOF，而是部分消耗
                hit_eof_in_page = false;
                is_partial_consumption = true;  // 设置标志：发生了部分消耗
                break;
            }
            
            // 【新增】完整消耗后，检查是否这就是最后一行（到达EOF）
            if (consumed_here == raw_bytes && !file.available())
            {
                hit_eof_in_page = true;
#if DBG_TEXT_HANDLE
                Serial.printf("[INDEX] EOF detected after consuming complete line: pos=%zu\n", file.position());
#endif
                break;
            }
            // otherwise continue reading next raw line
        }
        
        // 检查循环结束时是否到达EOF（本页包含了EOF内容）
        // 【关键修复】如果是partial consumption，不要检查EOF（因为还有内容未处理）
        if (!hit_eof_in_page && !is_partial_consumption && !file.available())
        {
            hit_eof_in_page = true;
#if DBG_TEXT_HANDLE
            Serial.printf("[INDEX] EOF detected after inner loop exit: file_pos=%zu consumed_total=%zu\n", 
                          file.position(), consumed_total);
#endif
        }

        // 【关键修复】如果本页包含了EOF内容，立即标记完成并退出，不要再执行seek
        // 这是针对短文件的关键修复：避免在已到达EOF后继续seek导致错误的分页
        if (hit_eof_in_page)
        {
            result.reached_eof = true;
#if DBG_TEXT_HANDLE
            Serial.printf("[INDEX] Marking reached_eof=true: total_pages=%zu current_start=%zu last_consumed=%zu (EOF detected)\n", 
                          pages.size(), current_start, consumed_total);
#endif
            break;
        }

        // 【短文件修复】处理consumed_total=0的情况
        // 原因：当read_raw_line失败或内层循环未执行时，consumed_total可能为0
        // 如果consumed_total=0但未检测到EOF，说明遇到了异常情况（例如空行、纯空白等）
        // 此时强制+1继续，但这可能导致短文件分页不完整
        size_t next_start = current_start + consumed_total;
        if (next_start <= current_start)
        {
            // consumed_total=0的情况：可能是空内容或读取异常
            // 检查是否真的到达EOF（双重检查）
            if (!file.available())
            {
                // 如果文件确实没有更多内容，标记EOF完成
                result.reached_eof = true;
#if DBG_TEXT_HANDLE
                Serial.printf("[INDEX] consumed_total=0 and no more data, marking reached_eof: total_pages=%zu\n", 
                              pages.size());
#endif
                break;
            }
            // 否则强制前进1字节，避免死循环
            next_start = current_start + 1;
#if DBG_TEXT_HANDLE
            Serial.printf("[INDEX] consumed_total=0 but file.available()=true, forcing +1: pos=%zu\n", 
                          current_start);
#endif
        }

        file.seek(next_start);
        current_start = next_start;

        if ((pages.size() & 0x0F) == 0)
        {
            taskYIELD();
            vTaskDelay(pdMS_TO_TICKS(PAGES_DELAY));
        }
    }

    return result;
}

// 文件级辅助函数：将候选扫描位置对齐到行起始或安全字符边界，避免从多字节字符中间开始
static size_t align_scan_pos_to_boundary(File &file, size_t pos, TextEncoding enc)
{
    if (pos == 0)
        return 0;

    size_t original_pos = file.position();

    // 优先：在一个小窗口内回溯查找最近的换行符，从其后一字节开始
    const size_t BACK_WINDOW = 256; // 最多回看256字节
    size_t back = (pos > BACK_WINDOW) ? BACK_WINDOW : pos;
    uint8_t buf[BACK_WINDOW];

    size_t start = pos - back;
    file.seek(start);
    size_t n = file.readBytes((char *)buf, back);
    // 查找最后一个'\n'
    for (size_t i = n; i-- > 0;)
    {
        if (buf[i] == '\n')
        {
            size_t aligned = start + i + 1;
            file.seek(original_pos);
            return aligned;
        }
    }

    // 未找到行边界，则按编码尽量对齐到字符边界
    size_t aligned = pos;
    if (enc == TextEncoding::UTF8)
    {
        // UTF-8: 回退最多3字节，找到非续字节(不是10xxxxxx)的位置
        size_t back_utf8 = (pos >= 3) ? 3 : pos;
        size_t probe_start = pos - back_utf8;
        uint8_t tmp[8] = {0};
        file.seek(probe_start);
        file.readBytes((char *)tmp, (pos - probe_start) + 4); // 读入一些后续字节，便于判定
        size_t rel = pos - probe_start;
        size_t best = pos;
        for (size_t t = 0; t <= rel; ++t)
        {
            size_t j = rel - t;
            if ((tmp[j] & 0xC0) != 0x80)
            {
                best = probe_start + j;
                break;
            }
        }
        aligned = best;
    }
    else if (enc == TextEncoding::GBK)
    {
        // GBK: 如果落在双字节的第二字节，则回退1字节
        if (pos >= 1)
        {
            uint8_t pair[2] = {0};
            file.seek(pos - 1);
            size_t m = file.readBytes((char *)pair, 2);
            if (m == 2)
            {
                if (pair[0] >= 0xA1 && pair[0] <= 0xFE && pair[1] >= 0xA1 && pair[1] <= 0xFE)
                {
                    aligned = pos - 1;
                }
            }
        }
    }

    file.seek(original_pos);
    return aligned;
}

// (已移除路径级别的向前声明 — 现在优先使用 File& 版本)

// 将核心的向后读取逻辑拆成一个内部 helper，便于在向前翻页时复用
// 内部核心实现：基于已打开的 File 实现（caller 负责 open/close）
static TextPageResult read_text_page_forward_file(File &file, const std::string &file_path, size_t start_pos,
                                                  int16_t area_width, int16_t area_height, float font_size,
                                                  TextEncoding encoding, bool vertical = false, size_t max_byte_pos = SIZE_MAX)
{
    TextPageResult result;
    result.success = false;
    result.file_pos = start_pos;
    result.page_end_pos = start_pos;
    result.page_text.clear();

    // Debug: initial parameters
#if DBG_TEXT_HANDLE
    Serial.printf("[TEXT] forward START file_path=%s start_pos=%zu area=(%d,%d) font=%.2f enc=%d vertical=%s\n",
                  file_path.c_str(), start_pos, area_width, area_height, font_size, (int)encoding, vertical ? "true" : "false");
#endif

    // Ensure file pointer at start_pos before reading/detecting
    file.seek(start_pos);
    // 编码检测：如果调用者传入了明确编码，则直接使用；只有在 AUTO_DETECT 时才进行检测
    TextEncoding detected_encoding = encoding;
    if (encoding == TextEncoding::AUTO_DETECT)
    {
        uint8_t detect_buffer[1024];
        size_t detect_size = file.readBytes((char *)detect_buffer, sizeof(detect_buffer));
        detected_encoding = detect_text_encoding(detect_buffer, detect_size);

        // reset file pointer back to start_pos - detect read advanced it
        file.seek(start_pos);

        // 保存到全局状态
        g_text_state.encoding = detected_encoding;

        // 将检测到的 encoding 写回书签（创建或更新 /bookmarks/<name>.bm），以便下次直接复用
        detected_encoding = detect_encoding_and_update_bookmark(file, file_path, start_pos, encoding, area_width, area_height, font_size);
    }

    // 在垂直模式下，需要重新理解显示逻辑：
    // 竖排模式：文字从上到下排列，列从右到左排列
    // max_lines: 表示能显示多少列（在竖排模式下，"列"就是从右到左的分割）
    // max_width: 表示每列的高度（在竖排模式下，用于垂直方向的断行）

    int16_t line_height;
    if (g_line_height > 0)
    {
        if (font_size > 0)
        {
            uint8_t base = get_font_size_from_file();
            if (base > 0)
            {
                float sf = font_size / (float)base;
                line_height = (int16_t)(g_line_height * sf);
                if (line_height <= 0)
                    line_height = 1;
            }
            else
            {
                line_height = g_line_height;
            }
        }
        else
        {
            line_height = g_line_height;
        }
    }
    else
    {
        line_height = (int16_t)(font_size + LINE_MARGIN);
    }

    int max_lines, max_width;
    if (vertical)
    {
        // 竖排模式：能显示多少列取决于总宽度，每列的高度就是总高度
        // 考虑边距和更精确的计算
        int16_t available_width = area_width; // 可用宽度
        int16_t column_width = line_height;   // 每列占用的宽度（字符宽度+间距）

        // 更精确的列数计算：向上取整，充分利用空间
        max_lines = (available_width + column_width - 1) / column_width; // 等价于 ceil(available_width / column_width)

        // 但要确保不超出实际可用空间
        if (max_lines * column_width > available_width + column_width / 2)
        {
            max_lines--; // 如果超出太多，减少一列
        }

        //        max_width = area_height - font_size;               // 每列的垂直高度 //竖排稍微调整下, 保守点 => Seems too safe..
        //=> 此处如果要动，务必联动 build_book_page_index
        max_width = area_height - font_size/2; // 每列的垂直高度 
    }
    else
    {
        // 横排模式：正常逻辑
        max_lines = area_height / line_height; // 纵向能放多少行
        max_width = area_width;                // 每行的水平宽度
    }

#if DBG_TEXT_HANDLE
    Serial.printf("[TEXT_HANDLE] 断行参数: line_height=%d, max_lines=%d, max_width=%d, vertical=%s\n",
                  line_height, max_lines, max_width, vertical ? "true" : "false");
    if (vertical)
    {
        Serial.printf("[TEXT_HANDLE] 竖排模式: area_width=%d -> max_lines=%d列, area_height=%d -> max_width=%d(每列高度)\n",
                      area_width, max_lines, area_height, max_width);
        Serial.printf("[TEXT_HANDLE] 列计算详情: available_width=%d, column_width=%d, 理论列数=%.2f\n",
                      area_width, line_height, (float)area_width / line_height);
    }
#endif

    int lines = 0;
    size_t file_ptr = start_pos;
    std::string page;

    size_t total_read_bytes = 0;     // 总读取字节数
    size_t consumed_bytes_total = 0; // 相对于 start_pos 的已消费原始字节数

    // 使用文件作用域的 helper: map_converted_pos_to_raw_consumed(...)

    // 使用文件作用域的 helper: read_raw_line(...)

    // 使用文件作用域的 helper: process_raw_line(...)

    while (lines < max_lines && file.available())
    {
        // read raw line
        std::string raw_line;
        size_t raw_bytes_read = 0;
        read_raw_line(file, raw_line, raw_bytes_read);
        total_read_bytes += raw_bytes_read;

#if DBG_TEXT_HANDLE
        // print short hex preview of raw_line
        Serial.printf("[TEXT] forward read raw bytes: raw_bytes=%zu len=%zu\n", raw_bytes_read, raw_line.length());
        size_t preview = raw_line.length() < 16 ? raw_line.length() : 16;
        for (size_t i = 0; i < preview; ++i)
            Serial.printf("%02X ", (uint8_t)raw_line[i]);
        Serial.println();
#endif
#if DBG_TEXT_HANDLE_VERBOSE
        Serial.printf("[TEXT] forward read raw bytes: raw_bytes=%zu\n", raw_bytes_read);
#endif

        // process raw line into page (may be partial consumption)
    int lines_added = 0;
    size_t consumed_here = process_raw_line(raw_line, raw_bytes_read, detected_encoding, max_width, (max_lines - lines), page, lines_added, font_size, vertical);

#if DBG_TEXT_HANDLE
        Serial.printf("[TEXT] forward consumed_here=%zu lines_added=%d\n", consumed_here, lines_added);
#endif

        // if process_raw_line consumed less than full raw line, and it added a remaining fragment line,
        // we should mark that the remaining part was consumed as part of this page (handled by process_raw_line)
        lines += lines_added;
        consumed_bytes_total += consumed_here;
        file_ptr = start_pos + consumed_bytes_total;

        // Check boundary after updating file_ptr (efficient: only integer comparison)
        if (max_byte_pos != SIZE_MAX && file_ptr >= max_byte_pos)
        {
#if DBG_TEXT_HANDLE
            Serial.printf("[TEXT] Consumed content reached boundary %zu (file_ptr=%zu), stopping\n", max_byte_pos, file_ptr);
#endif
            break;
        }

        // if we processed a truncated remainder and page is full, stop reading further
        if (lines >= max_lines)
            break;
    }

#if DBG_TEXT_HANDLE
    Serial.printf("[DEBUG][TXT] 最终总行数=%d\n", lines);
    Serial.printf("[PAGE] page_text长度=%zu, 内容预览=\n%.10s\n", page.length(), page.c_str());
#endif

    size_t file_size = 0;
    // caller may obtain file size via file.size()
    file_size = file.size();

    if (file_ptr <= start_pos)
    {
        size_t new_end = start_pos + 1;
        if (file_size > 0 && new_end > file_size)
            new_end = file_size;
#if DBG_TEXT_HANDLE
        Serial.printf("[TEXT][WARN] page_end_pos(%zu) <= start_pos(%zu)，强制推进到 %zu\n", file_ptr, start_pos, new_end);
#endif
        file_ptr = new_end;
    }

    // 不在此处关闭 file，caller 负责

    result.success = true;
    result.file_pos = start_pos;
    result.page_end_pos = file_ptr;
    result.page_text = page;

    // 更新全局状态
    g_text_state.file_path = file_path;
    g_text_state.file_pos = start_pos;
    g_text_state.page_end_pos = result.page_end_pos;
    g_text_state.last_page = result.page_text;
    // 缓存当前页的起点，便于下一次向前翻页加速（下一页的prev是此start_pos）
    g_text_state.prev_page_start = start_pos;

#if DBG_TEXT_HANDLE
    Serial.printf("[TEXT] forward total_read_bytes=%zu consumed_bytes=%zu\n", total_read_bytes, consumed_bytes_total);
#endif

    return result;
}

// 将核心的向后读取逻辑拆成一个内部 helper，便于在向前翻页时复用
// 内部核心实现：基于已打开的 File 实现（caller 负责 open/close）
// Removed

// 注意：移除基于路径的兼容实现，强制使用基于已打开 File 的单一策略。
// 任何需要读取分页的代码必须负责打开/关闭文件并传入有效的 File&。

// 新增：基于已打开 File 的公共接口（caller 负责 open/close）
TextPageResult read_text_page(File &file, const std::string &file_path, size_t start_pos,
                              int16_t area_width, int16_t area_height, float font_size,
                              TextEncoding encoding, bool backward, bool vertical, size_t max_byte_pos)
{
    // Simplified behavior: do not rely on history. For both forward and backward
    // attempts, delegate to the forward file-based parser starting at start_pos.
    // Caller may call this repeatedly with different start_pos if they want
    // to search backwards; this function will only attempt a single parse.
    if (!(bool)file)
    {
        TextPageResult empty;
        empty.success = false;
        return empty;
    }
    return read_text_page_forward_file(file, file_path, start_pos, area_width, area_height, font_size, encoding, vertical, max_byte_pos);
}

// 统一的分页函数：计算给定文本段的分页信息
PageBreakResult calculate_page_breaks(const std::string &text, size_t start_pos,
                                      int16_t area_width, int16_t area_height,
                                      float font_size, int max_lines, int16_t max_width,
                                      bool vertical)
{
    PageBreakResult result;

    if (text.empty() || start_pos >= text.length())
    {
        result.success = false;
        return result;
    }

#if DBG_TEXT_HANDLE
    Serial.printf("[UNIFIED_PAGE] 开始计算分页: start_pos=%zu, max_lines=%d, max_width=%d, vertical=%s\n",
                  start_pos, max_lines, max_width, vertical ? "true" : "false");
#endif

    size_t current_pos = start_pos;
    int lines = 0;

    while (current_pos < text.length() && lines < max_lines)
    {
        // 在竖排模式下，去除每行开头的空白字符
        if (vertical)
        {
            while (current_pos < text.length())
            {
                char c = text[current_pos];
                if (c != ' ' && c != '\t' && c != '\r')
                {
                    break;
                }
                current_pos++;
            }
            if (current_pos >= text.length())
                break;
        }

        // 查找断行位置
    size_t break_pos = find_break_position_scaled(text, current_pos, max_width, vertical, font_size);

        if (break_pos == current_pos)
        {
            // 无法前进，避免无限循环
            break;
        }

        result.line_breaks.push_back(break_pos);
        current_pos = break_pos;
        lines++;

        // 跳过换行符
        if (current_pos < text.length() && text[current_pos] == '\n')
        {
            current_pos++;
        }

#if DBG_TEXT_HANDLE
        if (lines <= 3 || lines == max_lines)
        { // 只打印前几行和最后一行的调试信息
            Serial.printf("[UNIFIED_PAGE] 第%d行: break_pos=%zu, 下一行开始=%zu\n",
                          lines, break_pos, current_pos);
        }
#endif
    }

    result.page_end_pos = current_pos;
    result.lines_count = lines;
    result.success = true;

#if DBG_TEXT_HANDLE
    Serial.printf("[UNIFIED_PAGE] 分页完成: lines=%d, page_end_pos=%zu\n",
                  result.lines_count, result.page_end_pos);
#endif

    return result;
}
