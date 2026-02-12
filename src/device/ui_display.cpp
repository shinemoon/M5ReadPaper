#include "readpaper.h"
#include "powermgt.h"
#include "ui_display.h"
#include "test/per_file_debug.h"
#include "text/book_handle.h"
#include "config/config_manager.h"
#include "file_manager.h"
#include "text/line_handle.h"
#include <SD.h>
#include <cstring>

extern GlobalConfig g_config;
extern M5Canvas *g_canvas;
#include "current_book.h"

void display_print(const char *text, float text_size, uint16_t text_color, uint8_t datum, // datum not used
                   int16_t margin_top, int16_t margin_bottom,
                   int16_t margin_left, int16_t margin_right,
                   uint16_t bg_color, bool fastmode, bool dark)
{
#if DBG_UI_DISPLAY
    Serial.printf("[DISPLAY_PRINT] 调用 display_print, text长度=%zu, text_size=%.2f, text_color=0x%04X, datum=%d, margin_top=%d, margin_bottom=%d, margin_left=%d, margin_right=%d, bg_color=0x%04X\n", text ? strlen(text) : 0, text_size, text_color, datum, margin_top, margin_bottom, margin_left, margin_right, bg_color);
    if (text)
        Serial.printf("[DISPLAY_PRINT] 文本预览: %.20s\n", text);
#endif

    // 使用 bin_font_print 替换原有显示逻辑
    std::string text_str = std::string(text);

    // 检查是否为垂直文本模式
    bool vertical = (g_current_book && g_current_book->getVerticalText());

    // 在垂直模式下，需要调整area参数
    int16_t area_width;
    if (vertical)
    {
        // 垂直模式下，"宽度"实际上是屏幕高度减去上下边距
        area_width = PAPER_S3_HEIGHT - margin_top - margin_bottom;
    }
    else
    {
        // 水平模式下，正常计算宽度
        area_width = PAPER_S3_WIDTH - margin_left - margin_right;
    }

#if DBG_UI_DISPLAY
    if (vertical)
    {
        Serial.printf("[DISPLAY_PRINT] 垂直模式: area_width=%d (基于高度%d-上边距%d-下边距%d)\n",
                      area_width, PAPER_S3_HEIGHT, margin_top, margin_bottom);
    }
    else
    {
        Serial.printf("[DISPLAY_PRINT] 水平模式: area_width=%d (基于宽度%d-左边距%d-右边距%d)\n",
                      area_width, PAPER_S3_WIDTH, margin_left, margin_right);
    }
#endif

    // 设置起始位置
    int16_t g_cursor_y = bin_font_get_cursor_y();

    bin_font_set_cursor(margin_left, g_cursor_y + margin_top);

    // 打印文本（会自动处理换行和光标管理）
    bool drawBottom = (g_current_book && g_current_book->getDrawBottom());
    bin_font_print(text_str, 0, 0, area_width, margin_left, margin_top, fastmode, g_canvas, TEXT_ALIGN_LEFT, 0, (g_current_book && g_current_book->getKeepOrg()), drawBottom, vertical, dark);

    // print but not flush
}

