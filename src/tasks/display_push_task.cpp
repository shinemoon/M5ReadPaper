#include "readpaper.h"
#include "test/per_file_debug.h"
#include "display_push_task.h"
#include <M5Unified.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "task_priorities.h"
#include "current_book.h"

extern M5Canvas *g_canvas;
extern GlobalConfig g_config;
bool s_toggleFastMode = false;
// 队列和任务句柄
static QueueHandle_t s_displayQueue = NULL;
static TaskHandle_t s_displayTaskHandle = NULL;
// Canvas FIFO 用于存放 M5Canvas* 克隆（由渲染端创建，显示任务消费）
static QueueHandle_t s_canvasQueue = NULL;

// 全局canvas用于存储屏幕底部4像素高度的区域(540*4)
static M5Canvas *g_bottomStripCanvas = nullptr;

// pushSprite 计数器
static volatile uint32_t s_pushCount = 0;
static const uint32_t PUSH_COUNT_THRESHOLD = FIRST_REFRESH_TH;          // 6次epd_fastest
static const uint32_t PUSH_COUNT_THRESHOLD_QUALIYT = SECOND_REFRESH_TH; // 24次epd_quality

// 任务函数
static void displayTaskFunction(void *pvParameters)
{
    M5.Display.powerSaveOff();

    // 初始化底部区域canvas (540*4)
    if (g_bottomStripCanvas == nullptr)
    {
        g_bottomStripCanvas = new M5Canvas(&M5.Display);
        if (g_bottomStripCanvas)
        {
            g_bottomStripCanvas->createSprite(540, 4);
        }
    }

    uint8_t msg;
    for (;;)
    {
        if (xQueueReceive(s_displayQueue, &msg, portMAX_DELAY) == pdTRUE)
        {

            bool isIndexing = (g_current_book && g_current_book->isIndexingInProgress());

            // Wait the slot
            M5.Display.waitDisplay();
            if (msg == DISPLAY_PUSH_MSG_TYPE_FLUSH || msg == DISPLAY_PUSH_MSG_TYPE_FLUSH_TRANS || msg == DISPLAY_PUSH_MSG_TYPE_FLUSH_INVERT_TRANS || msg == DISPLAY_PUSH_MSG_TYPE_FLUSH_QUALITY)
            {
                // 从 canvas FIFO 中 pop 出一个克隆画布用于推送；若没有则立即回退到使用全局 g_canvas
                M5Canvas *canvas_to_push = nullptr;
                if (s_canvasQueue)
                {
                    // 正常流程为：先入队 canvas，再 enqueue 消息，因此这里不需要等待。
                    (void)xQueueReceive(s_canvasQueue, &canvas_to_push, 0);
                }

                if (canvas_to_push == nullptr && g_canvas == nullptr)
                {
                    // 没有任何 canvas 可用，跳过本次
                    continue;
                }

                M5Canvas *use_canvas = canvas_to_push ? canvas_to_push : g_canvas;

                if (use_canvas)
                {
                    // 累加计数器

                    // 根据计数器决定是否使用quality模式 & fast mode
                    bool useTextMode = (s_pushCount % PUSH_COUNT_THRESHOLD == 0) && (s_pushCount >= PUSH_COUNT_THRESHOLD);
                    bool useQualityMode = (s_pushCount >= PUSH_COUNT_THRESHOLD_QUALIYT || msg == DISPLAY_PUSH_MSG_TYPE_FLUSH_QUALITY);
                    s_pushCount++;

                    if (useQualityMode)
                    {
                        s_pushCount = 0;
                        // 保存当前模式并切换到quality
                        // 注意：M5Stack没有直接的getEpdMode()，我们假设默认是fastest
                        M5.Display.setEpdMode(QUALITY_REFRESH);
                        M5.Display.setColorDepth(16);
#if DBG_BIN_FONT_PRINT
                        Serial.printf("[DISPLAY_PUSH_TASK] pushSprite #%lu - 使用quality模式\n", (unsigned long)s_pushCount);
#endif
                    }
                    else if (useTextMode)
                    {
                        // 其实只是为了切一下模式消除中部疑似硬件问题带来的残影。
                        // Toggle between epd_fastest and epd_fast to try to mitigate mid-screen ghosting.
                        s_toggleFastMode = !s_toggleFastMode; // 暂时不用了
                        // if (s_toggleFastMode)
                        if (true)
                        {
                            M5.Display.setEpdMode(MIDDLE_REFRESH);
                            // M5.Display.setEpdMode(g_config.fastrefresh ? NORMAL_REFRESH : MIDDLE_REFRESH);
                            /*
                            //- Insert one round Push to reset the screen !
                            if (msg == DISPLAY_PUSH_MSG_TYPE_FLUSH)
                                use_canvas->pushSprite(0, 0);
                            else if (msg == DISPLAY_PUSH_MSG_TYPE_FLUSH_TRANS)
                                use_canvas->pushSprite(0, 0, TFT_WHITE);
                            else if (msg == DISPLAY_PUSH_MSG_TYPE_FLUSH_INVERT_TRANS)
                                use_canvas->pushSprite(0, 0, TFT_BLACK);
                            else
                                use_canvas->pushSprite(0, 0);
                            */
                            // 推送一个白色小方块到屏幕 (0,0) 到 (10,10)
                            //                            M5.Display.fillRect(0, 478, 540, 4, TFT_WHITE);
                            g_bottomStripCanvas->pushSprite(0, 478, TFT_BLACK);
                            M5.Display.waitDisplay();
                            // delay(100);
                            M5.Display.setEpdMode(g_config.fastrefresh ? LOW_REFRESH : NORMAL_REFRESH);
#if DBG_BIN_FONT_PRINT
                            Serial.printf("[DISPLAY_PUSH_TASK] pushSprite #%lu - 切换到 epd_fastest (toggle)\n", (unsigned long)s_pushCount);
#endif
                        }
                        else
                        {
                            // M5.Display.setEpdMode( MIDDLE_REFRESH);
                            M5.Display.setEpdMode(g_config.fastrefresh ? NORMAL_REFRESH : MIDDLE_REFRESH);
#if DBG_BIN_FONT_PRINT
                            Serial.printf("[DISPLAY_PUSH_TASK] pushSprite #%lu - 切换到 epd_fast (toggle)\n", (unsigned long)s_pushCount);
#endif
                        }
#if DBG_BIN_FONT_PRINT
                        Serial.printf("[DISPLAY_PUSH_TASK] pushSprite #%lu - 使用quality模式\n", (unsigned long)s_pushCount);
#endif
                    }
                    else
                    {

#if DBG_BIN_FONT_PRINT
                        Serial.printf("[DISPLAY_PUSH_TASK] pushSprite #%lu - 使用fastest模式\n", (unsigned long)s_pushCount);
#endif
                        // M5.Display.setEpdMode(NORMAL_REFRESH);
                        M5.Display.setEpdMode(g_config.fastrefresh ? LOW_REFRESH : NORMAL_REFRESH);
                    }

                    unsigned long t0 = millis();
                    (void)t0; // 抑制未使用变量警告
#if DBG_BIN_FONT_PRINT
                    Serial.printf("[DISPLAY_PUSH_TASK] pushSprite start ts=%lu\n", t0);
#endif
                    if (msg == DISPLAY_PUSH_MSG_TYPE_FLUSH)
                        use_canvas->pushSprite(0, 0);
                    else if (msg == DISPLAY_PUSH_MSG_TYPE_FLUSH_TRANS)
                        use_canvas->pushSprite(0, 0, TFT_WHITE);
                    else if (msg == DISPLAY_PUSH_MSG_TYPE_FLUSH_INVERT_TRANS)
                        use_canvas->pushSprite(0, 0, TFT_BLACK);
                    else
                        use_canvas->pushSprite(0, 0);

                    // 保存底部区域 (0,478)->(540,482) 到全局canvas
                    if (g_bottomStripCanvas && use_canvas)
                    {
                        // 从use_canvas的(0,478)位置读取540*4区域并保存到g_bottomStripCanvas
                        for (int y = 0; y < 4; y++)
                        {
                            for (int x = 0; x < 540; x++)
                            {
                                uint16_t color = use_canvas->readPixel(x, 478 + y);
                                g_bottomStripCanvas->drawPixel(x, y, color);
                            }
                        }
                    }

                    // 如果使用了quality模式，推送后恢复fastest模式
                    // if (useQualityMode || useTextMode)
                    if (useQualityMode)
                    {
                        // delay(300);
                        M5.Display.waitDisplay();
                        // M5.Display.setEpdMode(NORMAL_REFRESH);
                        M5.Display.setEpdMode(g_config.fastrefresh ? LOW_REFRESH : NORMAL_REFRESH);
                        M5.Display.setColorDepth(TEXT_COLORDEPTH);
#if DBG_BIN_FONT_PRINT
                        Serial.printf("[DISPLAY_PUSH_TASK] pushSprite完成，恢复fastest模式\n");
#endif
                    }
                    else
                    {
                        M5.Display.waitDisplay();
                        // delay(100);
                    }

                    unsigned long t1 = millis();
                    (void)t1; // 抑制未使用变量警告
#if DBG_BIN_FONT_PRINT
                    Serial.printf("[DISPLAY_PUSH_TASK] pushSprite end ts=%lu elapsed=%lu ms\n", t1, t1 - t0);
#endif

                    // 如果我们使用了克隆 canvas，释放它
                    if (canvas_to_push)
                    {
                        delete canvas_to_push;
                    }
                }
            }
            // 如果未来有更多消息类型，在这里扩展
        }
    }
    M5.Display.powerSaveOn();
}

