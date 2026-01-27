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
// 全局flag：标记当前是否正在进行显示推送
volatile bool inDisplayPush = false;
// 队列和任务句柄
static QueueHandle_t s_displayQueue = NULL;
static TaskHandle_t s_displayTaskHandle = NULL;
// Canvas FIFO 用于存放 M5Canvas* 克隆（由渲染端创建，显示任务消费）
static QueueHandle_t s_canvasQueue = NULL;

// pushSprite 计数器
static volatile uint32_t s_pushCount = 0;
static const uint32_t PUSH_COUNT_THRESHOLD = FIRST_REFRESH_TH;          // WorkAround My own device's HW issue...
static const uint32_t PUSH_COUNT_THRESHOLD_QUALIYT = SECOND_REFRESH_TH; // FullFresh for fast Mode

// 任务函数
static void displayTaskFunction(void *pvParameters)
{
    M5.Display.powerSaveOff();

    DisplayPushMessage msg;
    for (;;)
    {
        if (xQueueReceive(s_displayQueue, &msg, portMAX_DELAY) == pdTRUE)
        {
            // 标记正在进行显示推送
            inDisplayPush = true;

            bool isIndexing = (g_current_book && g_current_book->isIndexingInProgress());

            // Wait the slot
            M5.Display.waitDisplay();
            // 所有入队消息都视为刷新请求，使用 flags 决定具体行为
            if (true)
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
                    bool needMiddleStep = g_config.fastrefresh && (s_pushCount % PUSH_COUNT_THRESHOLD == 0) && (s_pushCount >= PUSH_COUNT_THRESHOLD);
                    // bool needMiddleStep = (s_pushCount % PUSH_COUNT_THRESHOLD == 0) && (s_pushCount >= PUSH_COUNT_THRESHOLD);
                    bool useQualityMode = (s_pushCount >= PUSH_COUNT_THRESHOLD_QUALIYT && g_config.fastrefresh) || msg.flags[2] || (s_pushCount >= FULL_REFRESH_TH && !g_config.fastrefresh);
                    s_pushCount++;

                    if (useQualityMode)
                    {
                        s_pushCount = 0;
                        M5.Display.setEpdMode(QUALITY_REFRESH);
                        M5.Display.setColorDepth(16);
#if DBG_BIN_FONT_PRINT
                        Serial.printf("[DISPLAY_PUSH_TASK] pushSprite #%lu - 使用quality模式\n", (unsigned long)s_pushCount);
#endif
                    }
                    // else if (needMiddleStep && !g_config.dark && g_config.fastrefresh)
                    else if (needMiddleStep && !g_config.dark)
                    {
                        // 其实只是为了切一下模式消除中部疑似硬件问题带来的残影。
                        // Toggle between epd_fastest and epd_fast to try to mitigate mid-screen ghosting.

                        M5.Display.setEpdMode(MIDDLE_REFRESH);
                        M5.Display.fillRect(0, 476, 540, 8, TFT_WHITE);
                        M5.Display.waitDisplay();
                        M5.Display.setEpdMode(g_config.fastrefresh ? LOW_REFRESH : NORMAL_REFRESH);
#if DBG_BIN_FONT_PRINT
                        Serial.printf("[DISPLAY_PUSH_TASK] pushSprite #%lu - 切换到 epd_fastest (toggle)\n", (unsigned long)s_pushCount);
#endif
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

#if DBG_BIN_FONT_PRINT
                    unsigned long t0 = millis();
                    (void)t0; // 抑制未使用变量警告
#else
                    (void)0;
#endif
#if DBG_BIN_FONT_PRINT
                    Serial.printf("[DISPLAY_PUSH_TASK] pushSprite start ts=%lu\n", t0);
#endif
                    // 使用封装的 push 操作，传入 trans/invert/effect 和矩形区域参数
                    auto perform_push = [](M5Canvas *canvas, bool trans, bool invert, int8_t effect, int rect_x, int rect_y, int rect_w, int rect_h)
                    {
                        if (effect == display_type::VSHUTTER)
                        {
                            // 分成从上下两端向中间交错推送
                            const int slices = 32;
                            const int total_h = rect_h;
                            const int w = rect_w;
                            int slice_h = total_h / slices;

                            // 获取原始缓冲信息
                            void *src_buf = canvas->getBuffer();
                            size_t buf_len = canvas->bufferLength();
                            if (!src_buf || buf_len == 0 || slice_h <= 0)
                            {
                                return;
                            }

                            size_t row_bytes = buf_len / (size_t)PAPER_S3_HEIGHT;
                            size_t bytes_per_pixel = row_bytes / (size_t)PAPER_S3_WIDTH;

                            // 从上下两端向中间交错推送
                            for (int i = 0; i < slices; i++)
                            {
                                int s;
                                if (i % 2 == 0)
                                {
                                    // 从顶部: 0, 1, 2...
                                    s = i / 2;
                                }
                                else
                                {
                                    // 从底部: 31, 30, 29...
                                    s = slices - 1 - (i / 2);
                                }
                                
                                if (s < 0 || s >= slices)
                                    continue;

                                int start_row = s * slice_h;
                                int h = (s == slices - 1) ? (total_h - start_row) : slice_h;
                                if (h <= 0)
                                    continue;

                                M5Canvas *slice = new M5Canvas(&M5.Display);
                                if (!slice)
                                    break;
                                slice->setPsram(true);
                                slice->setColorDepth(canvas->getColorDepth());
                                slice->createSprite(w, h);

                                void *dst_buf = slice->getBuffer();
                                if (dst_buf)
                                {
                                    // 从源canvas的指定矩形区域中复制
                                    uint8_t *src_base = (uint8_t *)src_buf;
                                    uint8_t *dst_base = (uint8_t *)dst_buf;
                                    size_t slice_row_bytes = (size_t)w * bytes_per_pixel;
                                    size_t src_col_offset = (size_t)rect_x * bytes_per_pixel;
                                    
                                    for (int row = 0; row < h; row++)
                                    {
                                        uint8_t *src_row = src_base + (rect_y + start_row + row) * row_bytes + src_col_offset;
                                        uint8_t *dst_row = dst_base + row * slice_row_bytes;
                                        memcpy(dst_row, src_row, slice_row_bytes);
                                    }
                                }

                                if (trans)
                                {
                                    slice->pushSprite(rect_x, rect_y + start_row, invert ? TFT_BLACK : TFT_WHITE);
                                }
                                else
                                {
                                    slice->pushSprite(rect_x, rect_y + start_row);
                                }
//                                M5.Display.waitDisplay();
                                delete slice;
                            }
                        }
                        else if (effect == display_type::VSHUTTER_NORMAL)
                        {
                            // 从顶部向底部顺序推送
                            const int slices = 32;
                            const int total_h = rect_h;
                            const int w = rect_w;
                            int slice_h = total_h / slices;

                            // 获取原始缓冲信息
                            void *src_buf = canvas->getBuffer();
                            size_t buf_len = canvas->bufferLength();
                            if (!src_buf || buf_len == 0 || slice_h <= 0)
                            {
                                return;
                            }

                            size_t row_bytes = buf_len / (size_t)PAPER_S3_HEIGHT;
                            size_t bytes_per_pixel = row_bytes / (size_t)PAPER_S3_WIDTH;

                            // 从顶部到底部顺序推送
                            for (int s = 0; s < slices; s++)
                            {
                                int start_row = s * slice_h;
                                int h = (s == slices - 1) ? (total_h - start_row) : slice_h;
                                if (h <= 0)
                                    continue;

                                M5Canvas *slice = new M5Canvas(&M5.Display);
                                if (!slice)
                                    break;
                                slice->setPsram(true);
                                slice->setColorDepth(canvas->getColorDepth());
                                slice->createSprite(w, h);

                                void *dst_buf = slice->getBuffer();
                                if (dst_buf)
                                {
                                    // 从源canvas的指定矩形区域中复制
                                    uint8_t *src_base = (uint8_t *)src_buf;
                                    uint8_t *dst_base = (uint8_t *)dst_buf;
                                    size_t slice_row_bytes = (size_t)w * bytes_per_pixel;
                                    size_t src_col_offset = (size_t)rect_x * bytes_per_pixel;
                                    
                                    for (int row = 0; row < h; row++)
                                    {
                                        uint8_t *src_row = src_base + (rect_y + start_row + row) * row_bytes + src_col_offset;
                                        uint8_t *dst_row = dst_base + row * slice_row_bytes;
                                        memcpy(dst_row, src_row, slice_row_bytes);
                                    }
                                }

                                if (trans)
                                {
                                    slice->pushSprite(rect_x, rect_y + start_row, invert ? TFT_BLACK : TFT_WHITE);
                                }
                                else
                                {
                                    slice->pushSprite(rect_x, rect_y + start_row);
                                }
                                delete slice;
                            }
                        }
                        else if (effect == display_type::HSHUTTER)
                        {
                            // 分成若干片从左右两端向中间交错推送
                            const int slices = 17;
                            const int total_w = rect_w;
                            const int h = rect_h;
                            int slice_w = total_w / slices;

                            // 获取原始缓冲信息
                            void *src_buf = canvas->getBuffer();
                            size_t buf_len = canvas->bufferLength();
                            if (!src_buf || buf_len == 0 || slice_w <= 0)
                            {
                                return;
                            }

                            // 假设每行的字节数（根据颜色深度计算）
                            size_t row_bytes = buf_len / (size_t)PAPER_S3_HEIGHT;
                            size_t bytes_per_pixel = row_bytes / (size_t)PAPER_S3_WIDTH;

                            // 从左右两端向中间交错推送
                            for (int i = 0; i < slices; i++)
                            {
                                int s;
                                if (i % 2 == 0)
                                {
                                    // 从左侧: 0, 1, 2...
                                    s = i / 2;
                                }
                                else
                                {
                                    // 从右侧: 16, 15, 14...
                                    s = slices - 1 - (i / 2);
                                }
                                
                                if (s < 0 || s >= slices)
                                    continue;

                                int start_col = s * slice_w;
                                int w = (s == slices - 1) ? (total_w - start_col) : slice_w;
                                if (w <= 0)
                                    continue;

                                M5Canvas *slice = new M5Canvas(&M5.Display);
                                if (!slice)
                                    break;
                                slice->setPsram(true);
                                slice->setColorDepth(canvas->getColorDepth());
                                slice->createSprite(w, h);

                                void *dst_buf = slice->getBuffer();
                                if (dst_buf)
                                {
                                    // 逐行复制对应的列区域（从源canvas的指定矩形区域中）
                                    uint8_t *src_base = (uint8_t *)src_buf;
                                    uint8_t *dst_base = (uint8_t *)dst_buf;
                                    size_t slice_row_bytes = (size_t)w * bytes_per_pixel;
                                    size_t src_col_offset = (size_t)(rect_x + start_col) * bytes_per_pixel;

                                    for (int row = 0; row < h; row++)
                                    {
                                        uint8_t *src_row = src_base + (rect_y + row) * row_bytes + src_col_offset;
                                        uint8_t *dst_row = dst_base + row * slice_row_bytes;
                                        memcpy(dst_row, src_row, slice_row_bytes);
                                    }
                                }

                                if (trans)
                                {
                                    slice->pushSprite(rect_x + start_col, rect_y, invert ? TFT_BLACK : TFT_WHITE);
                                }
                                else
                                {
                                    slice->pushSprite(rect_x + start_col, rect_y);
                                }
//                                M5.Display.waitDisplay();
                                delete slice;
                            }
                        }
                        else if (effect == display_type::HSHUTTER_NORMAL)
                        {
                            // 从左到右顺序推送
                            const int slices = 17;
                            const int total_w = rect_w;
                            const int h = rect_h;
                            int slice_w = total_w / slices;

                            // 获取原始缓冲信息
                            void *src_buf = canvas->getBuffer();
                            size_t buf_len = canvas->bufferLength();
                            if (!src_buf || buf_len == 0 || slice_w <= 0)
                            {
                                return;
                            }

                            size_t row_bytes = buf_len / (size_t)PAPER_S3_HEIGHT;
                            size_t bytes_per_pixel = row_bytes / (size_t)PAPER_S3_WIDTH;

                            // 从左到右顺序推送
                            for (int s = 0; s < slices; s++)
                            {
                                int start_col = s * slice_w;
                                int w = (s == slices - 1) ? (total_w - start_col) : slice_w;
                                if (w <= 0)
                                    continue;

                                M5Canvas *slice = new M5Canvas(&M5.Display);
                                if (!slice)
                                    break;
                                slice->setPsram(true);
                                slice->setColorDepth(canvas->getColorDepth());
                                slice->createSprite(w, h);

                                void *dst_buf = slice->getBuffer();
                                if (dst_buf)
                                {
                                    // 逐行复制对应的列区域（从源canvas的指定矩形区域中）
                                    uint8_t *src_base = (uint8_t *)src_buf;
                                    uint8_t *dst_base = (uint8_t *)dst_buf;
                                    size_t slice_row_bytes = (size_t)w * bytes_per_pixel;
                                    size_t src_col_offset = (size_t)(rect_x + start_col) * bytes_per_pixel;

                                    for (int row = 0; row < h; row++)
                                    {
                                        uint8_t *src_row = src_base + (rect_y + row) * row_bytes + src_col_offset;
                                        uint8_t *dst_row = dst_base + row * slice_row_bytes;
                                        memcpy(dst_row, src_row, slice_row_bytes);
                                    }
                                }

                                if (trans)
                                {
                                    slice->pushSprite(rect_x + start_col, rect_y, invert ? TFT_BLACK : TFT_WHITE);
                                }
                                else
                                {
                                    slice->pushSprite(rect_x + start_col, rect_y);
                                }
                                delete slice;
                            }
                        }
                        else if (effect == display_type::RECT)
                        {
                            // 将指定区域划分成4x6的24个方块区域，乱序推送
                            const int cols = 4;
                            const int rows = 6;
                            const int total_blocks = cols * rows;
                            const int block_w = rect_w / cols;
                            const int block_h = rect_h / rows;

                            // 获取原始缓冲信息
                            void *src_buf = canvas->getBuffer();
                            size_t buf_len = canvas->bufferLength();
                            if (!src_buf || buf_len == 0)
                            {
                                return;
                            }

                            size_t row_bytes = buf_len / (size_t)PAPER_S3_HEIGHT;
                            size_t bytes_per_pixel = row_bytes / (size_t)PAPER_S3_WIDTH;

                            // 生成0-23的索引数组并打乱
                            int indices[24];
                            for (int i = 0; i < total_blocks; i++)
                            {
                                indices[i] = i;
                            }
                            // 简单的洗牌算法（Fisher-Yates）
                            for (int i = total_blocks - 1; i > 0; i--)
                            {
                                int j = rand() % (i + 1);
                                int temp = indices[i];
                                indices[i] = indices[j];
                                indices[j] = temp;
                            }

                            // 按照打乱的顺序推送每个方块
                            for (int idx = 0; idx < total_blocks; idx++)
                            {
                                int block_idx = indices[idx];
                                int block_col = block_idx % cols;
                                int block_row = block_idx / cols;
                                int start_x = block_col * block_w;
                                int start_y = block_row * block_h;

                                M5Canvas *block = new M5Canvas(&M5.Display);
                                if (!block)
                                    break;
                                block->setPsram(true);
                                block->setColorDepth(canvas->getColorDepth());
                                block->createSprite(block_w, block_h);

                                void *dst_buf = block->getBuffer();
                                if (dst_buf)
                                {
                                    // 逐行复制方块区域（从源canvas的指定矩形区域中）
                                    uint8_t *src_base = (uint8_t *)src_buf;
                                    uint8_t *dst_base = (uint8_t *)dst_buf;
                                    size_t block_row_bytes = (size_t)block_w * bytes_per_pixel;
                                    size_t src_col_offset = (size_t)(rect_x + start_x) * bytes_per_pixel;

                                    for (int row = 0; row < block_h; row++)
                                    {
                                        uint8_t *src_row = src_base + (rect_y + start_y + row) * row_bytes + src_col_offset;
                                        uint8_t *dst_row = dst_base + row * block_row_bytes;
                                        memcpy(dst_row, src_row, block_row_bytes);
                                    }
                                }

                                if (trans)
                                {
                                    block->pushSprite(rect_x + start_x, rect_y + start_y, invert ? TFT_BLACK : TFT_WHITE);
                                }
                                else
                                {
                                    block->pushSprite(rect_x + start_x, rect_y + start_y);
                                }
//                                M5.Display.waitDisplay();
                                delete block;
                            }
                        }
                        else
                        {
                            // NOEFFECT: 直接推送整个canvas或指定矩形区域
                            if (rect_w > 0 && rect_h > 0 && (rect_x != 0 || rect_y != 0 || rect_w != PAPER_S3_WIDTH || rect_h != PAPER_S3_HEIGHT))
                            {
                                // 推送指定矩形区域：创建临时canvas
                                void *src_buf = canvas->getBuffer();
                                size_t buf_len = canvas->bufferLength();
                                if (!src_buf || buf_len == 0)
                                {
                                    return;
                                }

                                size_t row_bytes = buf_len / (size_t)PAPER_S3_HEIGHT;
                                size_t bytes_per_pixel = row_bytes / (size_t)PAPER_S3_WIDTH;

                                M5Canvas *temp = new M5Canvas(&M5.Display);
                                if (temp)
                                {
                                    temp->setPsram(true);
                                    temp->setColorDepth(canvas->getColorDepth());
                                    temp->createSprite(rect_w, rect_h);

                                    void *dst_buf = temp->getBuffer();
                                    if (dst_buf)
                                    {
                                        // 从源canvas复制矩形区域
                                        uint8_t *src_base = (uint8_t *)src_buf;
                                        uint8_t *dst_base = (uint8_t *)dst_buf;
                                        size_t rect_row_bytes = (size_t)rect_w * bytes_per_pixel;
                                        size_t src_col_offset = (size_t)rect_x * bytes_per_pixel;

                                        for (int row = 0; row < rect_h; row++)
                                        {
                                            uint8_t *src_row = src_base + (rect_y + row) * row_bytes + src_col_offset;
                                            uint8_t *dst_row = dst_base + row * rect_row_bytes;
                                            memcpy(dst_row, src_row, rect_row_bytes);
                                        }

                                        // 推送临时canvas
                                        if (trans)
                                        {
                                            temp->pushSprite(rect_x, rect_y, invert ? TFT_BLACK : TFT_WHITE);
                                        }
                                        else
                                        {
                                            temp->pushSprite(rect_x, rect_y);
                                        }
                                    }
                                    delete temp;
                                }
                            }
                            else
                            {
                                // 推送整个canvas
                                if (trans)
                                {
                                    canvas->pushSprite(0, 0, invert ? TFT_BLACK : TFT_WHITE);
                                }
                                else
                                {
                                    canvas->pushSprite(0, 0);
                                }
                            }
                        }
                    };

                    // 确定实际宽高：如果width和height都为0，使用默认值
                    int actual_width = (msg.width == 0 && msg.height == 0) ? PAPER_S3_WIDTH : msg.width;
                    int actual_height = (msg.width == 0 && msg.height == 0) ? PAPER_S3_HEIGHT : msg.height;
                    
                    perform_push(use_canvas, msg.flags[0], msg.flags[1], msg.effect, msg.x, msg.y, actual_width, actual_height);

                    // 如果使用了quality模式，推送后恢复fastest模式
                    // if (useQualityMode || needMiddleStep)
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
            
            // 恢复标记：显示推送完成
            inDisplayPush = false;
        }
    }
    M5.Display.powerSaveOn();
}

bool initializeDisplayPushTask(size_t queue_len)
{
    if (s_displayQueue != NULL)
        return true;
    s_displayQueue = xQueueCreate(queue_len, sizeof(DisplayPushMessage));
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
}

bool enqueueDisplayPush(const DisplayPushMessage &msg)
{
    if (s_displayQueue == NULL)
        return false;
    BaseType_t res = xQueueSendToBack(s_displayQueue, &msg, 0);
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
