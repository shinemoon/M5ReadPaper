#include <FS.h>
#include <SPIFFS.h>
#include <SD.h>
#include "../SD/SDWrapper.h"
#include <cstring>
#include "papers3.h"
#include "test/per_file_debug.h"
#include <M5Unified.h>
#include "../device/file_manager.h"
#include "../device/ui_display.h"
#include "ui_canvas_image.h"
#include "text/book_handle.h"

// 假设全局 Canvas 已初始化为 g_canvas
extern M5Canvas *g_canvas;
#include "current_book.h"

// 两级回退：先 /spiffs/screen.png，再 /spiffs/screenlow.png，然后放弃
static void ui_try_canvas_fallback(const char *current, int16_t x, int16_t y, M5Canvas *canvas)
{
    const char *fb1 = "/spiffs/screen.png";
    const char *fb2 = "/spiffs/screenlow.png";
    if (strcmp(current, fb1) != 0 && strcmp(current, fb2) != 0)
    {
        ui_push_image_to_canvas(fb1, x, y, canvas);
    }
    else if (strcmp(current, fb1) == 0 && strcmp(current, fb2) != 0)
    {
        ui_push_image_to_canvas(fb2, x, y, canvas);
    }
    else
    {
        // already tried both fallbacks or current is fb2 -> give up
    }
}

static void ui_try_display_fallback(const char *current, int16_t x, int16_t y)
{
    const char *fb1 = "/spiffs/screen.png";
    const char *fb2 = "/spiffs/screenlow.png";
    if (strcmp(current, fb1) != 0 && strcmp(current, fb2) != 0)
    {
        ui_push_image_to_display_direct(fb1, x, y);
    }
    else if (strcmp(current, fb1) == 0 && strcmp(current, fb2) != 0)
    {
        ui_push_image_to_display_direct(fb2, x, y);
    }
    else
    {
        // give up
    }
}

// 将图片推送到Canvas指定位置，不立即刷新
void ui_push_image_to_canvas(const char *img_path, int16_t x, int16_t y, M5Canvas *canvas, bool preClean)
{
    // 确定要使用的canvas
    M5Canvas *target_canvas = canvas ? canvas : g_canvas;

    if (!target_canvas)
    {
#if DBG_UI_IMAGE
        Serial.println("[UI_IMAGE] 错误: Canvas尚未初始化！应在main.cpp中创建。");
#endif
        return;
    }

    File imgFile;

    if (strncmp(img_path, "/spiffs/", 8) == 0)
    {
        imgFile = SPIFFS.open(img_path + 7, "r");
    }
    else if (strncmp(img_path, "/sd/", 4) == 0)
    {
        imgFile = SDW::SD.open(img_path + 3, "r"); // 不需要/sd前缀
    }
    else
    {
#if DBG_UI_IMAGE
        Serial.printf("[UI_IMAGE] 路径前缀不支持: %s\n", img_path);
#endif
        imgFile = SDW::SD.open(img_path, "r"); // 默认是SD卡的图
        // return;
    }
    if (!imgFile)
    {
#if DBG_UI_IMAGE
        Serial.printf("[UI_IMAGE] 打开图片失败: %s\n", img_path);
#endif
        return;
    }

    // 支持BMP/JPG/PNG，优先使用 Stream/File 重载来流式读取并交由底层解码器处理（避免一次性分配大块内存）
    size_t len = imgFile.size();
    if (len == 0)
    {
        imgFile.close();
        return;
    }

#if DBG_UI_IMAGE
    Serial.printf("[UI_IMAGE] 使用流式读取推送图片到Canvas: %s 位置(%d,%d) canvas=%s, size=%u\n",
                  img_path, x, y, canvas ? "自定义" : "全局g_canvas", (unsigned)len);
#endif

    if (preClean)
    {
        if (g_current_book)
        {
            g_current_book->renderCurrentPage(0, g_canvas, false, false, true);
        }
        else
        {
            // No book: clear canvas to avoid showing stale content
            g_canvas->clearDisplay(TFT_WHITE);
        }
    }

    // 使用 Stream 重载（fs::File 继承自 Stream）
    if (strstr(img_path, ".bmp"))
    {
        // reset to beginning just in case
        imgFile.seek(0);
        target_canvas->drawBmp(&imgFile, x, y);
    }
    else if (strstr(img_path, ".jpg") || strstr(img_path, ".jpeg"))
    {
        imgFile.seek(0);
        target_canvas->drawJpg(&imgFile, x, y);
    }
    else if (strstr(img_path, ".png"))
    {
        imgFile.seek(0);
        target_canvas->drawPng(&imgFile, x, y);
    }
    else
    {
#if DBG_UI_IMAGE
        Serial.printf("[UI_IMAGE] 不支持的图片格式: %s\n", img_path);
#endif
        ui_try_canvas_fallback(img_path, x, y, target_canvas);
    }

    imgFile.close();
}

