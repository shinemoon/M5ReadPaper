#include "screenshot.h"
#include "SD/SDWrapper.h"
#include <time.h>
#include <vector>
#include "test/per_file_debug.h"

extern M5Canvas *g_canvas;

// PNG 文件头和必要的辅助函数
namespace PNGEncoder
{
    // CRC32 计算
    static uint32_t crc_table[256];
    static bool crc_table_computed = false;

    void make_crc_table()
    {
        uint32_t c;
        for (int n = 0; n < 256; n++)
        {
            c = (uint32_t)n;
            for (int k = 0; k < 8; k++)
            {
                if (c & 1)
                    c = 0xedb88320L ^ (c >> 1);
                else
                    c = c >> 1;
            }
            crc_table[n] = c;
        }
        crc_table_computed = true;
    }

    uint32_t update_crc(uint32_t crc, const uint8_t *buf, size_t len)
    {
        uint32_t c = crc;
        if (!crc_table_computed)
            make_crc_table();
        for (size_t n = 0; n < len; n++)
        {
            c = crc_table[(c ^ buf[n]) & 0xff] ^ (c >> 8);
        }
        return c;
    }

    uint32_t crc(const uint8_t *buf, size_t len)
    {
        return update_crc(0xffffffffL, buf, len) ^ 0xffffffffL;
    }

    // 写入大端序 32 位整数
    void write_be32(std::vector<uint8_t> &buf, uint32_t value)
    {
        buf.push_back((value >> 24) & 0xFF);
        buf.push_back((value >> 16) & 0xFF);
        buf.push_back((value >> 8) & 0xFF);
        buf.push_back(value & 0xFF);
    }

    // 写入 PNG chunk
    void write_chunk(std::vector<uint8_t> &png_data, const char *type, const uint8_t *data, size_t len)
    {
        write_be32(png_data, len);
        png_data.insert(png_data.end(), type, type + 4);
        if (len > 0 && data != nullptr)
        {
            png_data.insert(png_data.end(), data, data + len);
        }
        std::vector<uint8_t> crc_data;
        crc_data.insert(crc_data.end(), type, type + 4);
        if (len > 0 && data != nullptr)
        {
            crc_data.insert(crc_data.end(), data, data + len);
        }
        uint32_t crc_val = crc(crc_data.data(), crc_data.size());
        write_be32(png_data, crc_val);
    }

    // 简单的无压缩 zlib 封装（只添加 zlib 头和 Adler32 校验）
    // PNG 要求 IDAT 数据必须是 zlib 格式，即使是无压缩的
    
    // Adler-32 校验和计算
    uint32_t adler32(const uint8_t *data, size_t len)
    {
        const uint32_t MOD_ADLER = 65521;
        uint32_t a = 1, b = 0;
        
        for (size_t i = 0; i < len; i++)
        {
            a = (a + data[i]) % MOD_ADLER;
            b = (b + a) % MOD_ADLER;
        }
        
        return (b << 16) | a;
    }

    // 无压缩的 zlib 格式封装
    bool deflate_compress(const uint8_t *input, size_t input_len, std::vector<uint8_t> &output)
    {
        // zlib 头部（2字节）
        // CMF = 0x78 (CM=8 (deflate), CINFO=7 (32K window))
        // FLG = 0x01 (FCHECK, no preset dict, default compression)
        output.push_back(0x78);
        output.push_back(0x01);
        
        // 将数据分成最大 65535 字节的块（无压缩块）
        const size_t MAX_BLOCK_SIZE = 65535;
        size_t offset = 0;
        
        while (offset < input_len)
        {
            size_t block_size = (input_len - offset < MAX_BLOCK_SIZE) 
                              ? (input_len - offset) 
                              : MAX_BLOCK_SIZE;
            bool is_final = (offset + block_size >= input_len);
            
            // 块头：BFINAL(1bit) + BTYPE(2bits, 00=无压缩)
            uint8_t block_header = is_final ? 0x01 : 0x00; // BFINAL=1/0, BTYPE=00
            output.push_back(block_header);
            
            // LEN (2 bytes, little-endian)
            output.push_back(block_size & 0xFF);
            output.push_back((block_size >> 8) & 0xFF);
            
            // NLEN (2 bytes, little-endian, one's complement of LEN)
            uint16_t nlen = ~block_size;
            output.push_back(nlen & 0xFF);
            output.push_back((nlen >> 8) & 0xFF);
            
            // 数据
            output.insert(output.end(), input + offset, input + offset + block_size);
            
            offset += block_size;
        }
        
        // Adler-32 校验和（4字节，大端序）
        uint32_t checksum = adler32(input, input_len);
        output.push_back((checksum >> 24) & 0xFF);
        output.push_back((checksum >> 16) & 0xFF);
        output.push_back((checksum >> 8) & 0xFF);
        output.push_back(checksum & 0xFF);
        
        return true;
    }
}

