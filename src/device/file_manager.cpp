#include "file_manager.h"
#include "text/bin_font_print.h" // 提供 PSRAMAllocator
#include "readpaper.h"
#include "efficient_file_scanner.h"
#include <SPI.h>
#include <SPIFFS.h>
#include "SD/SDWrapper.h"
#include "papers3.h"
#include "powermgt.h"
#include <M5Unified.h>
#include "ui_display.h"
#include "test/per_file_debug.h"
#include "text/font_decoder.h"
#include "config/config_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <driver/sdmmc_host.h>
#include "sdmmc_cmd.h"
#include "internal_fs.h"

extern GlobalConfig g_config;

#if defined(ARDUINO_ARCH_ESP32)
static SPIClass s_sd_spi(HSPI);
#else
static SPIClass &s_sd_spi = SPI;
#endif

// Helper: ensure a fixed-size UTF-8 buffer does not end with a truncated multi-byte sequence
static void utf8_trim_tail(char *buf, size_t bufsize)
{
    if (!buf || bufsize == 0)
        return;
    buf[bufsize - 1] = '\0';
    size_t len = strnlen(buf, bufsize);
    if (len == 0)
        return;

    // Walk backwards past continuation bytes (0x80..0xBF)
    int i = (int)len - 1;
    while (i >= 0 && ((unsigned char)buf[i] & 0xC0) == 0x80)
    {
        --i;
    }
    if (i < 0)
    {
        buf[0] = '\0';
        return;
    }

    unsigned char lead = (unsigned char)buf[i];
    size_t expected_len = 1;
    if ((lead & 0x80) == 0x00)
        expected_len = 1;
    else if ((lead & 0xE0) == 0xC0)
        expected_len = 2;
    else if ((lead & 0xF0) == 0xE0)
        expected_len = 3;
    else if ((lead & 0xF8) == 0xF0)
        expected_len = 4;
    else
    {
        buf[i] = '\0';
        return;
    }

    size_t available = (size_t)len - (size_t)i;
    if (available < expected_len)
    {
        buf[i] = '\0';
    }
}

// 检查并创建必要的目录
void createRequiredDirectories()
{
    // 如果 SD 根目录没有 readpaper.cfg，则尝试从 SPIFFS 复制一份过去
    if (!SDW::SD.exists("/readpaper.cfg"))
    {
#if DBG_FILE_MANAGER
        Serial.println("[FS] /readpaper.cfg 未在 SD 上找到，尝试从 SPIFFS 复制...");
#endif
    if (SPIFFS.exists("/readpaper.cfg"))
        {
            File src = SPIFFS.open("/readpaper.cfg", "r");
            if (src)
            {
    File dst = SDW::SD.open("/readpaper.cfg", "w");
                if (dst)
                {
                    const size_t BUF_SIZE = 4096;
                    uint8_t buf[BUF_SIZE];
                    while (src.available())
                    {
                        size_t r = src.read(buf, BUF_SIZE);
                        if (r == 0)
                            break;
                        dst.write(buf, r);
                    }
                    dst.close();
#if DBG_FILE_MANAGER
                    Serial.println("[FS] 已将 /spiffs/readpaper.cfg 复制到 SD 根目录 /readpaper.cfg");
#endif
                }
                else
                {
#if DBG_FILE_MANAGER
                    Serial.println("[FS] 无法在 SD 上创建 /readpaper.cfg");
#endif
                }
                src.close();
            }
            else
            {
#if DBG_FILE_MANAGER
                Serial.println("[FS] 无法打开 SPIFFS:/readpaper.cfg 进行读取");
#endif
            }
        }
        else
        {
#if DBG_FILE_MANAGER
            Serial.println("[FS] SPIFFS 上不存在 /readpaper.cfg，跳过复制");
#endif
        }
    }
    const char *directories[] = {"/book", "/bookmarks", "/font", "/image", "/rdt"};
    const int numDirs = sizeof(directories) / sizeof(directories[0]);

    for (int i = 0; i < numDirs; i++)
    {
        if (!SDW::SD.exists(directories[i]))
        {
#if DBG_FILE_MANAGER
            Serial.printf("[FS] 创建目录: %s\n", directories[i]);
#endif
            if (SDW::SD.mkdir(directories[i]))
            {
#if DBG_FILE_MANAGER
                Serial.printf("[FS] 目录创建成功: %s\n", directories[i]);
#endif
            }
            else
            {
#if DBG_FILE_MANAGER
                Serial.printf("[FS] 目录创建失败: %s\n", directories[i]);
#endif
            }
        }
        else
        {
#if DBG_FILE_MANAGER
            Serial.printf("[FS] 目录已存在: %s\n", directories[i]);
#endif
        }
    }
}