void display_print_wrapped(const char* text, int16_t x, int16_t y, int16_t area_width,
                          int16_t area_height, uint8_t font_size, uint8_t color, 
                          int16_t bg_color, uint8_t align, bool vertical, bool skip)
{
    if (!text || text[0] == '\0') {
        return;
    }

#if DBG_UI_DISPLAY
    Serial.printf("[DISPLAY_WRAPPED] 开始打印: x=%d, y=%d, area_width=%d, area_height=%d, font_size=%d, color=%d, align=%d, vertical=%s\n",
                  x, y, area_width, area_height, font_size, color, align, vertical ? "true" : "false");
#endif

    std::string input_text(text);
    std::string wrapped_text;
    
    // 获取字体的行高
    uint8_t base_font_size = get_font_size_from_file();
    if (base_font_size == 0) {
        base_font_size = 24; // 使用默认值
    }
    
    float scale_factor = (font_size > 0 && base_font_size > 0) ? 
                         ((float)font_size / (float)base_font_size) : 1.0f;
    int16_t line_height = (int16_t)((base_font_size + LINE_MARGIN) * scale_factor);
    
    // 计算最大行数
    int16_t available_height;
    if (area_height > 0) {
        // 使用指定的高度限制
        available_height = area_height;
    } else {
        // 0 表示不限制高度，使用屏幕剩余高度
        available_height = vertical ? area_width : (PAPER_S3_HEIGHT - y);
    }
    int max_lines = available_height / line_height;
    if (max_lines <= 0) {
        max_lines = 1;
    }
    
#if DBG_UI_DISPLAY
    Serial.printf("[DISPLAY_WRAPPED] line_height=%d, max_lines=%d, scale_factor=%.2f\n",
                  line_height, max_lines, scale_factor);
#endif
    
    // 逐行处理文本，自动换行
    size_t pos = 0;
    int lines_added = 0;
    
    while (pos < input_text.length() && lines_added < max_lines) {
        // 跳过行首空白（仅垂直模式）
        if (vertical) {
            while (pos < input_text.length()) {
                char c = input_text[pos];
                if (c != ' ' && c != '\t' && c != '\r') {
                    break;
                }
                pos++;
            }
        }
        
        if (pos >= input_text.length()) {
            break;
        }
        
        // 查找换行位置
        size_t break_pos = find_break_position_scaled(input_text, pos, area_width, vertical, font_size);
        
        if (break_pos == pos) {
            // 无法前进，避免无限循环
            break;
        }
        
        // 提取这一行的文本
        std::string line = input_text.substr(pos, break_pos - pos);
        wrapped_text += line;
        wrapped_text += '\n';
        
        pos = break_pos;
        lines_added++;
        
        // 跳过显式的换行符
        if (pos < input_text.length() && input_text[pos] == '\n') {
            pos++;
        }
        
#if DBG_UI_DISPLAY
        if (lines_added <= 3) {
            Serial.printf("[DISPLAY_WRAPPED] 第%d行: 长度=%zu, break_pos=%zu\n",
                          lines_added, line.length(), break_pos);
        }
#endif
    }
    
#if DBG_UI_DISPLAY
    Serial.printf("[DISPLAY_WRAPPED] 换行完成: 原始长度=%zu, 处理后长度=%zu, 行数=%d\n",
                  input_text.length(), wrapped_text.length(), lines_added);
#endif
    
    // 映射对齐方式：0=左对齐，1=居中，2=右对齐
    TextAlign text_align = TEXT_ALIGN_LEFT;
    if (align == 1) {
        text_align = TEXT_ALIGN_CENTER;
    } else if (align == 2) {
        text_align = TEXT_ALIGN_RIGHT;
    }
    
    // 调用 bin_font_print 打印换行后的文本
    bin_font_print(
        wrapped_text,
        font_size,           // font_size
        color,               // color (0-15 灰度)
        area_width,  // area_width
        x,                   // margin_left (起点x)
        y,                   // margin_top (起点y)
        false,               // fast_mode
        g_canvas,            // canvas
        text_align,          // text_align (使用传入的对齐方式)
        area_width,                   // max_length (不限制)
        skip,                // skipConv (跳过繁简转换)
        false,               // drawBottom
        vertical,            // vertical
        false                // dark
    );
}

void initDisplay()
{
#if DBG_UI_DISPLAY
    unsigned long displayStartTime = millis();
#endif

    // 优化：在任何显示操作之前，立即设置EPD为最快速模式
    // 这可以减少复位后的初始闪烁
    M5.Display.setEpdMode(epd_mode_t::epd_fastest); // 使用最快速模式
    M5.Display.setColorDepth(TEXT_COLORDEPTH);
#if DBG_UI_DISPLAY
    Serial.printf("[DISPLAY] EPD最快速模式设置: %lu ms\n", millis() - displayStartTime);
#endif

    // 使用封装的 set_rotation，确保在设置前后正确管理 power-save 状态
    display_set_rotation(g_config.rotation);
#if DBG_UI_DISPLAY
    Serial.printf("[DISPLAY] 屏幕旋转设置: %lu ms\n", millis() - displayStartTime);
#endif

// 优化：完全跳过直接屏幕操作，只使用Canvas缓冲
// 这可以避免复位时的多次全屏刷新
#if DBG_UI_DISPLAY
    Serial.printf("[DISPLAY] 跳过所有直接屏幕操作，仅使用Canvas: %lu ms\n", millis() - displayStartTime);
#endif

    // 创建Canvas缓冲 - 这不会触发屏幕刷新
    /* Try directly into lock screen! */
    //    bin_font_clear_canvas();
    //    show_startpage(PAPER_S3_WIDTH, PAPER_S3_HEIGHT, 30); // Fontsize not used as pure png
#if DBG_UI_DISPLAY
    Serial.printf("[DISPLAY] Canvas创建完成(无屏幕刷新): %lu ms\n", millis() - displayStartTime);
#endif

// 初始化完成后，切换到文本模式以获得更好的显示质量
#if DBG_UI_DISPLAY
    Serial.printf("[DISPLAY] 切换到EPD文本模式: %lu ms\n", millis() - displayStartTime);
    Serial.printf("[DISPLAY] 显示系统总耗时: %lu ms\n", millis() - displayStartTime);
#endif
    // rebuild font list and reload fonts
    font_list_scan();
    fontLoad();
}

