#include "screenshot.h"
#include "SD/SDWrapper.h"
#include <time.h>
#include <vector>
#include "test/per_file_debug.h"
#include "text/bin_font_print.h"
#include "text/book_handle.h"
#include "ui/ui_lock_screen.h"
#include "current_book.h"
#include "tasks/state_machine_task.h"

#define USEBACK 1

extern M5Canvas *g_canvas;
extern GlobalConfig g_config;
extern float font_size;

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
}

// ensureScreenshotFolder() 已在 book_handle.cpp 中定义
// 在此只需要声明（通过包含 text/book_handle.h）
#include "text/book_handle.h"
#include "ui/ui_canvas_image.h"
// 需要检查当前系统状态以避免在 IDLE 状态启用背景
#include "tasks/state_machine_task.h"

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

    // 推送截图图标
    // ui_push_image_to_display_direct("/spiffs/screenshot.png", 270-32, 480-32);
    // 生成一个临时的 canvas（140x40），绘制背景并 push 到屏幕 (200,0)
    {
        M5Canvas tempCanvas(&M5.Display);
        if (tempCanvas.createSprite(180, 40))
        {
            tempCanvas.fillRect(0, 0, 180, 40, TFT_WHITE);
            bin_font_print("截图中", 32, 0, 180, 0, 4, false, &tempCanvas, TEXT_ALIGN_CENTER, 180, false, false, false, true);
            M5.Display.powerSaveOff();
            tempCanvas.pushSprite(180, 460);
            M5.Display.waitDisplay();
            M5.Display.powerSaveOn();
            tempCanvas.deleteSprite();
        }
    }

    // 获取当前时间
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    // 获取当前系统状态
    SystemState_t currentState = getCurrentSystemState();

    // 根据状态生成文件名前缀
    char prefix[96];
    if (currentState == STATE_READING)
    {
        // READING状态：使用当前书名作为前缀
        auto book = current_book_shared();
        if (book && book->isOpen())
        {
            std::string bookName = book->getBookName();
            // 移除扩展名
            size_t dotPos = bookName.find_last_of('.');
            if (dotPos != std::string::npos)
            {
                bookName = bookName.substr(0, dotPos);
            }
            snprintf(prefix, sizeof(prefix), "%s", bookName.c_str());
        }
        else
        {
            snprintf(prefix, sizeof(prefix), "READING");
        }
    }
    else
    {
        // 其他状态：使用状态枚举名
        const char *stateNames[] = {
            "IDLE", "DEBUG", "READING", "READING_QUICK_MENU",
            "HELP", "INDEX_DISPLAY", "TOC_DISPLAY", "MENU",
            "MAIN_MENU", "2ND_LEVEL_MENU", "WIRE_CONNECT",
            "USB_CONNECT", "SHUTDOWN", "SHOW_TIME_REC"};
        if (currentState >= 0 && currentState < sizeof(stateNames) / sizeof(stateNames[0]))
        {
            snprintf(prefix, sizeof(prefix), "%s", stateNames[currentState]);
        }
        else
        {
            snprintf(prefix, sizeof(prefix), "UNKNOWN");
        }
    }

    // 生成文件名
    char filename[256];
    snprintf(filename, sizeof(filename), "/screenshot/%s_%04d_%02d_%02d_%02d_%02d_%02d.png",
             prefix,
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
    ihdr_chunk.push_back(8);                                        // bit depth (8-bit)
    ihdr_chunk.push_back(3);                                        // color type (3 = indexed-color)
    ihdr_chunk.push_back(0);                                        // compression method
    ihdr_chunk.push_back(0);                                        // filter method
    ihdr_chunk.push_back(0);                                        // interlace method
    uint32_t ihdr_crc = PNGEncoder::crc(ihdr_chunk.data() + 4, 17); // 跳过长度字段
    PNGEncoder::write_be32(ihdr_chunk, ihdr_crc);
    file.write(ihdr_chunk.data(), ihdr_chunk.size());
    ihdr_chunk.clear();
    ihdr_chunk.shrink_to_fit();

    // 分块处理图像数据以减少内存占用
    const int ROWS_PER_CHUNK = 80; // 增加到80行以减少循环开销（~43KB/块，仍然安全）
    std::vector<uint8_t> compressed_data;
    // 预估压缩后大小：每像素 1 字节索引 + 每行 1 字节滤波器
    compressed_data.reserve(height * (width + 1) / 2);

    // 如果启用了背景叠加且处于非 dark 模式，尝试载入背景图到临时 canvas（放在 PLTE 之前以便采样）
    M5Canvas bgCanvas(&M5.Display);
    bool have_bg = false;
#if USEBACK
    // Only enable background when in READING state
    if (!g_config.dark && getCurrentSystemState() == STATE_READING)
    {
        if (bgCanvas.createSprite(width, height))
        {
            // Check for scback.png on SD root (use /sd/ prefix when calling ui helpers)
            File scf = SDW::SD.open("/scback.png", "r");
            if (scf)
            {
                scf.close();
                bgCanvas.fillRect(0, 0, width, height, TFT_WHITE);
                ui_push_image_to_canvas("/sd/scback.png", 0, 0, &bgCanvas, false);
                have_bg = true;
            }
            else
            {
                // no background file on SD; free the sprite and continue without background
                bgCanvas.deleteSprite();
            }
        }
    }
#endif

    // 加工g_canvas,
    if (currentState == STATE_READING)
        show_lockscreen(PAPER_S3_WIDTH, PAPER_S3_HEIGHT, 30, "SCREENSHOT", true, "top", true);

    // 自适应 256 色调色板：使用 12-bit 直方图 (r4,g4,b4 -> 4096 bins) 采样 g_canvas 与 scback（若存在）
    const int PALETTE_MAX = 256;
    const int HIST_SIZE = 4096; // 12-bit
    std::vector<uint32_t> hist(HIST_SIZE);
    std::fill(hist.begin(), hist.end(), 0);

    int sample_step = (width * height > 200000) ? 4 : 2;
    for (int y = 0; y < height; y += sample_step)
    {
        for (int x = 0; x < width; x += sample_step)
        {
            uint16_t color = g_canvas->readPixel(x, y);
            uint8_t r5 = (color >> 11) & 0x1F;
            uint8_t g6 = (color >> 5) & 0x3F;
            uint8_t b5 = color & 0x1F;
            uint8_t r4 = r5 >> 1;
            uint8_t g4 = g6 >> 2;
            uint8_t b4 = b5 >> 1;
            int idx12 = (r4 << 8) | (g4 << 4) | b4;
            hist[idx12]++;

            if (have_bg)
            {
                uint16_t bgc = bgCanvas.readPixel(x, y);
                uint8_t br5 = (bgc >> 11) & 0x1F;
                uint8_t bg6 = (bgc >> 5) & 0x3F;
                uint8_t bb5 = bgc & 0x1F;
                uint8_t br4 = br5 >> 1;
                uint8_t bg4 = bg6 >> 2;
                uint8_t bb4 = bb5 >> 1;
                int bidx12 = (br4 << 8) | (bg4 << 4) | bb4;
                hist[bidx12]++;
            }
        }
    }

    std::vector<std::pair<uint32_t, int>> freq;
    for (int i = 0; i < HIST_SIZE; ++i)
    {
        if (hist[i] > 0)
            freq.emplace_back(hist[i], i);
    }
    std::sort(freq.begin(), freq.end(), [](const std::pair<uint32_t, int> &a, const std::pair<uint32_t, int> &b)
              { return a.first > b.first; });

    int palCount = std::min((int)freq.size(), PALETTE_MAX);
    if (palCount == 0)
        palCount = 1;

    // 强制包含 RGB(100,100,100) 用于 lum==28 的特殊淡灰色
    uint8_t gray100_r4 = (uint8_t)((100 * 15) / 255); // ≈5
    uint8_t gray100_g4 = gray100_r4;
    uint8_t gray100_b4 = gray100_r4;
    int gray100_idx12 = (gray100_r4 << 8) | (gray100_g4 << 4) | gray100_b4;

    // 检查是否已在 freq 中
    bool has_gray100 = false;
    for (const auto &f : freq)
    {
        if (f.second == gray100_idx12)
        {
            has_gray100 = true;
            break;
        }
    }
    // 如果不存在且调色板未满，插入到 freq 开头（高优先级）
    if (!has_gray100 && palCount < PALETTE_MAX)
    {
        freq.insert(freq.begin(), {1, gray100_idx12});
        palCount++;
    }
    else if (!has_gray100 && palCount == PALETTE_MAX)
    {
        // 调色板已满，替换最后一个（频率最低）
        freq[palCount - 1] = {1, gray100_idx12};
    }

    std::vector<uint8_t> palette;
    palette.reserve(palCount * 3);
    std::vector<int> selected_indices;
    selected_indices.reserve(palCount);
    for (int i = 0; i < palCount; ++i)
    {
        int idx12 = freq[i].second;
        selected_indices.push_back(idx12);
        uint8_t r4 = (idx12 >> 8) & 0x0F;
        uint8_t g4 = (idx12 >> 4) & 0x0F;
        uint8_t b4 = idx12 & 0x0F;
        uint8_t r8 = (uint8_t)((r4 * 255 + 7) / 15);
        uint8_t g8 = (uint8_t)((g4 * 255 + 7) / 15);
        uint8_t b8 = (uint8_t)((b4 * 255 + 7) / 15);
        palette.push_back(r8);
        palette.push_back(g8);
        palette.push_back(b8);
    }

    std::vector<uint8_t> map12(HIST_SIZE, 0xFF);
    int gray100_pal_idx = -1;
    for (int i = 0; i < palCount; ++i)
    {
        map12[selected_indices[i]] = (uint8_t)i;
        if (selected_indices[i] == gray100_idx12)
            gray100_pal_idx = i;
    }
    for (int i = 0; i < HIST_SIZE; ++i)
    {
        if (map12[i] != 0xFF)
            continue;
        uint8_t r4 = (i >> 8) & 0x0F;
        uint8_t g4 = (i >> 4) & 0x0F;
        uint8_t b4 = i & 0x0F;
        int best = 0;
        int bestd = INT_MAX;
        for (int p = 0; p < palCount; ++p)
        {
            int pr4 = (selected_indices[p] >> 8) & 0x0F;
            int pg4 = (selected_indices[p] >> 4) & 0x0F;
            int pb4 = selected_indices[p] & 0x0F;
            int dr = (int)r4 - pr4;
            int dg = (int)g4 - pg4;
            int db = (int)b4 - pb4;
            int d = dr * dr + dg * dg + db * db;
            if (d < bestd)
            {
                bestd = d;
                best = p;
            }
        }
        map12[i] = (uint8_t)best;
    }

    // 写入 PLTE chunk
    std::vector<uint8_t> plte_chunk;
    PNGEncoder::write_be32(plte_chunk, (uint32_t)palette.size());
    plte_chunk.insert(plte_chunk.end(), {'P', 'L', 'T', 'E'});
    plte_chunk.insert(plte_chunk.end(), palette.begin(), palette.end());
    uint32_t plte_crc = PNGEncoder::crc(plte_chunk.data() + 4, 4 + palette.size());
    PNGEncoder::write_be32(plte_chunk, plte_crc);
    file.write(plte_chunk.data(), plte_chunk.size());

    // 构建 65536 大小的 rgb565 -> palette 索引查表，加速像素映射（约64KB）
    std::vector<uint8_t> rgb565_map(65536);
    for (int c = 0; c < 65536; ++c)
    {
        uint16_t color = (uint16_t)c;
        uint8_t r5 = (color >> 11) & 0x1F;
        uint8_t g6 = (color >> 5) & 0x3F;
        uint8_t b5 = color & 0x1F;
        uint8_t r4 = r5 >> 1;
        uint8_t g4 = g6 >> 2;
        uint8_t b4 = b5 >> 1;
        int idx12 = (r4 << 8) | (g4 << 4) | b4;
        rgb565_map[c] = map12[idx12];
    }

    // 使用流式写入 IDAT（多 IDAT 块），以减少峰值内存占用
    bool first_idat = true;
    uint32_t adler_a = 1;
    uint32_t adler_b = 0;
    const uint32_t MOD_ADLER = 65521;

    // 通用亮度到映射值的函数（不包含白色->背景特殊处理）
    auto map_lum_generic = [&](uint16_t lum) -> uint8_t
    {
        uint8_t mapped_lum_local;
        if (lum >= 210)
        {
            mapped_lum_local = (uint8_t)(170 + ((lum - 210) * 17) / 45);
        }
        else if (lum >= 180)
        {
            mapped_lum_local = (uint8_t)(153 + ((lum - 180) * 16) / 30);
        }
        else if (lum >= 130)
        {
            mapped_lum_local = (uint8_t)(134 + ((lum - 130) * 24) / 50);
        }
        else if (lum >= 100)
        {
            mapped_lum_local = (uint8_t)(102 + ((lum - 100) * 33) / 30);
        }
        else
        {
            mapped_lum_local = (uint8_t)lum;
        }
        /*        if (lum >= 10 && lum < 28)
                {
                    mapped_lum_local = 170;
                }
        */
        return mapped_lum_local;
    };

    // 预分配并复用 chunk_data，避免每次分配/释放
    std::vector<uint8_t> chunk_data;
    chunk_data.reserve(ROWS_PER_CHUNK * (1 + width));

    for (int chunk_start = 0; chunk_start < height; chunk_start += ROWS_PER_CHUNK)
    {
        int chunk_rows = (chunk_start + ROWS_PER_CHUNK > height) ? (height - chunk_start) : ROWS_PER_CHUNK;

        // 复用缓冲区
        chunk_data.clear();
        chunk_data.reserve(chunk_rows * (1 + width));

        // 读取像素数据
        for (int y = chunk_start; y < chunk_start + chunk_rows; y++)
        {
            chunk_data.push_back(0); // 滤波器类型 0 (None)
            for (int x = 0; x < width; x++)
            {
                // 从 Canvas 读取为 RGB565（uint16_t），并构造 12-bit 索引 (r4,g4,b4)
                uint16_t color = g_canvas->readPixel(x, y);
                uint8_t r5 = (color >> 11) & 0x1F;
                uint8_t g6 = (color >> 5) & 0x3F;
                uint8_t b5 = color & 0x1F;
                uint8_t r8 = (uint8_t)((r5 * 255 + 15) / 31);
                uint8_t g8 = (uint8_t)((g6 * 255 + 31) / 63);
                uint8_t b8 = (uint8_t)((b5 * 255 + 15) / 31);
                uint16_t lum = (uint16_t)((299 * r8 + 587 * g8 + 114 * b8 + 500) / 1000); // 0-255

                // 使用预计算的 rgb565 -> palette 查表以加速映射
                uint8_t pal_idx_fg = 0;
                if (!rgb565_map.empty())
                {
                    pal_idx_fg = rgb565_map[color];
                }
                else
                {
                    uint8_t r4 = r5 >> 1; // 0..15
                    uint8_t g4 = g6 >> 2; // 0..15
                    uint8_t b4 = b5 >> 1; // 0..15
                    int idx12_fg = (r4 << 8) | (g4 << 4) | b4;
                    pal_idx_fg = map12[idx12_fg];
                }

                if (have_bg)
                {
                    // 特殊处理：lum==28 强制映射到淡灰色 RGB(100,100,100)
                    if (lum == 28 && gray100_pal_idx >= 0)
                    {
                        chunk_data.push_back((uint8_t)gray100_pal_idx);
                    }
                    else
                    {
                        uint16_t bg_color = bgCanvas.readPixel(x, y);
                        uint8_t br5 = (bg_color >> 11) & 0x1F;
                        uint8_t bg6 = (bg_color >> 5) & 0x3F;
                        uint8_t bb5 = bg_color & 0x1F;
                        uint8_t br4 = br5 >> 1;
                        uint8_t bg4 = bg6 >> 2;
                        uint8_t bb4 = bb5 >> 1;
                        int idx12_bg = (br4 << 8) | (bg4 << 4) | bb4;
                        uint8_t pal_idx_bg = 0;
                        if (!rgb565_map.empty())
                            pal_idx_bg = rgb565_map[bg_color];
                        else
                            pal_idx_bg = map12[idx12_bg];

                        if (lum == 255)
                        {
                            chunk_data.push_back(pal_idx_bg);
                        }
                        else
                        {
                            chunk_data.push_back(pal_idx_fg);
                        }
                    }
                }
                else
                {
                    // 无背景：灰度量化后映射到 palette 空间
                    uint8_t mapped_here;
                    if (lum == 255)
                    {
                        mapped_here = 204;
                    }
                    else
                    {
                        mapped_here = map_lum_generic(lum);
                    }
                    uint8_t final_quant = (uint8_t)((mapped_here / 17) * 17);
                    // 特殊处理：lum==28 强制映射到淡灰色 RGB(100,100,100)
                    if (lum == 28 && gray100_pal_idx >= 0)
                    {
                        chunk_data.push_back((uint8_t)gray100_pal_idx);
                    }
                    else
                    {
                        uint8_t r4_g = (uint8_t)((final_quant * 15) / 255);
                        uint8_t g4_g = r4_g;
                        uint8_t b4_g = (uint8_t)((final_quant * 15) / 255);
                        int idx12_gray = (r4_g << 8) | (g4_g << 4) | b4_g;
                        chunk_data.push_back(map12[idx12_gray]);
                    }
                }
            }
        }

        // 更新 Adler-32 校验和
        for (size_t i = 0; i < chunk_data.size(); i++)
        {
            adler_a = (adler_a + chunk_data[i]) % MOD_ADLER;
            adler_b = (adler_b + adler_a) % MOD_ADLER;
        }

        // 写入无压缩块（分割为 <=65535 字节的子块，逐个写入 IDAT）
        bool chunk_is_final = (chunk_start + chunk_rows >= height);
        size_t offset = 0;
        const size_t MAX_BLOCK_SIZE = 65535;
        while (offset < chunk_data.size())
        {
            size_t sub_size = std::min(MAX_BLOCK_SIZE, chunk_data.size() - offset);
            bool sub_is_final = (chunk_is_final && (offset + sub_size >= chunk_data.size()));

            // 构造较小的 deflate 无压缩子块头（包含 zlib 头仅写入首个子块）
            std::vector<uint8_t> hdr;
            if (first_idat)
            {
                hdr.push_back(0x78);
                hdr.push_back(0x01);
                first_idat = false;
            }
            uint8_t block_header_sub = sub_is_final ? 0x01 : 0x00;
            hdr.push_back(block_header_sub);
            uint16_t sub_block_size16 = (uint16_t)sub_size;
            hdr.push_back(sub_block_size16 & 0xFF);
            hdr.push_back((sub_block_size16 >> 8) & 0xFF);
            uint16_t sub_nlen = ~sub_block_size16;
            hdr.push_back(sub_nlen & 0xFF);
            hdr.push_back((sub_nlen >> 8) & 0xFF);

            // IDAT chunk 长度 = hdr.size() + sub_size
            uint32_t data_len = (uint32_t)(hdr.size() + sub_size);
            uint8_t len_be[4];
            len_be[0] = (data_len >> 24) & 0xFF;
            len_be[1] = (data_len >> 16) & 0xFF;
            len_be[2] = (data_len >> 8) & 0xFF;
            len_be[3] = (data_len) & 0xFF;

            // 写入长度和类型
            file.write(len_be, 4);
            file.write((const uint8_t *)"IDAT", 4);

            // 增量计算 CRC（先 'IDAT'）
            uint32_t crc_val = PNGEncoder::update_crc(0xffffffffL, (const uint8_t *)"IDAT", 4);

            // 写入 hdr 并更新 CRC
            file.write(hdr.data(), hdr.size());
            crc_val = PNGEncoder::update_crc(crc_val, hdr.data(), hdr.size());

            // 写入数据块并更新 CRC
            file.write(chunk_data.data() + offset, sub_size);
            crc_val = PNGEncoder::update_crc(crc_val, chunk_data.data() + offset, sub_size);

            crc_val ^= 0xffffffffL;
            uint8_t crc_be[4];
            crc_be[0] = (crc_val >> 24) & 0xFF;
            crc_be[1] = (crc_val >> 16) & 0xFF;
            crc_be[2] = (crc_val >> 8) & 0xFF;
            crc_be[3] = (crc_val) & 0xFF;
            file.write(crc_be, 4);

            offset += sub_size;
        }

        // 释放块数据内存
        chunk_data.clear();
        chunk_data.shrink_to_fit();
    }

    // Adler-32 校验和（写入 zlib 流尾部）
    uint32_t checksum = (adler_b << 16) | adler_a;
    std::vector<uint8_t> adler_bytes;
    adler_bytes.push_back((checksum >> 24) & 0xFF);
    adler_bytes.push_back((checksum >> 16) & 0xFF);
    adler_bytes.push_back((checksum >> 8) & 0xFF);
    adler_bytes.push_back(checksum & 0xFF);

    // 将 Adler32 写为最后的 IDAT chunk
    std::vector<uint8_t> final_idat;
    PNGEncoder::write_be32(final_idat, (uint32_t)adler_bytes.size());
    final_idat.insert(final_idat.end(), {'I', 'D', 'A', 'T'});
    final_idat.insert(final_idat.end(), adler_bytes.begin(), adler_bytes.end());
    uint32_t final_crc = PNGEncoder::update_crc(0xffffffffL, (const uint8_t *)"IDAT", 4);
    final_crc = PNGEncoder::update_crc(final_crc, adler_bytes.data(), adler_bytes.size());
    final_crc ^= 0xffffffffL;
    PNGEncoder::write_be32(final_idat, final_crc);
    file.write(final_idat.data(), final_idat.size());

    if (have_bg)
    {
        bgCanvas.deleteSprite();
    }

    // 写入 IEND chunk
    const uint8_t iend_chunk[] = {
        0x00, 0x00, 0x00, 0x00, // 长度 = 0
        'I', 'E', 'N', 'D',
        0xAE, 0x42, 0x60, 0x82 // CRC
    };
    file.write(iend_chunk, sizeof(iend_chunk));

    size_t total_size = file.size();
    file.close();
    (void)total_size;

#if DBG_SCREENSHOT
    Serial.printf("[SCREENSHOT] 截图成功: %s (%d bytes)\n", filename, total_size);
#endif

    if (currentState == STATE_READING)
        g_current_book->renderCurrentPage(font_size);
    else
        bin_font_flush_canvas(false, false, true, NOEFFECT);

    return true;
}