bool initializeDisplayPushTask(size_t queue_len)
{
    if (s_displayQueue != NULL)
        return true;
    s_displayQueue = xQueueCreate(queue_len, sizeof(uint8_t));
    if (s_displayQueue == NULL)
        return false;

    // 初始化 canvas FIFO（固定为 2 槽）
    if (s_canvasQueue == NULL)
    {
        s_canvasQueue = xQueueCreate(2, sizeof(M5Canvas *));
        if (s_canvasQueue == NULL)
        {
#if DBG_BIN_FONT_PRINT
            Serial.println("[DISPLAY_PUSH_TASK] 警告：无法创建 canvas FIFO");
#endif
            // 不致命：继续仅使用全局 canvas 推送
        }
    }

    BaseType_t res = xTaskCreatePinnedToCore(
        displayTaskFunction,
        "DisplayPushTask",
        4096,
        NULL,
        PRIO_DISPLAY, // 高优先级，尽快完成 push
        &s_displayTaskHandle,
        0 // 放在核心0，让显示更新独占
    );

    if (res != pdPASS)
    {
        vQueueDelete(s_displayQueue);
        s_displayQueue = NULL;
        return false;
    }
    return true;
}

void destroyDisplayPushTask()
{
    if (s_displayTaskHandle != NULL)
    {
        vTaskDelete(s_displayTaskHandle);
        s_displayTaskHandle = NULL;
    }
    if (s_displayQueue != NULL)
    {
        vQueueDelete(s_displayQueue);
        s_displayQueue = NULL;
    }
    if (s_canvasQueue != NULL)
    {
        // 清理队列中可能残留的 canvas
        M5Canvas *c = nullptr;
        while (xQueueReceive(s_canvasQueue, &c, 0) == pdTRUE)
        {
            if (c)
                delete c;
        }
        vQueueDelete(s_canvasQueue);
        s_canvasQueue = NULL;
    }
    // 清理底部区域canvas
    if (g_bottomStripCanvas != nullptr)
    {
        delete g_bottomStripCanvas;
        g_bottomStripCanvas = nullptr;
    }
}

bool enqueueDisplayPush(uint8_t msgType)
{
    if (s_displayQueue == NULL)
        return false;
    BaseType_t res = xQueueSendToBack(s_displayQueue, &msgType, 0);
    return res == pdPASS;
}

bool enqueueCanvasCloneBlocking(M5Canvas *canvas_clone)
{
    if (s_canvasQueue == NULL || canvas_clone == nullptr)
        return false;
    BaseType_t res = xQueueSendToBack(s_canvasQueue, &canvas_clone, portMAX_DELAY);
    return res == pdPASS;
}

void resetDisplayPushCount()
{
    s_pushCount = 0;
#if DBG_BIN_FONT_PRINT
    Serial.printf("[DISPLAY_PUSH_TASK] pushSprite计数器已重置\n");
#endif
}

uint32_t getDisplayPushCount()
{
    return s_pushCount;
}