void fontLoad()
{
#if DBG_UI_DISPLAY
    unsigned long fontStartTime = millis();
#endif

    extern GlobalConfig g_config;
    bool fontLoaded = false;

    // If a font is already loaded, unload it first to free resources
    const char *cur = get_current_font_name();
    if (cur && cur[0] != '\0')
    {
#if DBG_UI_DISPLAY
        Serial.printf("[DISPLAY] 卸载当前字体: %s\n", cur);
#endif
        unload_bin_font();
    }

    // 根据配置设置选择字体
#if DBG_UI_DISPLAY
    // 打印当前配置的字体路径与状态
    if (g_config.fontset && g_config.fontset[0] != '\0')
    {
        Serial.printf("[DISPLAY] g_config.fontset=\"%s\", len=%u\n", g_config.fontset, (unsigned)strlen(g_config.fontset));
    }
    else
    {
        Serial.println("[DISPLAY] g_config.fontset 为空或未设置");
    }
#endif

    if (load_bin_font(g_config.fontset))
    {
        fontLoaded = true;
#if DBG_UI_DISPLAY
        Serial.printf("[DISPLAY] 字体加载成功 : %lu ms\n", millis() - fontStartTime);
#endif
    }
    if (!fontLoaded)
    {
        // Config update - copy into the buffer (g_config.fontset is likely a char array)
        strncpy(g_config.fontset, "/spiffs/lite.bin", sizeof(g_config.fontset));
        g_config.fontset[sizeof(g_config.fontset) - 1] = '\0';
        config_save();
        if (!load_bin_font("/spiffs/lite.bin"))
        {
#if DBG_UI_DISPLAY
            Serial.printf("[DISPLAY] 默认字体加载失败: %lu ms\n", millis() - fontStartTime);
#endif
            return;
        }
#if DBG_UI_DISPLAY
        Serial.printf("[DISPLAY] 默认字体加载成功 (/spiffs/lite.bin): %lu ms\n", millis() - fontStartTime);
#endif
    }

    // 字体加载成功后，更新全局字体大小为字体文件中的实际大小
    extern float font_size;
    font_size = (float)get_font_size_from_file();

#if DBG_UI_DISPLAY
    Serial.printf("[DISPLAY] 字体大小: %.0f\n", font_size);
#endif

    // 关键修复：字体加载后，清空 canvas 并重置光标，确保初始状态正确
    // 这是为了修复上电初始化时，show_start_screen() 在字体加载前就调用了
    // bin_font_clear_canvas() 导致的状态不一致问题
    bin_font_clear_canvas(g_config.dark);
#if DBG_UI_DISPLAY
    Serial.println("[DISPLAY] 字体加载后重置 Canvas 状态");
#endif

    // Note: 不在这里更新 BookHandle 的 font_size
    // 让 jumpToPage/renderCurrentPage 中的检测逻辑自动发现字体大小变化
    // 并触发重索引，之后才会更新书签中的 font_size
    // 这样可以确保字体变化能够正确触发重索引
}

// Wrapper implementation: ensure power-save is off during rotation change
void display_set_rotation(int rotation)
{
#if DBG_UI_DISPLAY
    Serial.printf("[DISPLAY] set rotation wrapper: requested=%d\n", rotation);
#endif
    // 如果当前旋转已是目标值，则无需再次设置，避免触发无意义刷新
    int cur = M5.Display.getRotation();
    if (cur == rotation)
    {
#if DBG_UI_DISPLAY
        Serial.printf("[DISPLAY] rotation unchanged (%d), skip setRotation\n", rotation);
#endif
        return;
    }

    // Ensure display is awake for rotation change
    M5.Display.powerSaveOff();
    delay(10); // small delay to let controller wake
    M5.Display.setRotation(rotation);
    // Give the display controller a moment to settle
    delay(10);
    M5.Display.powerSaveOn();
}