bool init_filesystem()
{
#if DBG_FILE_MANAGER
    unsigned long fsStartTime = millis();
#endif

    // 初始化 SPIFFS
#if DBG_FILE_MANAGER
    Serial.printf("[FS] 开始初始化SPIFFS...\n");
#endif
        if (!InternalFS::begin(true))
        {
    #if DBG_FILE_MANAGER
        Serial.println("[FS] Failed to mount internal flash filesystem");
    #endif
        return false;
        }
#if DBG_FILE_MANAGER
    Serial.printf("[FS] SPIFFS初始化完成: %lu ms\n", millis() - fsStartTime);
#endif

    // SD Card Initialization - 优化：设置更快的SPI频率
#if DBG_FILE_MANAGER
    Serial.printf("[FS] 开始初始化SD卡...\n");
#endif
   // 尝试更高的SPI频率以提升SD卡初始化速度
#if DBG_FILE_MANAGER
    Serial.printf("[FS][DBG] millis=%lu freeHeap=%u\n", millis(), (unsigned)ESP.getFreeHeap());
    UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
    Serial.printf("[FS][DBG] MainTask stack high water mark (words): %u\n", (unsigned)highWater);
    Serial.printf("[FS][DBG] Calling SDW::SD.begin(cs=%d, freq=%u)\n", SD_SPI_CS_PIN, 40000000u);
#endif

    // 优先尝试使用 SD_MMC：仅在编译时显式启用（-DUSE_SD_MMC）且库可用时才尝试
    bool sd_ok = false;
#if HAS_SD_MMC && defined(USE_SD_MMC)
    // SD_MMC 中 begin() 不需要 SPI/CS/freq 参数；使用 iface 参数告诉 wrapper 使用 SDMMC
    sd_ok = SDW::SD.begin(0, s_sd_spi, 0, SDW::IF_SDMMC);
#if DBG_FILE_MANAGER
    Serial.printf("[FS][DBG] Attempted SDW::SD.begin(IF_SDMMC) returned: %s\n", sd_ok ? "true" : "false");
#endif
#else
#if DBG_FILE_MANAGER
    Serial.println("[FS][DBG] SD_MMC attempt skipped (compile without -DUSE_SD_MMC to enable)");
#endif
#endif

    // 如果 SD_MMC 不可用或初始化失败，回退到原来的 SPI 多频率尝试
    if (!sd_ok)
    {
        // 尝试多个 SPI 频率（从高到低）以避免与板上 flash/SPI 总线冲突导致的异常
        const uint32_t freqs[] = {40000000u, 25000000u, 8000000u};
        for (size_t fi = 0; fi < sizeof(freqs) / sizeof(freqs[0]); ++fi)
        {
            uint32_t f = freqs[fi];
#if DBG_FILE_MANAGER
            Serial.printf("[FS][DBG] Attempting SDW::SD.begin with freq=%u\n", (unsigned)f);
#endif
#if DBG_FILE_MANAGER
            Serial.printf("[FS][DBG] Calling SDW::SD.begin(cs=%d, freq=%u)\n", SD_SPI_CS_PIN, (unsigned)f);
#endif
            sd_ok = SDW::SD.begin(SD_SPI_CS_PIN, s_sd_spi, f, SDW::IF_SPI);
#if DBG_FILE_MANAGER
            Serial.printf("[FS][DBG] SDW::SD.begin(freq=%u) returned: %s\n", (unsigned)f, sd_ok ? "true" : "false");
#endif
            if (sd_ok)
                break;
            delay(50); // small pause between attempts
        }
    }

#if DBG_FILE_MANAGER
    Serial.printf("[FS][DBG] Final SDW::SD.begin result: %s\n", sd_ok ? "true" : "false");
    Serial.printf("[FS][DBG] After SDW::SD.begin millis=%lu freeHeap=%u\n", millis(), (unsigned)ESP.getFreeHeap());
    highWater = uxTaskGetStackHighWaterMark(NULL);
    Serial.printf("[FS][DBG] After SDW::SD.begin MainTask stack high water mark (words): %u\n", (unsigned)highWater);
#endif

    if (!sd_ok)
    {
#if DBG_FILE_MANAGER
        Serial.println("[FS] SD初始失败...");
#endif
        show_shutdown_low("icon-sd.png", "SDCard Needed", 100, 100);
        return false;
    }

    // SDMMC host probing removed to keep compatibility across builds
    // (not required for normal SD SPI operation used by this project)

#if DBG_FILE_MANAGER
    Serial.printf("[FS] SD卡初始化完成: %lu ms\n", millis() - fsStartTime);

    Serial.printf("[FS] 文件系统总初始化时间: %lu ms\n", millis() - fsStartTime);
#endif

    // 检查并创建必要的目录
    createRequiredDirectories();
    // 清除默认文件 /ReadPaper.txt 的书签文件
    // 这些文件位于 SD 卡的 /bookmarks/ 目录下
    const char* bookmark_files[] = {
        "/bookmarks/_spiffs_ReadPaper.bm",
        "/bookmarks/_spiffs_ReadPaper.rec",
        "/bookmarks/_spiffs_ReadPaper.complete",
        "/bookmarks/_spiffs_ReadPaper.page",
        "/bookmarks/_spiffs_ReadPaper.tags"
    };
    
    for (size_t i = 0; i < sizeof(bookmark_files) / sizeof(bookmark_files[0]); ++i)
    {
        if (SDW::SD.exists(bookmark_files[i]))
        {
#if DBG_FILE_MANAGER
            Serial.printf("[FS] 清除默认书签文件: %s\n", bookmark_files[i]);
#endif
            SDW::SD.remove(bookmark_files[i]);
        }
    }
    // 初始化完成后，刷新全局字体列表
    // font_list_scan();

    return true;
}

