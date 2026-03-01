#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#if __has_include(<SD_MMC.h>)
#include <SD_MMC.h>
#define HAS_SD_MMC 1
#else
#define HAS_SD_MMC 0
#endif

namespace SDW
{
    enum Interface
    {
        IF_SPI = 0,
        IF_SDMMC = 1
    };

    class SDWrapper
    {
    public:
        SDWrapper();
        // begin with SPI parameters (csPin, SPIClass&, frequency) or choose sdmmc via iface
        // Default is SDMMC mode; will auto-fallback to SPI if SDMMC fails
        bool begin(uint8_t csPin = 5, SPIClass &spi = SPI, uint32_t freq = 40000000, Interface iface = IF_SDMMC);

        bool exists(const char *path);
        bool exists(const String &path) { return exists(path.c_str()); }
        bool mkdir(const char *path);
        bool mkdir(const String &path) { return mkdir(path.c_str()); }
        bool remove(const char *path);
        bool remove(const String &path) { return remove(path.c_str()); }
        bool rename(const char *oldPath, const char *newPath);
        bool rename(const String &oldPath, const String &newPath) { return rename(oldPath.c_str(), newPath.c_str()); }
        // open with explicit mode string (no default to avoid macro/type issues)
        File open(const char *path, const char *mode, const bool create = false);
        // convenience overload when only path is provided
        File open(const char *path);
        // String-friendly overloads
        File open(const String &path, const char *mode) { return open(path.c_str(), mode, false); }
        File open(const String &path) { return open(path.c_str(), "r", false); }
        // (removed uint8_t overloads to avoid macro/type conflicts)

        uint64_t cardSize();
        uint64_t usedBytes();
        uint64_t totalBytes();

        // expose underlying mode
        Interface currentInterface() const { return iface_; }

        // Low-level utility: read from file at specified offset into provided buffer
        // Returns number of bytes actually read
        size_t readAtOffset(File &f, size_t offset, uint8_t *buffer, size_t read_len);

        // Performance statistics
        void reset_readAtOffset_stats();
        void print_readAtOffset_stats();
        
        // 🔄 重新初始化SD卡以清除驱动状态（实验性）
        bool reinitialize();

        // Utility: perform a small non-DMA read at specified offset and log timing
        // Returns elapsed time in microseconds
        unsigned long benchmarkSmallRead(File &f, const char *path, size_t offset, size_t read_len);

    private:
        Interface iface_;
        bool initialized_;
        
        // DMA缓冲区池，避免频繁分配
        // 3个槽 × 8192字节(16扇区) = 24KB 内部SRAM，覆盖绝大多数随机读场景
        // 注意：MALLOC_CAP_DMA 强制使用内部SRAM，不走PSRAM，须控制总量
        static constexpr size_t DMA_POOL_SIZE = 3;
        static constexpr size_t DMA_BUFFER_SIZE = 8192; // 16个扇区
        uint8_t* dma_pool_[DMA_POOL_SIZE];
        bool dma_pool_in_use_[DMA_POOL_SIZE];
        
        uint8_t* allocate_dma_buffer();
        void free_dma_buffer(uint8_t* buf);
    };

    extern SDWrapper SD;
}
