#include "SDWrapper.h"
#include <SPI.h>
#include <Arduino.h>
#include "papers3.h"
#include <M5Unified.h>
#include "test/per_file_debug.h"
#include <vector>

#if HAS_SD_MMC
#include "driver/sdmmc_host.h"  // For SDMMC_FREQ_HIGHSPEED constant
#ifndef SDMMC_FREQ_PROBING
#define SDMMC_FREQ_PROBING (SDMMC_FREQ_DEFAULT / 2)
#endif
#endif

namespace SDW
{

    SDWrapper SD;

    SDWrapper::SDWrapper() : iface_(IF_SPI), initialized_(false) 
    {
        // 初始化DMA缓冲区池
        for (size_t i = 0; i < DMA_POOL_SIZE; i++)
        {
            dma_pool_[i] = nullptr;
            dma_pool_in_use_[i] = false;
        }
    }
    
    uint8_t* SDWrapper::allocate_dma_buffer()
    {
        // 尝试从池中获取
        for (size_t i = 0; i < DMA_POOL_SIZE; i++)
        {
            if (dma_pool_[i] && !dma_pool_in_use_[i])
            {
                dma_pool_in_use_[i] = true;
                return dma_pool_[i];
            }
        }
        
        // 池中没有可用的，尝试分配新的
        for (size_t i = 0; i < DMA_POOL_SIZE; i++)
        {
            if (!dma_pool_[i])
            {
#if defined(ESP_PLATFORM) || defined(ESP32)
                dma_pool_[i] = (uint8_t*)heap_caps_malloc(DMA_BUFFER_SIZE, MALLOC_CAP_DMA);
                if (dma_pool_[i])
                {
                    dma_pool_in_use_[i] = true;
                    return dma_pool_[i];
                }
#endif
            }
        }
        
        // 池已满或分配失败，返回nullptr让调用者使用fallback
        return nullptr;
    }
    
    void SDWrapper::free_dma_buffer(uint8_t* buf)
    {
        if (!buf) return;
        
        // 标记为可用，但不真正释放内存
        for (size_t i = 0; i < DMA_POOL_SIZE; i++)
        {
            if (dma_pool_[i] == buf)
            {
                dma_pool_in_use_[i] = false;
                return;
            }
        }
    }