int list_root_files()
{
    File root = SDW::SD.open("/", "r");
    if (!root)
    {
#if DBG_FILE_MANAGER
        Serial.println("无法打开根目录");
#endif
        return 0;
    }

    root.close();

    // 使用高效文件扫描器计数文件
    int fileCount = EfficientFileScanner::countFiles("/");

    // 如果需要详细输出，使用高效扫描
    if (fileCount > 0)
    {
        std::vector<FileInfo> allFiles = EfficientFileScanner::scanDirectory("/");
        for (const auto &fileInfo : allFiles)
        {
            Serial.print(fileInfo.name.c_str());
            if (fileInfo.isDirectory)
            {
                Serial.println("/");
            }
            else
            {
                Serial.print("\t");
                Serial.println((int)fileInfo.size, DEC);
            }
        }
    }

#if DBG_FILE_MANAGER
    Serial.print("总文件数: ");
    Serial.println(fileCount);
#endif
    return fileCount;
}

// 显示 SPIFFS 文件系统中的文件
void display_spiffs_files()
{
    // 初始化 SPIFFS
        if (!InternalFS::begin(false))
        {
    #if DBG_FILE_MANAGER
        Serial.println("Failed to mount internal flash filesystem.");
    #endif
        return;
        }

#if DBG_FILE_MANAGER
    // 获取 SPIFFS 分区信息
    size_t totalBytes, usedBytes;
    totalBytes = SPIFFS.totalBytes();
    usedBytes = SPIFFS.usedBytes();

    // 打印 SPIFFS 分区信息
    Serial.print("SPIFFS Total Size: ");
    Serial.print(totalBytes);
    Serial.println(" bytes");

    Serial.print("SPIFFS Used Size: ");
    Serial.print(usedBytes);
    Serial.println(" bytes");
#endif

    // 打印 SPIFFS 文件系统中的文件 - 使用 EfficientFileScanner 的 SPIFFS 扫描方法
    std::vector<FileInfo> spiffsFiles = EfficientFileScanner::scanSPIFFSDirectory(std::string("/"));
#if DBG_FILE_MANAGER
    Serial.println("Files in SPIFFS:");
    for (const auto &fi : spiffsFiles)
    {
        Serial.print("File: ");
        Serial.print(fi.path.c_str());
        Serial.print(" Size: ");
        Serial.print((int)fi.size);
        Serial.println(" bytes");
    }
#endif
}

