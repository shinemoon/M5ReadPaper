#include <M5Unified.h>
#include <SPIFFS.h>
#include <SD.h>
#include "readpaper.h"
#include "device/file_manager.h"
#include "device/ui_display.h"
#include "device/powermgt.h"
#include "device/wifi_hotspot_manager.h"
#include "init/setup.h"
#include "test/per_file_debug.h"
#include "tasks/state_machine_task.h"
#include "tasks/timer_interrupt_task.h"
#include "tasks/device_interrupt_task.h"
#include "text/book_handle.h"
#include "tasks/display_push_task.h"
#include "config/config_manager.h"
#include "globals.h"
#include "tasks/background_index_task.h"
#include "tasks/task_priorities.h"

// globals
int8_t STM = IDLE;

// 全局配置实例
GlobalConfig g_config;

// For jump target
int16_t target_page = 1;

int16_t area_width = PAPER_S3_WIDTH - MARGIN_LEFT - MARGIN_RIGHT;
int16_t area_height = PAPER_S3_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM;
float font_size = SYSFONTSIZE;

// timing globals
unsigned long idleTime = 0; // 最后一次非idle时间

// Wakeup cause from last boot/sleep (initialized in setup())
esp_sleep_wakeup_cause_t g_wake_cause = ESP_SLEEP_WAKEUP_UNDEFINED;

// 全局Canvas
M5Canvas *g_canvas = nullptr;

// 全局 BookHandle (migrated to atomic shared_ptr for safe cross-thread use)
#include "current_book.h"

// Definition of the shared pointer holding the current book. Other TUs see
// this via extern in current_book.h as __g_current_book_shared.
std::shared_ptr<BookHandle> __g_current_book_shared(nullptr);

// Global flag to request all SD access be disabled (used before switching to USB MSC)
// Defined in src/globals.cpp
// extern volatile bool g_disable_sd_access;

// ESP-IDF 兼容入口，创建一个独立任务运行 setup/loop
#ifdef __cplusplus
extern "C"
{
#endif
    static void MainTask(void *pvParameters)
    {
        setup();

#if DBG_MAIN
        Serial.println("[MAIN] 启动任务系统");
#endif

        // 初始化状态机任务
        if (!initializeStateMachine())
        {
#if DBG_MAIN
            Serial.println("[MAIN] 状态机任务初始化失败");
#endif
            return;
        }

        // 初始化定时器中断任务
        if (!initializeTimerInterrupt())
        {
#if DBG_MAIN
            Serial.println("[MAIN] 定时器中断任务初始化失败");
#endif
            destroyStateMachine();
            return;
        }

        // 初始化设备中断任务
        if (!initializeDeviceInterrupt())
        {
#if DBG_MAIN
            Serial.println("[MAIN] 设备中断任务初始化失败");
#endif
            destroyTimerInterrupt();
            destroyStateMachine();
            return;
        }

        // 初始化显示推送任务（用于异步 pushSprite）
        if (!initializeDisplayPushTask())
        {
#if DBG_MAIN
            Serial.println("[MAIN] 初始化显示推送任务失败");
#endif
            // 非致命：继续运行，但 bin_font_flush_canvas 的入队会失败
        }

        // 取消后台索引任务初始化：索引在主循环中显式执行（同步工作周期）

#if DBG_MAIN
        Serial.println("[MAIN] 所有任务初始化成功");
        Serial.printf("[MAIN] 可用堆内存: %u bytes\n", esp_get_free_heap_size());
#endif

        // Entry for interrupt monitoring - 主任务进入监控循环
        for (;;)
        {
            // 仅在WIRE CONNECT状态时处理Web服务器请求
            if (getCurrentSystemState() == STATE_WIRE_CONNECT && g_wifi_hotspot && g_wifi_hotspot->isRunning())
            {
                g_wifi_hotspot->handleClient();
            }
            else if (getCurrentSystemState() == STATE_USB_CONNECT)
            {
                do
                {
                } while (0); // Avoid Index in USBMSC
            }
            // 主任务可以在这里执行一些全局监控工作
            // 或者简单地睡眠，让其他任务运行

            // 后台索引建立支持
            // 后台索引任务：由 BackgroundIndexTask 处理。MainTask 只负责触发通知，BackgroundIndexTask 执行具体的索引片段。
            else
            {
                // Snapshot book once to avoid races with g_current_book macro
                auto main_bh_sp = current_book_shared();
                BookHandle *main_bh = main_bh_sp ? main_bh_sp.get() : nullptr;
                static uint32_t last_notify_ms = 0;
                // 动态冷却时间：强制重建挂起时更积极推进
                const uint32_t NOTIFY_COOLDOWN_MS = isForceReindexPending() ? 200 : 500;
                // 若需要重建或可继续索引，且堆内存充足，则在此同步执行一个索引工作周期
                if ((isForceReindexPending() || (main_bh && main_bh->canContinueIndexing())) &&
                    esp_get_free_heap_size() > (320 * 1024))
                {
                    uint32_t now = millis();
                    if (now - last_notify_ms > NOTIFY_COOLDOWN_MS)
                    {
#if DBG_MAIN
                        Serial.printf("[MAIN] 触发同步索引工作周期 (freeHeap=%u)\n", esp_get_free_heap_size());
#endif
                        // 同步执行一次工作周期（处理 pending force-reindex 或增量索引片段）
                        (void)runBackgroundIndexWorkCycle();
                        last_notify_ms = now;
                    }
                }
            }

            vTaskDelay(pdMS_TO_TICKS(50)); // 50ms检查一次
        }

        // 清理资源 (通常不会到达这里)
        wifi_hotspot_cleanup();
        destroyDeviceInterrupt();
        destroyDisplayPushTask();
        // 不再销毁后台索引任务（未创建）
        destroyTimerInterrupt();
        destroyStateMachine();
        vTaskDelete(NULL);
    }

    void app_main(void)
    {
        BaseType_t rc = xTaskCreatePinnedToCore(MainTask, "MainTask", 32768, NULL, PRIO_MAIN, NULL, 1);
        (void)rc; // 避免未使用警告
    }
#ifdef __cplusplus
}
#endif