// ensureScreenshotFolder() 已在 book_handle.cpp 中定义
// 在此只需要声明（通过包含 text/book_handle.h）
#include "text/book_handle.h"

bool screenShot()
{
    if (g_canvas == nullptr)
    {
#if DBG_SCREENSHOT
        Serial.println("[SCREENSHOT] g_canvas 为空，无法截图");
#endif
        return false;
    }

    // 确保目录存在
    if (!ensureScreenshotFolder())
    {
#if DBG_SCREENSHOT
        Serial.println("[SCREENSHOT] 无法创建 /screenshot 目录");
#endif
        return false;
    }

    // 获取当前时间
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    // 生成文件名
    char filename[128];
    snprintf(filename, sizeof(filename), "/screenshot/readpaper_screen_%04d_%02d_%02d_%02d_%02d_%02d.png",
             timeinfo.tm_year + 1900,
             timeinfo.tm_mon + 1,
             timeinfo.tm_mday,
             timeinfo.tm_hour,
             timeinfo.tm_min,
             timeinfo.tm_sec);

#if DBG_SCREENSHOT
    Serial.printf("[SCREENSHOT] 准备截图: %s\n", filename);
#endif

    int width = g_canvas->width();
    int height = g_canvas->height();

#if DBG_SCREENSHOT
    Serial.printf("[SCREENSHOT] 画布尺寸: %dx%d\n", width, height);
#endif

    // PNG 文件头
    std::vector<uint8_t> png_data;
    // PNG 签名
    const uint8_t png_sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    png_data.insert(png_data.end(), png_sig, png_sig + 8);

    // IHDR chunk (13 bytes)
    std::vector<uint8_t> ihdr_data;
    PNGEncoder::write_be32(ihdr_data, width);
    PNGEncoder::write_be32(ihdr_data, height);
    ihdr_data.push_back(8);    // bit depth (8-bit grayscale)
    ihdr_data.push_back(0);    // color type (0 = grayscale)
    ihdr_data.push_back(0);    // compression method
    ihdr_data.push_back(0);    // filter method
    ihdr_data.push_back(0);    // interlace method
    PNGEncoder::write_chunk(png_data, "IHDR", ihdr_data.data(), ihdr_data.size());

    // 准备图像数据（每行前面加滤波器类型字节）
    std::vector<uint8_t> raw_image;
    raw_image.reserve(height * (width + 1));

    for (int y = 0; y < height; y++)
    {
        raw_image.push_back(0); // 滤波器类型 0 (None)
        for (int x = 0; x < width; x++)
        {
            // 读取像素颜色
            uint16_t color = g_canvas->readPixel(x, y);
            
            // M5Paper 使用 4-bit grayscale (16级灰度)
            // 提取4位灰度值（0-15）并扩展到8位（0-255）
            uint8_t gray4 = (color >> 8) & 0x0F;  // 提取高4位
            uint8_t gray = gray4 * 17;  // 将0-15映射到0-255 (0*17=0, 15*17=255)
            raw_image.push_back(gray);
        }
    }

    // 压缩图像数据
    std::vector<uint8_t> compressed_data;
    if (!PNGEncoder::deflate_compress(raw_image.data(), raw_image.size(), compressed_data))
    {
#if DBG_SCREENSHOT
        Serial.println("[SCREENSHOT] 压缩失败");
#endif
        return false;
    }

    // IDAT chunk
    PNGEncoder::write_chunk(png_data, "IDAT", compressed_data.data(), compressed_data.size());

    // IEND chunk
    PNGEncoder::write_chunk(png_data, "IEND", nullptr, 0);

    // 写入文件
    File file = SDW::SD.open(filename, FILE_WRITE);
    if (!file)
    {
#if DBG_SCREENSHOT
        Serial.printf("[SCREENSHOT] 无法创建文件: %s\n", filename);
#endif
        return false;
    }

    size_t written = file.write(png_data.data(), png_data.size());
    file.close();

    if (written != png_data.size())
    {
#if DBG_SCREENSHOT
        Serial.printf("[SCREENSHOT] 写入不完整: %d/%d\n", written, png_data.size());
#endif
        return false;
    }

#if DBG_SCREENSHOT
    Serial.printf("[SCREENSHOT] 截图成功: %s (%d bytes)\n", filename, png_data.size());
#endif

    return true;
}