// 读取bin字体文件头部信息
bool read_font_header(const std::string &filepath, FontFileInfo &info)
{
    File file;
    std::string real_path;
    bool use_spiffs = false;

    if (!resolve_fake_path(filepath, real_path, use_spiffs))
    {
        return false;
    }

    if (use_spiffs)
    {
        file = SPIFFS.open(real_path.c_str(), "r");
    }
    else
    {
        file = SDW::SD.open(real_path.c_str(), "r");
    }

    if (!file)
    {
        return false;
    }

    // 检查文件最小尺寸（134字节头部）
    size_t file_size = file.size();
    if (file_size < 134)
    {
        file.close();
        return false;
    }

    // 读取前6字节头部
    // ensure we start reading from file beginning
    file.seek(0);

    uint8_t header[6];
    if (file.read(header, 6) != 6)
    {
        file.close();
        return false;
    }

    // 解析头部信息
    uint32_t char_count = *(uint32_t *)&header[0];
    uint8_t font_height = header[4];
    uint8_t version = header[5];

    // 调试输出：打印 header 基本信息，方便验证上传的字体文件
//    Serial.printf("[FONT_SCAN_INFO] path=%s size=%u char_count=%u font_height=%u version=%u\n",
 //                 real_path.c_str(), (unsigned)file_size, (unsigned)char_count, (unsigned)font_height, (unsigned)version);

    // 验证是否为有效的字体格式（支持 V2 的 1bit 格式与 V3 的 2bit Huffman）
    bool valid = true;
    if (char_count == 0 || char_count > 50000)
        valid = false;

    if (version == 2)
    {
        // V2: 1bit 旧格式，常见字体高度在 20..50 之间
        if (font_height < 20 || font_height > 50)
            valid = false;
    }
    else if (version == 3)
    {
        // V3: 支持更宽范围的字体高度（小字号也可能），允许 8..200
        if (font_height < 8 || font_height > 200)
            valid = false;
    }
    else
    {
        valid = false;
    }

    if (!valid)
    {
        file.close();
        return false;
    }

    // 读取字体族名（64字节）
    char family_name[64];
    memset(family_name, 0, sizeof(family_name));
    if (file.read((uint8_t *)family_name, 64) != 64)
    {
        file.close();
        return false;
    }
    // Ensure null-termination and trim incomplete UTF-8 tail
    family_name[63] = '\0';
    utf8_trim_tail(family_name, sizeof(family_name));

#if DBG_FILE_MANAGER
    // 打印原始族名字节并用 utf8_decode 逐个解析 codepoint，便于与生成端比对
    Serial.print("[FONT_SCAN_DBG] family raw bytes: ");
    const uint8_t *family_raw = (const uint8_t *)family_name;
    for (size_t i = 0; i < 64; ++i)
    {
        Serial.printf("%02X", family_raw[i]);
        if (i + 1 < 64)
            Serial.print(" ");
    }
    Serial.println();

    Serial.print("[FONT_SCAN_DBG] family decoded codepoints: ");
    const uint8_t *utf8ptr = (const uint8_t *)family_name;
    const uint8_t *utf8end = (const uint8_t *)family_name + 64;
    while (utf8ptr < utf8end && *utf8ptr)
    {
        const uint8_t *prev = utf8ptr;
        uint32_t cp = utf8_decode(utf8ptr, utf8end);
        if (cp == 0)
        {
            Serial.print("[FONT_SCAN_DBG] <invalid utf8> ");
            break;
        }
        Serial.printf("U+%04X ", cp);
        if (utf8ptr <= prev)
            break;
    }
    Serial.println();
#endif

    // 读取字体样式名（64字节）
    char style_name[64];
    memset(style_name, 0, sizeof(style_name));
    if (file.read((uint8_t *)style_name, 64) != 64)
    {
        file.close();
        return false;
    }
    style_name[63] = '\0';
    utf8_trim_tail(style_name, sizeof(style_name));

#if DBG_FILE_MANAGER
    Serial.print("[FONT_SCAN_DBG] style raw bytes: ");
    const uint8_t *style_raw = (const uint8_t *)style_name;
    for (size_t i = 0; i < 64; ++i)
    {
        Serial.printf("%02X", style_raw[i]);
        if (i + 1 < 64)
            Serial.print(" ");
    }
    Serial.println();

    Serial.print("[FONT_SCAN_DBG] style decoded codepoints: ");
    utf8ptr = (const uint8_t *)style_name;
    utf8end = (const uint8_t *)style_name + 64;
    while (utf8ptr < utf8end && *utf8ptr)
    {
        const uint8_t *prev = utf8ptr;
        uint32_t cp = utf8_decode(utf8ptr, utf8end);
        if (cp == 0)
        {
            Serial.print("[FONT_SCAN_DBG] <invalid utf8> ");
            break;
        }
        Serial.printf("U+%04X ", cp);
        if (utf8ptr <= prev)
            break;
    }
    Serial.println();
#endif

    // 填充信息结构体
    info.path = filepath;
    info.family_name = std::string(family_name);
    info.style_name = std::string(style_name);
    info.font_size = font_height;
    info.version = version;
    info.file_size = file_size;

    file.close();
    return true;
}

