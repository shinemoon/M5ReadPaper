#include "usb_msc.h"
#include "SD/SDWrapper.h"
#include "papers3.h"
#include <Arduino.h>

// Arduino ESP32 USB MSC (requires Arduino ESP32 core 3.x+)
#include "USB.h"
#include "USBMSC.h"

// ESP-IDF SDMMC low-level access for multi-sector operations
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_err.h"
#include "tasks/background_index_task.h"
#include "globals.h"

static USBMSC msc;
static bool g_active = false;
static sdmmc_card_t* g_card = nullptr;
static bool g_unmounted_sdmmc_for_msc = false;
// Count of pending write operations (SCSI WRITE commands being processed)
static volatile int g_pending_writes = 0;

// Callbacks for TinyUSB MSC
static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    if (!g_card) return -1;
    // Track pending writes so we can delay shutdown until writes finish
    ++g_pending_writes;
    int32_t result = -1;

    // Write complete sectors directly to SD card via sdmmc
    if (sdmmc_write_sectors(g_card, buffer, lba, bufsize / 512) == ESP_OK)
    {
        result = (int32_t)bufsize;
    }

    --g_pending_writes;
    return result;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    if (!g_card) return -1;
    
    // Read complete sectors directly from SD card via sdmmc
    if (sdmmc_read_sectors(g_card, (uint8_t*)buffer, lba, bufsize / 512) == ESP_OK)
    {
        return bufsize;
    }
    return -1;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject)
{
    Serial.printf("[USB_MSC] StartStop: power=%d start=%d eject=%d\n", 
                  power_condition, start, load_eject);
    
    if (load_eject)
    {
        // Host is ejecting/unmounting the drive - report media removed to host,
        // then start a background task to wait for outstanding writes (or timeout)
        // before stopping MSC and rebooting. Do NOT block the USB callback here.
        Serial.println("[USB_MSC] Host ejected drive - reporting media removed, waiting for writes to finish...");
        msc.mediaPresent(false);

        // Background waiter task
        auto eject_waiter = [](void* /*param*/) {
            const int timeout_ms = 5000; // wait up to 5s for pending writes
            int waited = 0;
            while (g_pending_writes > 0 && waited < timeout_ms)
            {
                vTaskDelay(pdMS_TO_TICKS(100));
                waited += 100;
            }
            Serial.printf("[USB_MSC] Eject waiter: pending_writes=%d, waited=%d ms\n", g_pending_writes, waited);
            // Now perform a full stop (will reboot)
            usb_msc_stop();
            // Should not reach here because usb_msc_stop() restarts the MCU
            vTaskDelete(NULL);
        };

        // Create a detached FreeRTOS task to handle the delayed stop
        xTaskCreate(
            /*pvTaskCode*/ (TaskFunction_t)eject_waiter,
            /*pcName*/ "msc_eject",
            /*usStackDepth*/ 4096,
            /*pvParameters*/ NULL,
            /*uxPriority*/ tskIDLE_PRIORITY + 1,
            /*pxCreatedTask*/ NULL
        );
    }
    return true;
}

bool usb_msc_init()
{
    // Nothing to do here, initialization happens in usb_msc_start
    return true;
}