// 直接在display上推送图片，计算真实可见区域，实现最快显示
void ui_push_image_to_display_direct(const char *img_path, int16_t x, int16_t y, bool preClean)
{
    M5.Display.powerSaveOff();
    File imgFile;

    if (strncmp(img_path, "/spiffs/", 8) == 0)
    {
        imgFile = SPIFFS.open(img_path + 7, "r");
    }
    else if (strncmp(img_path, "/sd/", 4) == 0)
    {
        imgFile = SDW::SD.open(img_path + 3, "r"); // 不需要/sd前缀
    }
    else
    {
#if DBG_UI_IMAGE
        Serial.printf("[UI_IMAGE_DIRECT] 路径前缀不支持: %s\n", img_path);
#endif
        return;
    }

    if (!imgFile)
    {
#if DBG_UI_IMAGE
        Serial.printf("[UI_IMAGE_DIRECT] 打开图片失败: %s\n", img_path);
#endif
        return;
    }

    size_t len = imgFile.size();
    if (len == 0)
    {
        imgFile.close();
        return;
    }

    // 直接在display上绘制图片：使用 Stream/File 重载以流式解码，避免一次性分配大缓冲区
    const int16_t screen_w = PAPER_S3_WIDTH;
    const int16_t screen_h = PAPER_S3_HEIGHT;

    // 检查位置是否完全超出屏幕范围
    if (x >= screen_w || y >= screen_h)
    {
#if DBG_UI_IMAGE
        Serial.printf("[UI_IMAGE_DIRECT] 图片位置超出屏幕: (%d,%d)\n", x, y);
#endif
        imgFile.close();
        return;
    }

#if DBG_UI_IMAGE
    Serial.printf("[UI_IMAGE_DIRECT] 使用流式读取直接显示图片: %s 位置(%d,%d) 大小=%u\n", img_path, x, y, (unsigned)len);
#endif

    // 使用 Stream/File 方式，由底层库流式解码
    if (strstr(img_path, ".bmp"))
    {
        imgFile.seek(0);
        M5.Display.drawBmp(&imgFile, x, y);
    }
    else if (strstr(img_path, ".jpg") || strstr(img_path, ".jpeg"))
    {
        imgFile.seek(0);
        M5.Display.drawJpg(&imgFile, x, y);
    }
    else if (strstr(img_path, ".png"))
    {
        imgFile.seek(0);
        M5.Display.drawPng(&imgFile, x, y);
    }
    else
    {
#if DBG_UI_IMAGE
        Serial.printf("[UI_IMAGE_DIRECT] 不支持的图片格式: %s\n", img_path);
#endif
        imgFile.close();
        ui_try_display_fallback(img_path, x, y);
        return;
    }

    imgFile.close();

#if DBG_UI_IMAGE
    Serial.printf("[UI_IMAGE_DIRECT] 完成直接显示图片: %s\n", img_path);
#endif
    M5.Display.powerSaveOff();
}