std::vector<FontFileInfo, PSRAMAllocator<FontFileInfo>> scan_font_files()
{
    std::vector<FontFileInfo, PSRAMAllocator<FontFileInfo>> fonts;

    // 检查/font目录是否存在
    if (!SDW::SD.exists("/font"))
    {
#if DBG_FILE_MANAGER
        Serial.println("[FONT_SCAN] /font 目录不存在");
#endif
        return fonts;
    }

    // 使用 EfficientFileScanner 扫描字体目录（不需要打开目录句柄）
#if DBG_FILE_MANAGER
    Serial.println("[FONT_SCAN] 使用 EfficientFileScanner 扫描字体目录...");
#endif
    std::vector<FileInfo> files = EfficientFileScanner::scanDirectory("/font");

    for (const auto &fi : files)
    {
        // 检查内存使用情况，避免内存不足
        if (ESP.getFreeHeap() < 8192)
        { // 保留至少8KB内存
#if DBG_FILE_MANAGER
            Serial.println("[FONT_SCAN] 内存不足，停止扫描");
#endif
            break;
        }

        // 防止处理过多字体文件 - 限制最多MAX_MAIN_MENU_FILE_COUNT个字体文件
        if (fonts.size() >= MAX_MAIN_MENU_FILE_COUNT)
        {
#if DBG_FILE_MANAGER
            Serial.printf("[FONT_SCAN] 已达到%d个字体文件限制，停止处理\n", MAX_MAIN_MENU_FILE_COUNT);
#endif
            break;
        }

        if (!fi.isDirectory)
        {
            std::string filename = fi.name;

            // 检查是否为.bin文件
            if (filename.length() > 4 && filename.substr(filename.length() - 4) == ".bin")
            {
                // file name may already include the directory prefix (/font/xxx.bin)
                std::string filepath;
                if (filename.rfind("/", 0) == 0)
                {
                    filepath = filename; // already absolute
                }
                else
                {
                    filepath = std::string("/font/") + filename;
                }

                FontFileInfo info;
                if (read_font_header(filepath, info))
                {
                    info.path = std::string("/sd") + filepath;
                    fonts.push_back(info);
#if DBG_FILE_MANAGER
                    Serial.printf("[FONT_SCAN_DBG] read header: path=%s, family='%s', style='%s', size=%u\n",
                                  filepath.c_str(), info.family_name.c_str(), info.style_name.c_str(), info.font_size);
                    Serial.printf("[FONT_SCAN] 找到字体: %s - %s %s (版本%u, %u像素)\n",
                                  filename.c_str(),
                                  info.family_name.c_str(),
                                  info.style_name.c_str(),
                                  info.version,
                                  info.font_size);
#endif
                }
                else
                {
#if DBG_FILE_MANAGER
                    Serial.printf("[FONT_SCAN] 跳过无效字体文件: %s\n", filename.c_str());
#endif
                }
            }
        }
    }

#if DBG_FILE_MANAGER
    Serial.printf("[FONT_SCAN] 扫描完成，找到 %d 个有效字体文件\n", fonts.size());
#endif

    // 尝试加载内置默认字体位于 SPIFFS 的 /spiffs/lite.bin，若存在则插入到向量首位
    {
        FontFileInfo liteInfo;
        const std::string litePath = "/spiffs/lite.bin";
        if (read_font_header(litePath, liteInfo))
        {
            // 确保路径标记为 SPIFFS
            liteInfo.path = litePath;
            // 插入到首位
            fonts.insert(fonts.begin(), liteInfo);
#if DBG_FILE_MANAGER
            Serial.println("[FONT_SCAN] 已插入 SPIFFS 默认字体 /spiffs/lite.bin 到列表首位");
#endif
        }
    }

    // 如果 g_config.fontset 指向某个已扫描的字体路径，则将该项移动到首位，方便首屏显示
    if (fonts.size() > 1)
    {
        std::string cfgPath = std::string(g_config.fontset);
        if (!cfgPath.empty())
        {
            // 规范化配置中的路径，以便与 scan 得到的 fonts[].path 形式一致
            std::string normCfg = cfgPath;
            // 如果配置仅以 /font/ 开头，而扫描时会把 SD 字体路径记录为 /sd/font/..., 则补上 /sd 前缀
            if (cfgPath.rfind("/font/", 0) == 0)
            {
                normCfg = std::string("/sd") + cfgPath;
            }

#if DBG_FILE_MANAGER
            Serial.printf("[FONT_SCAN_DBG] cfgPath='%s' normCfg='%s'\n", cfgPath.c_str(), normCfg.c_str());
#endif

            for (size_t i = 0; i < fonts.size(); ++i)
            {
#if DBG_FILE_MANAGER
                Serial.printf("[FONT_SCAN_DBG] comparing with fonts[%u].path='%s'\n", (unsigned)i, fonts[i].path.c_str());
#endif
                if (fonts[i].path == normCfg)
                {
                    if (i != 0)
                    {
                        FontFileInfo fav = fonts[i];
                        fonts.erase(fonts.begin() + i);
                        fonts.insert(fonts.begin(), fav);
                    }
                    break;
                }
            }
        }
    }

    return fonts;
}