bool usb_msc_start()
{
    if (g_active)
        return true;

    Serial.println("[USB_MSC] Starting USB Mass Storage...");

    // Ensure no SD access happens while we prepare MSC: set global disable flag
    // and stop any in-progress indexing gracefully. IMPORTANT: do NOT trigger
    // a force-reindex here (which performs on-disk operations and may call
    // filesystem APIs in contexts unsafe for USB callbacks). Instead request
    // the current BookHandle to stop and wait briefly for it to finish.
    g_disable_sd_access = true;
    if (g_current_book)
    {
        Serial.printf("[USB_MSC] Requesting stopIndexingAndWait for %s\n", g_current_book->filePath().c_str());
        // Best-effort: ask the book to stop indexing and wait up to 5s
        (void)g_current_book->stopIndexingAndWait(5000);
    }
    // Wait up to 5s (poll) for indexing flag to clear in case it takes a bit
    // longer for background pieces to fully stop. Do not call functions that
    // trigger file I/O here.
    {
        const unsigned long WAIT_MS = 5000;
        unsigned long deadline = millis() + WAIT_MS;
        while (millis() < deadline)
        {
            if (!g_current_book || !g_current_book->isIndexingInProgress())
                break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    // Ensure SD is initialized in SDMMC mode
    if (SDW::SD.currentInterface() != SDW::IF_SDMMC)
    {
        Serial.println("[USB_MSC] ERROR: SD not in SDMMC mode!");
        Serial.println("[USB_MSC] Please ensure SD is initialized with SDMMC interface");
        return false;
    }

    // Initialize our own sdmmc_card_t for direct sector access
    // We'll use 1-bit mode to match the existing SD_MMC initialization
    g_card = (sdmmc_card_t*)malloc(sizeof(sdmmc_card_t));
    if (!g_card)
    {
        Serial.println("[USB_MSC] Failed to allocate card structure");
        return false;
    }

    // Configure SDMMC host for 1-bit mode
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;  // 1-bit mode
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    // Configure slot with custom pins for M5Stack Paper S3
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;  // 1-bit mode
    slot_config.clk = (gpio_num_t)SD_SPI_SCK_PIN;   // GPIO 39
    slot_config.cmd = (gpio_num_t)SD_SPI_MOSI_PIN;  // GPIO 38
    slot_config.d0 = (gpio_num_t)SD_SPI_MISO_PIN;   // GPIO 40

    // If SD was previously initialized/mounted via Arduino's SD_MMC,
    // unmount it so we can take direct control of the SDMMC host/slot.
    // This avoids driver/slot contention and can significantly reduce
    // initialization/enumeration time when doing raw sector access.
    if (SDW::SD.currentInterface() == SDW::IF_SDMMC)
    {
        Serial.println("[USB_MSC] Detected SD already in SDMMC mode - will attempt to unmount SD_MMC to take raw control");

        // Try to safely close any open book file handles to avoid unmount races
        // that can trigger newlib/vfs assertions if another task holds libc locks.
        bool safe_to_unmount = true;
        if (g_current_book)
        {
            // Request the book to mark for close (save auto-tag) and try to
            // acquire its file lock so we can close the underlying File safely.
            g_current_book->markForClose();
                const unsigned long lock_wait_ms = 2000;
                const unsigned long poll_interval_ms = 100;
                unsigned long waited_ms = 0;
                bool got_lock = false;
                while (waited_ms < lock_wait_ms)
                {
                    if (g_current_book->tryAcquireFileLock(pdMS_TO_TICKS(0)))
                    {
                        got_lock = true;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
                    waited_ms += poll_interval_ms;
                }
                if (!got_lock)
                {
                    Serial.println("[USB_MSC] Failed to acquire book file lock within timeout; aborting MSC start to avoid unsafe unmount");
                    safe_to_unmount = false;
                }
                else
                {
                    // Close the book's resident file handle while holding the lock.
                    g_current_book->close();
                    // Release the lock via public API
                    g_current_book->releaseFileLockPublic();
                }
        }

        if (!safe_to_unmount)
        {
            // Re-enable SD access since we couldn't safely prepare for MSC
            g_disable_sd_access = false;
            return false;
        }

        ::SD_MMC.end();
        g_unmounted_sdmmc_for_msc = true;
    }

    // Initialize SDMMC peripheral
    // Enable internal pull-ups on SD pins to improve card detection reliability
    pinMode(SD_SPI_SCK_PIN, INPUT_PULLUP);
    pinMode(SD_SPI_MOSI_PIN, INPUT_PULLUP);
    pinMode(SD_SPI_MISO_PIN, INPUT_PULLUP);

    esp_err_t ret = sdmmc_host_init();
    if (ret != ESP_OK)
    {
        Serial.printf("[USB_MSC] Failed to init SDMMC host: %d (%s)\n", ret, esp_err_to_name(ret));
        /*
         * ESP_ERR_INVALID_STATE (0x103 == 259) commonly means the host
         * was already initialized elsewhere (for example via Arduino's
         * SD_MMC.begin()). Treat that as non-fatal and continue - we
         * still need to try initializing the slot/card below.
         */
        if (ret != ESP_ERR_INVALID_STATE)
        {
            free(g_card);
            g_card = nullptr;
            return false;
        }
        else
        {
            Serial.println("[USB_MSC] SDMMC host already initialized, continuing");
        }
    }

    ret = sdmmc_host_init_slot(host.slot, &slot_config);
    if (ret != ESP_OK)
    {
        Serial.printf("[USB_MSC] Failed to init SDMMC slot: %d (%s)\n", ret, esp_err_to_name(ret));
        /* If the slot is already initialized, we can continue and try
         * to probe the card. Only treat other errors as fatal here. */
        if (ret != ESP_ERR_INVALID_STATE)
        {
            sdmmc_host_deinit();
            free(g_card);
            g_card = nullptr;
            return false;
        }
        else
        {
            Serial.println("[USB_MSC] SDMMC slot already initialized, continuing");
        }
    }

    // Probe and initialize the card
    ret = sdmmc_card_init(&host, g_card);
    if (ret != ESP_OK)
    {
        Serial.printf("[USB_MSC] Failed to init SD card: %d (%s)\n", ret, esp_err_to_name(ret));
        sdmmc_host_deinit();
        free(g_card);
        g_card = nullptr;
        return false;
    }

    // Print card info
    Serial.printf("[USB_MSC] SD Card: %s, Size: %llu MB, Sectors: %u\n",
                  g_card->cid.name,
                  ((uint64_t)g_card->csd.capacity) * g_card->csd.sector_size / (1024 * 1024),
                  g_card->csd.capacity);

    // Initialize USB MSC
    // ⚠️ 关键顺序：必须在 USB.begin() 之前完成所有 MSC 类配置。
    // USB.begin() 会立即拉高 D+，触发主机枚举；若此时描述符尚未包含 MSC 接口，
    // Windows 枚举失败后按指数退避重试，导致出现 U 盘需要长达 2 分钟的问题。
    msc.vendorID("M5Stack");
    msc.productID("Paper S3");
    msc.productRevision("1.0");
    msc.onRead(onRead);
    msc.onWrite(onWrite);
    msc.onStartStop(onStartStop);
    msc.mediaPresent(true);
    msc.begin(g_card->csd.capacity, g_card->csd.sector_size);

    // 所有类已注册完毕，D+ 上拉，主机此时可以读到完整的复合设备描述符
    USB.begin();

    g_active = true;
    
    return true;
}

void usb_msc_stop()
{
    // Reboot after stopping
    delay(200);
    ESP.restart();
}

void usb_msc_poll()
{
    // USB task is handled by Arduino framework automatically
    // No manual polling needed
}

bool usb_msc_is_active()
{
    return g_active;
}
