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

    // 打开文件
    File file = SDW::SD.open(filename, FILE_WRITE);
    if (!file)
    {
#if DBG_SCREENSHOT
        Serial.printf("[SCREENSHOT] 无法创建文件: %s\n", filename);
#endif
        return false;
    }

    // 写入 PNG 签名
    const uint8_t png_sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    file.write(png_sig, 8);

    // 准备并写入 IHDR chunk
    std::vector<uint8_t> ihdr_chunk;
    PNGEncoder::write_be32(ihdr_chunk, 13); // IHDR 数据长度
    ihdr_chunk.insert(ihdr_chunk.end(), {'I', 'H', 'D', 'R'});
    PNGEncoder::write_be32(ihdr_chunk, width);
    PNGEncoder::write_be32(ihdr_chunk, height);
    ihdr_chunk.push_back(8);    // bit depth (8-bit grayscale)
    ihdr_chunk.push_back(0);    // color type (0 = grayscale)
    ihdr_chunk.push_back(0);    // compression method
    ihdr_chunk.push_back(0);    // filter method
    ihdr_chunk.push_back(0);    // interlace method
    uint32_t ihdr_crc = PNGEncoder::crc(ihdr_chunk.data() + 4, 17); // 跳过长度字段
    PNGEncoder::write_be32(ihdr_chunk, ihdr_crc);
    file.write(ihdr_chunk.data(), ihdr_chunk.size());
    ihdr_chunk.clear();
    ihdr_chunk.shrink_to_fit();

    // 分块处理图像数据以减少内存占用
    const int ROWS_PER_CHUNK = 50; // 每次处理50行
    std::vector<uint8_t> compressed_data;
    compressed_data.reserve(height * (width + 1) / 2); // 预估压缩后大小
    
    // zlib 头部
    compressed_data.push_back(0x78);
    compressed_data.push_back(0x01);
    
    uint32_t adler_a = 1;
    uint32_t adler_b = 0;
    const uint32_t MOD_ADLER = 65521;
    
    for (int chunk_start = 0; chunk_start < height; chunk_start += ROWS_PER_CHUNK)
    {
        int chunk_rows = (chunk_start + ROWS_PER_CHUNK > height) ? (height - chunk_start) : ROWS_PER_CHUNK;
        
        // 为这一块分配缓冲区
        std::vector<uint8_t> chunk_data;
        chunk_data.reserve(chunk_rows * (width + 1));
        
        // 读取像素数据
        for (int y = chunk_start; y < chunk_start + chunk_rows; y++)
        {
            chunk_data.push_back(0); // 滤波器类型 0 (None)
            for (int x = 0; x < width; x++)
            {
                uint16_t color = g_canvas->readPixel(x, y);
                uint8_t gray4 = (color >> 8) & 0x0F;
                uint8_t gray = gray4 * 17;
                chunk_data.push_back(gray);
            }
        }
        
        // 更新 Adler-32 校验和
        for (size_t i = 0; i < chunk_data.size(); i++)
        {
            adler_a = (adler_a + chunk_data[i]) % MOD_ADLER;
            adler_b = (adler_b + adler_a) % MOD_ADLER;
        }
        
        // 写入无压缩块
        bool is_final = (chunk_start + chunk_rows >= height);
        uint8_t block_header = is_final ? 0x01 : 0x00;
        compressed_data.push_back(block_header);
        
        uint16_t block_size = chunk_data.size();
        compressed_data.push_back(block_size & 0xFF);
        compressed_data.push_back((block_size >> 8) & 0xFF);
        
        uint16_t nlen = ~block_size;
        compressed_data.push_back(nlen & 0xFF);
        compressed_data.push_back((nlen >> 8) & 0xFF);
        
        compressed_data.insert(compressed_data.end(), chunk_data.begin(), chunk_data.end());
        
        // 释放块数据内存
        chunk_data.clear();
        chunk_data.shrink_to_fit();
    }
    
    // Adler-32 校验和
    uint32_t checksum = (adler_b << 16) | adler_a;
    compressed_data.push_back((checksum >> 24) & 0xFF);
    compressed_data.push_back((checksum >> 16) & 0xFF);
    compressed_data.push_back((checksum >> 8) & 0xFF);
    compressed_data.push_back(checksum & 0xFF);

    // 写入 IDAT chunk
    std::vector<uint8_t> idat_chunk;
    PNGEncoder::write_be32(idat_chunk, compressed_data.size());
    idat_chunk.insert(idat_chunk.end(), {'I', 'D', 'A', 'T'});
    idat_chunk.insert(idat_chunk.end(), compressed_data.begin(), compressed_data.end());
    uint32_t idat_crc = PNGEncoder::crc(idat_chunk.data() + 4, idat_chunk.size() - 4);
    PNGEncoder::write_be32(idat_chunk, idat_crc);
    
    size_t written = file.write(idat_chunk.data(), idat_chunk.size());
    
    // 释放内存
    compressed_data.clear();
    compressed_data.shrink_to_fit();
    idat_chunk.clear();
    idat_chunk.shrink_to_fit();

    // 写入 IEND chunk
    const uint8_t iend_chunk[] = {
        0x00, 0x00, 0x00, 0x00, // 长度 = 0
        'I', 'E', 'N', 'D',
        0xAE, 0x42, 0x60, 0x82  // CRC
    };
    file.write(iend_chunk, sizeof(iend_chunk));
    
    size_t total_size = file.size();
    file.close();

#if DBG_SCREENSHOT
    Serial.printf("[SCREENSHOT] 截图成功: %s (%d bytes)\n", filename, total_size);
#endif

    return true;
}