// 将带有伪前缀的路径（/sd/... 或 /spiffs/...）解析为真实路径并返回应使用的文件系统
bool resolve_fake_path(const std::string &fake_path, std::string &out_real_path, bool &out_use_spiffs)
{
    if (fake_path.empty())
        return false;

    if (fake_path.rfind("/spiffs/", 0) == 0)
    {
        // strip '/spiffs' prefix (7 chars), keep the leading '/'
        out_real_path = std::string(fake_path.c_str() + 7);
        out_use_spiffs = true;
        return true;
    }
    if (fake_path.rfind("/sd/", 0) == 0)
    {
        // strip '/sd' prefix (3 chars), keep the leading '/'
        out_real_path = std::string(fake_path.c_str() + 3);
        out_use_spiffs = false;
        return true;
    }

    out_real_path = fake_path;
    // If no /font in, then by default assuming in SPIFFS (i.e. useing cpp)
    if (fake_path.rfind("/font/", 0) == 0)
    {
        out_use_spiffs = false;
    }
    else
    {
        out_use_spiffs = true;
    }
    return true;
}

// 全局字体列表定义（使用 PSRAM 分配）
std::vector<FontFileInfo, PSRAMAllocator<FontFileInfo>> g_font_list;

// 刷新全局字体列表
void font_list_scan()
{
    g_font_list = scan_font_files();

    // 如果 g_config.fontset 指定了首选字体路径，则在全局列表中查找并将其移动到首位
    std::string cfgPath = std::string(g_config.fontset);
    if (!cfgPath.empty())
    {
        // 规范化 cfgPath 同样规则（/font/ -> /sd/font/）以匹配 fonts[].path
        std::string normCfg = cfgPath;
        if (cfgPath.rfind("/font/", 0) == 0)
        {
            normCfg = std::string("/sd") + cfgPath;
        }
#if DBG_FILE_MANAGER
        Serial.printf("[FONT_SCAN_DBG] font_list_scan cfgPath='%s' normCfg='%s'\n", cfgPath.c_str(), normCfg.c_str());
#endif
        for (size_t i = 0; i < g_font_list.size(); ++i)
        {
            if (g_font_list[i].path == normCfg)
            {
                update_font_list_order((int16_t)i);
                break;
            }
        }
    }
}

// 将 g_font_list 中指定索引的字体移动到首位（索引基于当前 g_font_list）
void update_font_list_order(int16_t ind)
{
    if (ind < 0)
        return;
    size_t uind = (size_t)ind;
    if (uind >= g_font_list.size())
        return;
    if (uind == 0)
        return; // 已经在首位

    FontFileInfo item = g_font_list[uind];
    g_font_list.erase(g_font_list.begin() + uind);
    g_font_list.insert(g_font_list.begin(), item);
}