    bool SDWrapper::begin(uint8_t csPin, SPIClass &spi, uint32_t freq, Interface iface)
    {
        // If already initialized, return success without re-initializing
        if (initialized_)
        {
#if DBG_FILE_MANAGER
            Serial.printf("[SDW] begin: already initialized (interface=%s), skipping\n", 
                         iface_ == IF_SDMMC ? "SDMMC" : "SPI");
#endif
            return true;
        }

        iface_ = iface;
#if HAS_SD_MMC
        if (iface_ == IF_SDMMC)
        {
#if DBG_FILE_MANAGER
            Serial.println("[SDW] begin: attempting SD_MMC interface");
#endif

#ifdef SOC_SDMMC_USE_GPIO_MATRIX
            ::SD_MMC.setPins(SD_SPI_SCK_PIN, SD_SPI_MOSI_PIN, SD_SPI_MISO_PIN);
#if DBG_FILE_MANAGER
            Serial.printf("[SDW] SD_MMC pins configured: CLK=%d, CMD=%d, D0=%d\n",
                          SD_SPI_SCK_PIN, SD_SPI_MOSI_PIN, SD_SPI_MISO_PIN);
#endif
#endif

            const uint32_t freq_candidates[] = {
                SDMMC_FREQ_HIGHSPEED,
                SDMMC_FREQ_DEFAULT,
                SDMMC_FREQ_PROBING};
            for (uint32_t candidate : freq_candidates)
            {
#if DBG_FILE_MANAGER
                Serial.printf("[SDW] SD_MMC.begin at %u Hz\n", (unsigned)candidate);
#endif
                bool ok = ::SD_MMC.begin("/sdcard", true, false, candidate, 20);
                if (ok)
                {
                    initialized_ = true;
                    return true;
                }
            }

#if DBG_FILE_MANAGER
            Serial.println("[SDW] All SD_MMC attempts failed, falling back to SPI mode");
#endif
            iface_ = IF_SPI;
        }
#else
        if (iface_ == IF_SDMMC)
        {
            iface_ = IF_SPI;
        }
#endif

        // SPI fallback path
        spi.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, csPin);
#if DBG_FILE_MANAGER
        Serial.printf("[SDW] begin: using SPI interface cs=%d freq=%u\n", csPin, (unsigned)freq);
#endif
        bool r = ::SD.begin(csPin, spi, freq);
#if DBG_FILE_MANAGER
        Serial.printf("[SDW] SDW::SD.begin returned: %s\n", r ? "true" : "false");
#endif
        if (r)
        {
            initialized_ = true;
        }
        return r;
    }

    bool SDWrapper::exists(const char *path)
    {
#if HAS_SD_MMC
        if (iface_ == IF_SDMMC)
        {
#if DBG_FILE_MANAGER
            Serial.printf("[SDW] exists() called with: %s (SDMMC mode)\n", path);
#endif
            bool result = ::SD_MMC.exists(path);
#if DBG_FILE_MANAGER
            if (!result)
            {
                Serial.printf("[SDW] exists() returned false for: %s (may not exist or FS error)\n", path);
            }
#endif
            return result;
        }
#endif
#if DBG_FILE_MANAGER
        Serial.printf("[SDW] exists() called with: %s (SPI mode)\n", path);
#endif
        return ::SD.exists(path);
    }

    bool SDWrapper::mkdir(const char *path)
    {
#if HAS_SD_MMC
        if (iface_ == IF_SDMMC)
        {
#if DBG_FILE_MANAGER
            Serial.printf("[SDW] mkdir() called with: %s (SDMMC mode)\n", path);
#endif
            return ::SD_MMC.mkdir(path);
        }
#endif
#if DBG_FILE_MANAGER
        Serial.printf("[SDW] mkdir() called with: %s (SPI mode)\n", path);
#endif
        return ::SD.mkdir(path);
    }

    bool SDWrapper::remove(const char *path)
    {
#if HAS_SD_MMC
        if (iface_ == IF_SDMMC)
        {
#if DBG_FILE_MANAGER
            Serial.printf("[SDW] remove() called with: %s (SDMMC mode)\n", path);
#endif
            return ::SD_MMC.remove(path);
        }
#endif
#if DBG_FILE_MANAGER
        Serial.printf("[SDW] remove() called with: %s (SPI mode)\n", path);
#endif
        return ::SD.remove(path);
    }

    bool SDWrapper::rename(const char *oldPath, const char *newPath)
    {
#if HAS_SD_MMC
        if (iface_ == IF_SDMMC)
        {
#if DBG_FILE_MANAGER
            Serial.printf("[SDW] rename() called with: %s -> %s (SDMMC mode)\n", oldPath, newPath);
#endif
            return ::SD_MMC.rename(oldPath, newPath);
        }
#endif
#if DBG_FILE_MANAGER
        Serial.printf("[SDW] rename() called with: %s -> %s (SPI mode)\n", oldPath, newPath);
#endif
        return ::SD.rename(oldPath, newPath);
    }

    File SDWrapper::open(const char *path, const char *mode, const bool create)
    {
#if HAS_SD_MMC
        if (iface_ == IF_SDMMC)
        {
#if DBG_FILE_MANAGER
            Serial.printf("[SDW] open() called with: %s (mode: %s) (SDMMC mode)\n", path, mode);
#endif
            File f = ::SD_MMC.open(path, mode);
#if DBG_FILE_MANAGER
            if (!f)
            {
                Serial.printf("[SDW] open() FAILED for: %s (mode: %s) - file handle invalid\n", path, mode);
            }
#endif
            return f;
        }
#endif
#if DBG_FILE_MANAGER
        Serial.printf("[SDW] open() called with: %s (mode: %s, create: %d) (SPI mode)\n", 
                     path, mode, create);
#endif
        return ::SD.open(path, mode, create);
    }

    File SDWrapper::open(const char *path)
    {
        return open(path, "r", false);
    }

    // uint8_t overloads removed due to macro/type differences across frameworks

    uint64_t SDWrapper::cardSize()
    {
#if HAS_SD_MMC
        if (iface_ == IF_SDMMC)
        {
            return ::SD_MMC.cardSize();
        }
#endif
        return ::SD.cardSize();
    }

    uint64_t SDWrapper::usedBytes()
    {
#if HAS_SD_MMC
        if (iface_ == IF_SDMMC)
        {
            return ::SD_MMC.usedBytes();
        }
#endif
        return ::SD.usedBytes();
    }

    uint64_t SDWrapper::totalBytes()
    {
#if HAS_SD_MMC
        if (iface_ == IF_SDMMC)
        {
            return ::SD_MMC.totalBytes();
        }
#endif
        return ::SD.totalBytes();
    }

    // 全局统计：readAtOffset性能数据
    static uint32_t g_readAtOffset_total_us = 0;
    static uint32_t g_readAtOffset_count = 0;
    static uint32_t g_readAtOffset_seek_us = 0;
    static uint32_t g_readAtOffset_read_us = 0;
    
    size_t SDWrapper::readAtOffset(File &f, size_t offset, uint8_t *buffer, size_t read_len)
    {
        // Low-level utility to read from file at specified offset
        if (!f || !buffer || read_len == 0)
        {
            return 0;
        }
        
        uint32_t t_total_start = micros();
        
        // 🚀 DMA优化：对于SD_MMC接口，使用扇区对齐的DMA读取
#if HAS_SD_MMC
        if (iface_ == IF_SDMMC && read_len <= 16 * 1024)  // 最大16KB
        {
            const size_t SECTOR = 512;
            size_t sector_idx = offset / SECTOR;
            size_t aligned_offset = sector_idx * SECTOR;
            size_t start_in_sector = offset - aligned_offset;
            
            // 计算需要读取的扇区数
            size_t bytes_needed = start_in_sector + read_len;
            size_t sectors_needed = (bytes_needed + SECTOR - 1) / SECTOR;
            
            // 优先使用固定大小的DMA缓冲池（避免频繁分配）
            if (sectors_needed <= 8)  // DMA_BUFFER_SIZE = 4096 = 8扇区
            {
                uint8_t* dma_buf = allocate_dma_buffer();
                if (dma_buf)
                {
                    // 成功获取DMA缓冲区
                    size_t aligned_size = sectors_needed * SECTOR;
                    f.seek(aligned_offset);
                    uint32_t t_seek_end = micros();
                    
                    size_t got = f.read(dma_buf, aligned_size);
                    uint32_t t_read_end = micros();
                    
                    if (got >= start_in_sector + read_len)
                    {
                        // 读取成功，复制用户需要的部分
                        memcpy(buffer, dma_buf + start_in_sector, read_len);
                        free_dma_buffer(dma_buf);  // 归还到池中
                        
                        uint32_t seek_us = t_seek_end - t_total_start;
                        uint32_t read_us = t_read_end - t_seek_end;
                        uint32_t total_us = t_read_end - t_total_start;
                        
                        g_readAtOffset_total_us += total_us;
                        g_readAtOffset_seek_us += seek_us;
                        g_readAtOffset_read_us += read_us;
                        g_readAtOffset_count++;
                        
#if DBG_GLYPH_TIMING
                        Serial.printf("[RO-DMA-POOL] offset=%u size=%u aligned=%u dma_size=%u seek=%u us read=%u us total=%u us\n",
                                     (unsigned)offset, (unsigned)read_len, (unsigned)aligned_offset,
                                     (unsigned)aligned_size, seek_us, read_us, total_us);
#endif
                        return read_len;
                    }
                    free_dma_buffer(dma_buf);  // 读取失败也要归还
                }
            }
            
            // 如果DMA池不可用或扇区数太多，尝试动态分配（保留原有逻辑作为fallback）
            if (sectors_needed > 8)
            {
                size_t aligned_size = sectors_needed * SECTOR;
#if defined(ESP_PLATFORM) || defined(ESP32)
                uint8_t *dma_buf = (uint8_t *)heap_caps_malloc(aligned_size, MALLOC_CAP_DMA);
                if (dma_buf)
                {
                    // 成功分配，执行对齐读取
                    f.seek(aligned_offset);
                    uint32_t t_seek_end = micros();
                    
                    size_t got = f.read(dma_buf, aligned_size);
                    uint32_t t_read_end = micros();
                    
                    if (got >= start_in_sector + read_len)
                    {
                        // 读取成功，复制用户需要的部分
                        memcpy(buffer, dma_buf + start_in_sector, read_len);
                        heap_caps_free(dma_buf);
                        
                        uint32_t seek_us = t_seek_end - t_total_start;
                        uint32_t read_us = t_read_end - t_seek_end;
                        uint32_t total_us = t_read_end - t_total_start;
                        
                        g_readAtOffset_total_us += total_us;
                        g_readAtOffset_seek_us += seek_us;
                        g_readAtOffset_read_us += read_us;
                        g_readAtOffset_count++;
                        
#if DBG_GLYPH_TIMING
                        Serial.printf("[RO-DMA-ALLOC] offset=%u size=%u aligned=%u dma_size=%u seek=%u us read=%u us total=%u us\n",
                                     (unsigned)offset, (unsigned)read_len, (unsigned)aligned_offset,
                                     (unsigned)aligned_size, seek_us, read_us, total_us);
#endif
                        return read_len;
                    }
                    heap_caps_free(dma_buf);
                }
#endif
            }
        }
#endif
        
        // Fallback: 普通读取（非DMA或分配失败）
        f.seek(offset);
        uint32_t t_seek_end = micros();
        
        size_t result = f.read(buffer, read_len);
        uint32_t t_read_end = micros();
        
        uint32_t seek_us = t_seek_end - t_total_start;
        uint32_t read_us = t_read_end - t_seek_end;
        uint32_t total_us = t_read_end - t_total_start;
        
        // 累积统计（注意：这些变量在CPP中定义）
        g_readAtOffset_total_us += total_us;
        g_readAtOffset_seek_us += seek_us;
        g_readAtOffset_read_us += read_us;
        g_readAtOffset_count++;
        
        // 每次都打印详细时间
#if DBG_GLYPH_TIMING
        Serial.printf("[RO-FALLBACK] offset=%u size=%u seek=%u us read=%u us total=%u us\n",
                     (unsigned)offset, (unsigned)read_len, seek_us, read_us, total_us);
#endif
        
        return result;
    }
    
    void SDWrapper::reset_readAtOffset_stats()
    {
        g_readAtOffset_total_us = 0;
        g_readAtOffset_count = 0;
        g_readAtOffset_seek_us = 0;
        g_readAtOffset_read_us = 0;
    }
    
    void SDWrapper::print_readAtOffset_stats()
    {
        if (g_readAtOffset_count > 0) {
            uint32_t avg_total = g_readAtOffset_total_us / g_readAtOffset_count;
            uint32_t avg_seek = g_readAtOffset_seek_us / g_readAtOffset_count;
            uint32_t avg_read = g_readAtOffset_read_us / g_readAtOffset_count;
            Serial.printf("[READATOFFSET_STATS] 总调用=%u 总时间=%u us [平均seek=%u us 平均read=%u us 平均总计=%u us]\n",
                         g_readAtOffset_count,
                         g_readAtOffset_total_us,
                         avg_seek, avg_read, avg_total);
        }
    }

    bool SDWrapper::reinitialize()
    {
        // 🔄 完全重新初始化SD卡驱动，以清除累积的状态
        Serial.println("[SDW] 🔄 重新初始化SD卡...");
        
#if HAS_SD_MMC
        if (iface_ == IF_SDMMC)
        {
            // 先结束当前会话
            ::SD_MMC.end();
            delay(100);  // 给SD卡一点恢复时间
            
            // 重新初始化
#ifdef SOC_SDMMC_USE_GPIO_MATRIX
            ::SD_MMC.setPins(SD_SPI_SCK_PIN, SD_SPI_MOSI_PIN, SD_SPI_MISO_PIN);
#endif
            bool ok = ::SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_HIGHSPEED, 20);
            if (ok) {
                Serial.println("[SDW] ✅ SD_MMC 重新初始化成功");
                return true;
            } else {
                Serial.println("[SDW] ❌ SD_MMC 重新初始化失败");
                return false;
            }
        }
#endif
        
        // SPI 模式暂不支持重新初始化（需要更多参数）
        Serial.println("[SDW] ⚠️  SPI 模式暂不支持重新初始化");
        return false;
    }

    unsigned long SDWrapper::benchmarkSmallRead(File &f, const char *path, size_t offset, size_t read_len)
    {
        // Perform a small non-DMA read at specified offset and log timing
        if (!f || read_len == 0)
        {
            return 0;
        }

        std::vector<uint8_t> read_buf(read_len);
        unsigned long t0 = micros();
        size_t got = readAtOffset(f, offset, read_buf.data(), read_len);
        unsigned long t1 = micros();
        unsigned long elapsed_us = t1 - t0;

        // Log the result
        Serial.printf("benchmark small-nonDMA read %s at %u, got %u bytes, seek+read %lu us\n",
                      path, (unsigned)offset, (unsigned)got, elapsed_us);

        return elapsed_us;
    }

}

