#include "readpaper.h"
#include "ui_canvas_utils.h"
#include "../device/ui_display.h"
#include "../text/bin_font_print.h"
#include "../device/wifi_hotspot_manager.h"
#include "../device/efficient_file_scanner.h"
#include "../device/book_file_manager.h"
#include "test/per_file_debug.h"
#include <M5Unified.h>
#include "ui_canvas_image.h"
#include "text/book_handle.h"
#include <SD.h>
#include "../SD/SDWrapper.h"
#include "text/font_buffer.h"
#include <vector>
#include <string>
#include <cctype>
#include <cmath>
#include <algorithm>
#include "tasks/device_interrupt_task.h"

// RAII wrapper for File to ensure proper close
class AutoCloseFile
{
private:
    File f;

public:
    AutoCloseFile(File file) : f(file) {}
    ~AutoCloseFile()
    {
        if (f)
            f.close();
    }
    File &get() { return f; }
    operator bool() const { return (bool)f; }
    // Prevent copying
    AutoCloseFile(const AutoCloseFile &) = delete;
    AutoCloseFile &operator=(const AutoCloseFile &) = delete;
};

// 外部全局Canvas
extern M5Canvas *g_canvas;
#include "current_book.h"
#include "toc_display.h"
extern int16_t target_page;
// keep the main menu page state in sync with the state machine
extern int current_file_page;
// 全局配置
extern GlobalConfig g_config;

// 全局文件列表缓存
static std::vector<std::string> cached_book_files;
static bool file_list_cached = false;
// 当为 true 时，主菜单文件列表来源于 SD 上的 /history.list
bool show_recent = false;

// Public helper: shorten book name for display.
std::string shorten_book_name(const std::string &orig, size_t cutlength)
{
    const std::string &s = orig;
    // collect UTF-8 codepoint start indices
    std::vector<size_t> idx;
    idx.reserve(s.size());
    for (size_t i = 0; i < s.size();)
    {
        idx.push_back(i);
        unsigned char c = (unsigned char)s[i];
        size_t adv = 1;
        if ((c & 0x80) == 0)
            adv = 1;
        else if ((c & 0xE0) == 0xC0)
            adv = 2;
        else if ((c & 0xF0) == 0xE0)
            adv = 3;
        else if ((c & 0xF8) == 0xF0)
            adv = 4;
        else
            adv = 1;
        i += adv;
    }
    size_t cp_count = idx.size();
    if (cp_count < cutlength + 4)
        return orig; // too short to bother

    // find last two ASCII digits by codepoint index
    std::vector<int> digit_pos; // positions in idx
    for (int i = (int)idx.size() - 1; i >= 0 && digit_pos.size() < 2; --i)
    {
        size_t b = idx[i];
        if ((unsigned char)s[b] < 0x80 && std::isdigit((unsigned char)s[b]))
        {
            digit_pos.push_back(i);
        }
    }
    if (digit_pos.size() < 2)
        return orig;

    // build ab in original left-to-right order (earlier, later)
    std::string ab;
    ab.push_back(s[idx[digit_pos[1]]]);
    ab.push_back(s[idx[digit_pos[0]]]);

    // prepare byte ranges to remove (may be single-byte for ASCII digits)
    std::vector<std::pair<size_t, size_t>> ranges;
    for (int p : digit_pos)
    {
        size_t b = idx[p];
        size_t e = (p + 1 < (int)idx.size()) ? idx[p + 1] : s.size();
        ranges.emplace_back(b, e);
    }
    // sort ascending
    std::sort(ranges.begin(), ranges.end());

    // build a new string without those two codepoints
    std::string no_digits;
    size_t cur = 0;
    for (auto &r : ranges)
    {
        if (cur < r.first)
            no_digits.append(s.substr(cur, r.first - cur));
        cur = r.second;
    }
    if (cur < s.size())
        no_digits.append(s.substr(cur));

    // collect codepoint starts for new string
    std::vector<size_t> idx2;
    idx2.reserve(no_digits.size());
    for (size_t i = 0; i < no_digits.size();)
    {
        idx2.push_back(i);
        unsigned char c = (unsigned char)no_digits[i];
        size_t adv = 1;
        if ((c & 0x80) == 0)
            adv = 1;
        else if ((c & 0xE0) == 0xC0)
            adv = 2;
        else if ((c & 0xF0) == 0xE0)
            adv = 3;
        else if ((c & 0xF8) == 0xF0)
            adv = 4;
        else
            adv = 1;
        i += adv;
    }
    if (idx2.empty())
        return orig;

    size_t take = cutlength;
    if (take > idx2.size())
        take = idx2.size();
    size_t prefix_byte_len = (take < idx2.size()) ? idx2[take] : no_digits.size();
    std::string prefix = no_digits.substr(0, prefix_byte_len);

    std::string out = prefix + ".." + ab;
    return out;
}

// 外部可能定义全局 canvas，show_reading_menu 接收 canvas 指针以保证可重入性
// 打算用整张图片来取代界面
bool show_reading_menu(M5Canvas *canvas, bool refresh, readingMenuArea area)
{
#if DBG_UI_CANVAS_UTILS
    unsigned long menu_start_time = millis();
    Serial.printf("[MENU] === 开始加载阅读菜单 ===\n");
    Serial.printf("[MENU] 菜单开始时间: %lu ms\n", menu_start_time);
#endif

    if (!canvas)
    {
#if DBG_UI_CANVAS_UTILS
        Serial.printf("[MENU] 错误: canvas为空\n");
#endif
        return false;
    }

#if DBG_UI_CANVAS_UTILS
    unsigned long image_start_time = millis();
    Serial.printf("[MENU] 开始加载图片资源: %lu ms\n", image_start_time);
#endif

    //    ui_push_image_to_display_direct("/spiffs/clock.png", 220, 430);

    //    ui_push_image_to_canvas("/spiffs/top.png", 0, 0);
    //     ui_push_image_to_canvas("/spiffs/lower.png", 0, 740);
    // drawMiddleUI(canvas, 0, 300);
    drawTopUI(canvas, 0, 0);
    drawBottomUI(canvas, 0, 600);

#if DBG_UI_CANVAS_UTILS
    unsigned long image_end_time = millis();
    Serial.printf("[MENU] 图片加载完成: %lu ms，耗时: %lu ms\n", image_end_time, image_end_time - image_start_time);
    Serial.printf("[MENU] 开始获取书籍信息: %lu ms\n", image_end_time);
#endif

    // Bookname
    // 当前书名
    std::string path = getBookFilePath(g_current_book);
    // 去掉路径，只保留文件名
    size_t pos = path.find_last_of("/\\");
    std::string name = (pos == std::string::npos) ? path : path.substr(pos + 1);
    // 去掉扩展名
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);
    // 对书名做特殊短化以便区分同系列多卷（use public helper）
    name = shorten_book_name(name, 12);
    // 拼接页码信息
    size_t cur_page = 1, total_page = 1;
    if (g_current_book)
    {
#if DBG_UI_CANVAS_UTILS
        unsigned long page_info_start = millis();
        Serial.printf("[MENU] 开始获取页码信息: %lu ms\n", page_info_start);
#endif
        cur_page = g_current_book->getCurrentPageIndex() + 1;
        total_page = g_current_book->getTotalPages();
#if DBG_UI_CANVAS_UTILS
        unsigned long page_info_end = millis();
        Serial.printf("[MENU] 页码信息获取完成: %lu ms，耗时: %lu ms (当前页: %zu/%zu)\n",
                      page_info_end, page_info_end - page_info_start, cur_page, total_page);
#endif
    }
    // Check the item
    char name_with_page[128];
    snprintf(name_with_page, sizeof(name_with_page), "%s ", name.c_str());

#if DBG_UI_CANVAS_UTILS
    unsigned long text_render_start = millis();
    Serial.printf("[MENU] 开始渲染文本内容: %lu ms\n", text_render_start);
#endif

    // 书名
    // Bottom
    // bin_font_print(name_with_page, 21, 0, 540, 0, 770, false, nullptr, TEXT_ALIGN_CENTER); // 0.7f * 30 = 21
    //  Top
    bin_font_print(name_with_page, 21, 0, 540, 0, 5, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.7f * 30 = 21
    snprintf(name_with_page, sizeof(name_with_page), "%zu", cur_page);
    // 页码
    // bin_font_print(name_with_page, 24, 0, 540, 0, 800, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
    bin_font_print(name_with_page, 28, 0, 540, 0, 775, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24
    snprintf(name_with_page, sizeof(name_with_page), "%zu", total_page);
    bin_font_print(name_with_page, 28, 0, 540, 0, 815, true, nullptr, TEXT_ALIGN_CENTER, 300); // 0.8f * 30 = 24

    canvas->drawWideLine(PAPER_S3_WIDTH / 2 - 20, 809, PAPER_S3_WIDTH / 2 + 20, 809, 1.8f, TFT_BLACK);

    // sync target_page
    target_page = cur_page;

    // label
    if (g_current_book && !g_current_book->isIndexingComplete())
    {
        // 显示索引百分比（四舍五入到整数）
        float prog = g_current_book->getIndexingProgress();
        if (prog < 0.0f)
            prog = 0.0f;
        if (prog > 100.0f)
            prog = 100.0f;
        int pct = (int)(prog + 0.5f);
        char progress_text[64];
        snprintf(progress_text, sizeof(progress_text), "索引中 %d%%", pct);
        bin_font_print(progress_text, 24, 0, 170, 96, 146, true, nullptr, TEXT_ALIGN_CENTER, 300);
    }
    else
    {
        bin_font_print("重新索引", 28, 0, 170, 88, 144, true, nullptr, TEXT_ALIGN_CENTER, 300);
    }

    // 获取阅读时间记录

    // bin_font_print("已读 ", 24, 0, 50, 310, 146, false, nullptr, TEXT_ALIGN_LEFT, 120, true, true, false, true);
    bin_font_print("已读 ", 28, 0, 50, 300, 144, false, nullptr, TEXT_ALIGN_LEFT, 120, false, false, false, true);
    // 已读时间小时数/分钟数：从bm文件获取（已与rec文件第一行同步）
    char read_hour_str[16] = "0";
    char read_min_str[8] = "00";
    if (g_current_book)
    {
        int rh = g_current_book->getReadHour();
        int rm = g_current_book->getReadMin();
        // clamp negative values just in case
        if (rh < 0)
            rh = 0;
        if (rm < 0)
            rm = 0;
        // clamp to reasonable maximums to avoid overflow in formatting
        if (rh > 9999)
            rh = 9999;
        if (rm > 59)
            rm = 59;
        snprintf(read_hour_str, sizeof(read_hour_str), "%d", rh);
        snprintf(read_min_str, sizeof(read_min_str), "%02d", rm);
    }
    // 已读时间小时数
    bin_font_print(read_hour_str, 28, 0, 80, 365, 144, true, nullptr, TEXT_ALIGN_CENTER, 80);
    //    bin_font_print(":", 24, 0, 120, 440, 146, true, nullptr, TEXT_ALIGN_CENTER, 30);
    // 已读时间分钟数
    // bin_font_print(read_min_str, 24, 0, 80, 452, 146, false, nullptr, TEXT_ALIGN_CENTER, 80, true, false, false, true);
    bin_font_print(read_min_str, 28, 0, 80, 452, 144, false, nullptr, TEXT_ALIGN_CENTER, 80, false, false, false, true);

    // 0间
    // 获取当前时间
    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
        char time_str[32];
        snprintf(time_str, sizeof(time_str), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        bin_font_print(time_str, 18, 0, 200, 24, 6, true, nullptr, TEXT_ALIGN_LEFT);
    }

#if DBG_UI_CANVAS_UTILS
    unsigned long text_render_end = millis();
    Serial.printf("[MENU] 文本渲染完成: %lu ms，耗时: %lu ms\n",
                  text_render_end, text_render_end - text_render_start);
    Serial.printf("[MENU] 开始屏幕刷新: %lu ms\n", text_render_end);
#endif
    if (refresh) // Proactive refresh
    {
        M5.Display.powerSaveOff();
        canvas->pushSprite(0, 0);
        M5.Display.waitDisplay();
        M5.Display.powerSaveOn();
    }
    else
    {
        // BOTTOMUI : y=600
        int BOTTOMUI_y = 600;
        int TOPUI_y = 0;

        switch (area)
        {
        case DARKMODE:
            bin_font_flush_canvas(false, false, false, NOEFFECT, 40, BOTTOMUI_y + 60 + 38, 460, 40);
            break;
        case FASTMODE:
            bin_font_flush_canvas(false, false, false, NOEFFECT, 40 + 260, BOTTOMUI_y + 60 + 38, 40, 40);
            break;
        case SKIPCONV:
            bin_font_flush_canvas(false, false, false, NOEFFECT, 42 + 3, TOPUI_y + 240 + 3, 24, 24);
            break;
        case UNDERLINE:
            bin_font_flush_canvas(false, false, false, NOEFFECT, 42 + 270 + 3, TOPUI_y + 60 + 3, 24, 24);
            break;
        case LOCKBM:
            bin_font_flush_canvas(false, false, false, NOEFFECT, 42 + 3, TOPUI_y + 60 + 3, 24, 24);
            break;
        default:
            bin_font_flush_canvas();
            break;
        }
    }

#if DBG_UI_CANVAS_UTILS
    unsigned long menu_end_time = millis();
    Serial.printf("[MENU] 屏幕刷新完成: %lu ms\n", menu_end_time);
    Serial.printf("[MENU] === 菜单加载完成，总耗时: %lu ms ===\n", menu_end_time - menu_start_time);
#endif

    return true;
}

// 通用按钮绘制：传入中心坐标和文字，inverted=true 表示黑底白字
// 新增参数 `ratio`（默认1.0）表示按钮缩放比例，会缩放按钮几何与文字大小
void draw_button(M5Canvas *canvas, int16_t cx, int16_t cy, const char *text, bool inverted, bool second, float ratio)
{
    const int16_t w = (int16_t)(164 * ratio);
    const int16_t h = (int16_t)(54 * ratio);

    int16_t off_top = (int16_t)(16 * ratio);
    int16_t off_a = (int16_t)(3 * ratio);
    int16_t off_b = (int16_t)(2 * ratio);
    int16_t off_c = (int16_t)(5 * ratio);
    int16_t off_d = (int16_t)(4 * ratio);
    int16_t off_e = (int16_t)(7 * ratio);
    int16_t off_f = (int16_t)(4 * ratio);
    int16_t off_g = (int16_t)(7 * ratio);
    int16_t off_h = (int16_t)(8 * ratio);

    canvas->drawRect(cx, cy - off_top + off_a, w, h, TFT_WHITE);
    // outer black fill to match existing style
    canvas->fillRect(cx + off_b, cy - off_top + off_c, w - 2 * off_b, h - 2 * off_b, TFT_BLACK);
    if (!inverted)
    {
        // inner white area
        canvas->fillRect(cx + off_d, cy - off_top + off_e, w - 2 * off_d, h - 2 * off_d, TFT_WHITE);
        canvas->drawRect(cx + off_f, cy - off_top + off_g, w - (2 * off_f), h - (2 * off_f), TFT_BLACK);
    }
    else
    {
        canvas->drawRect(cx + off_d, cy - off_top + off_e, w - 2 * off_d, h - 2 * off_d, TFT_WHITE);
    }
    if (false)
    // if (second)
    {
        if (!inverted)
            // inner white area
            canvas->fillRect(cx + off_h, cy - off_top + off_c, (int16_t)(3 * ratio), h - 2 * off_b, TFT_BLACK);
        else
            canvas->fillRect(cx + off_h, cy - off_top + off_c, (int16_t)(3 * ratio), h - 2 * off_b, TFT_WHITE);
    }

    // 文字颜色：反色时用白（15），否则用黑（0）
    //    int text_color = inverted ? 15 : 0;
    int text_color = 0;
    bool fastmode = inverted ? false : true;
    // 字体大小和可用宽度也按 ratio 缩放
    uint8_t font_size = (uint8_t)std::max(1, (int)roundf(32.0f * ratio));
    int16_t area_width = (int16_t)std::max(1, (int)(160 * ratio));
    int16_t max_length = area_width;
    bin_font_print(text, font_size, text_color, area_width, cx, cy - (int16_t)roundf(4.0f * ratio), fastmode, canvas, TEXT_ALIGN_CENTER, max_length, false, false, false, inverted);
}

void draw_label(M5Canvas *canvas, int16_t cx, int16_t cy, const char *text, bool inverted, bool second)
{

    // in label we don't need inverted.
    inverted = false;
    (void)cx;
    (void)cy;
    (void)inverted;
    (void)second;

    // 文字颜色：反色时用白（15），否则用黑（0）
    int text_color = inverted ? 15 : 0;
    bool fastmode = inverted ? false : true;
    bin_font_print((std::string(text) + ":").c_str(), 30, text_color, 160, cx, cy, fastmode, canvas, TEXT_ALIGN_LEFT, 160);

    canvas->drawFastHLine(cx, cy + 35, 130, TFT_BLACK);
    canvas->drawFastHLine(cx, cy + 38, PAPER_S3_WIDTH - 2 * cx, TFT_BLACK);
}

// 在指定 canvas 上绘制 UI
// 参数：
//   canvas -> 目标画布（LGFX_Sprite 或 M5.Display）
//   x, y   -> 绘制起点坐标
void drawTopUI(M5Canvas *canvas, int16_t x, int16_t y)
{
    // === 背景 ===
    // 覆盖整个 UI 区域 (宽 540, 高 230，可按需求调整)
    canvas->fillRect(x, y, 540, 300, TFT_WHITE);

    // 绘制四个角落的螺丝
    drawScrew(canvas, x + 15, y + 45);
    drawScrew(canvas, 525, y + 45);
    drawScrew(canvas, x + 15, y + 285);
    drawScrew(canvas, 525, y + 285);

    // 1. 左上水平线
    // canvas->fillRect(x + 20, y + 30, 50, 2, TFT_BLACK);
    canvas->fillRect(x, y + 30, 540, 2, TFT_BLACK);

    // 2. 锁屏书签复选框
    bool showLabel = (g_current_book && g_current_book->getShowLabel());
    drawCheckbox(canvas, x + 42, y + 60, showLabel, "锁屏书签", 30);

    // 2.5. 下划线复选框（移到右上位置）
    if (g_current_book)
    {
        bool drawBottom = g_current_book->getDrawBottom();
        drawCheckbox(canvas, x + 42 + 270, y + 60, drawBottom, "下划线", 30, 48);
    }
    // 2.6 划线与竖排

    // 2.6 跳过繁简转换复选框（移到左下位置）
    if (g_current_book)
    {
        bool keepOrg = g_current_book->getKeepOrg();
        drawCheckbox(canvas, x + 42, y + 240, keepOrg, "跳过繁简转换", 30);
    }

    // 在下划线右边添加竖排显示开关
    if (g_current_book)
    {
        // 竖排显示开关的位置
        const int16_t switch_x = x + 42 + 270;
        const int16_t switch_y = 244;

        // 绘制竖排显示开关
        drawSwitch(canvas, switch_x, switch_y - 6, g_current_book->getVerticalText(), "竖排", 30);
    }

    // 右上方框
    //    canvas->fillRect(x + 42 +270, y + 160, 30, 30, TFT_BLACK);
    //   canvas->fillRect(x + 45 +270, y + 163, 24, 24, TFT_WHITE);

    // 3. 右上电池格子 - 根据电量决定格子数量
    int x0 = x + 460;
    int y0 = y + 5;

    // 获取电池电量百分比
    // int batteryLevel = M5.Power.getBatteryLevel(); // Dropped, to avoid the risk instability of power meter's query time;
    // int batteryLevel = 100;
    int batteryLevel = DeviceInterruptTask::getLastBatteryPercentage(); // Relying on periodly query result

    // 每20%电量代表一个格子 (0-20%:1格, 21-40%:2格, ..., 81-100%:5格)
    int batteryBars = (batteryLevel + 19) / 20; // 向上取整除法
    if (batteryBars > 5)
        batteryBars = 5; // 最大5格
    if (batteryBars < 1)
        batteryBars = 1; // 最小1格

    for (int i = 0; i < batteryBars; i++)
    {
        canvas->fillRect(x0 + i * 9, y0, 6, 16, TFT_BLACK);
    }

    // 4. 顶部分隔线
    canvas->drawLine(x, y + 110, x + 540, y + 110, TFT_BLACK);

    // 4.5 时间窗口
    // canvas->fillRect(300, 140, 65, 38, TFT_BLACK);
    canvas->fillRect(280, y + 130, 85, 60, TFT_BLACK);
    canvas->drawRect(365, y + 130, 175, 60, TFT_BLACK);
    canvas->fillRect(452, y + 130, 88, 60, TFT_BLACK);

    // 5. 大矩形框
    canvas->fillRect(x + 40, y + 130, 230, 60, TFT_BLACK);
    canvas->fillRect(x + 42, y + 132, 226, 56, TFT_WHITE);

    // 6. 左侧小矩形（包含感叹号）
    canvas->fillRect(x + 42, y + 132, 56, 56, TFT_LIGHTGRAY);

    // 感叹号竖条
    canvas->fillRect(x + 64, y + 139, 10, 25, TFT_BLACK);

    // 感叹号底部小块
    canvas->fillRect(x + 64, y + 170, 10, 10, TFT_BLACK);

    // SPLIT
    canvas->drawLine(x, y + 210, x + 540, y + 210, TFT_BLACK);

    // 菜单底栏
    canvas->fillRect(x, y + 300, 540, 2, TFT_BLACK);
}

// 在指定 canvas 上绘制 Bottom UI
// 参数：
//   canvas -> 目标画布（M5Canvas 或 M5.Display）
//   x, y   -> 绘制起点坐标
void drawBottomUI(M5Canvas *canvas, int16_t x, int16_t y)
{
    // === 背景 ===
    canvas->fillRect(x, y + 20, 540, 360, TFT_WHITE);
    y = y + 20;
    // 0. 章节名分隔线
    canvas->fillRect(x, y, 540, 2, TFT_BLACK);
    drawScrew(canvas, x + 15, y + 20);
    drawScrew(canvas, 525, y + 20);

    // 章节名（如果存在）: 优先使用内存 idx 缓存查找当前页对应的章节
    if (g_current_book && g_current_book->isIndexed())
    {
        size_t cur_page_index = g_current_book->getCurrentPageIndex();
        size_t page_start = g_current_book->getPageStart(cur_page_index);
        if (page_start == (size_t)-1)
            page_start = g_current_book->position();

        size_t toc_idx = 0;
        int toc_page = -1;
        int toc_row = -1;
        bool on_current = false;

        if (find_toc_entry_for_position(g_current_book->filePath(), page_start,
                                        toc_idx, toc_page, toc_row, on_current))
        {
            std::string title;
            // Fast path: load only the TOC page containing toc_idx (<= TOC_ROWS entries)
            if (get_toc_title_for_index(g_current_book->filePath(), toc_idx, title))
            {
                bin_font_print(title.c_str(), 24, 0, 435, 45, y + 8, false, canvas, TEXT_ALIGN_CENTER, 435);
            }
            else
            {
                // kick off background load of the needed TOC page and render placeholder immediately
                start_async_load_toc_page(g_current_book->filePath(), toc_page);
                bin_font_print(" ", 24, 0, 435, 45, y + 8, false, canvas, TEXT_ALIGN_CENTER, 435);
            }
        }
        else
        {
            bin_font_print(" ", 24, 0, 435, 45, y + 8, false, canvas, TEXT_ALIGN_CENTER, 435);
        }
    }
    else
    {
        bin_font_print(" ", 24, 0, 435, 45, y + 8, false, canvas, TEXT_ALIGN_CENTER, 435);
    }

    //  Push whole y 20px down
    y = y + 40;

    // 1. 顶部分隔线
    canvas->fillRect(x, y, 540, 2, TFT_BLACK);
    // Move the space
    drawSwitch(canvas, x + 40, y + 38, g_config.dark, "深色模式", 30);
    // dark 模式下快刷开关显示为灰色（不可修改）
    // drawSwitch(canvas, x + 260, y + 38, g_config.fastrefresh, "快刷模式", 30, g_config.dark ? 8 : 0);
    drawSwitch(canvas, x + 260, y + 38, g_config.fastrefresh, "快刷模式", 30, 0);
    // Delete Line in darkmode
    if (g_config.dark)
    {
        canvas->drawWideLine(x + 260, y + 53, x + 260 + 190, y + 53, 1.5f, TFT_BLACK);
    }

    // 5. 书签目录相关
    /*
    canvas->fillCircle(x + 280, y + 50, 32, TFT_WHITE);
    canvas->fillCircle(x + 280, y + 50, 30, TFT_BLACK);
    canvas->fillCircle(x + 280, y + 50, 27, TFT_WHITE);
    // ui_push_image_to_canvas("/spiffs/tags.png", 280-13, 590-13);
    bin_font_print("标", 32, 0, 540, x+280 - 16, y+50- 16 , false, canvas, TEXT_ALIGN_LEFT, 540);
    bin_font_print("记显示", 25, !g_config.dark ? 0 : 15, 540, x+280+30, y +50 - 8 , true, canvas, TEXT_ALIGN_LEFT, 540);
    */
    //    draw_button(canvas, x + 330, y + 40, "使用帮助", false);
    canvas->fillCircle(x + 450 + 45, y + 50, 22, TFT_BLACK);
    canvas->fillCircle(x + 450 + 45, y + 50, 20, TFT_WHITE);
    canvas->fillCircle(x + 450 + 45, y + 50, 18, TFT_BLACK);
    bin_font_print("?", 32, 0, 50, x + 450 + 22, y + 50 - 16, false, canvas, TEXT_ALIGN_CENTER, 80, true, false, false, true);

    // ROW II
    y = y + 100;
    // 2. 顶部功能栏（4个图标：菜单-、水平线、+、菜单）
    // 2.1. 顶部分隔线
    canvas->fillRect(x, y, 540, 2, TFT_BLACK);

    // 左快进

    canvas->fillTriangle(x + 43, y + 50, x + 63, y + 40, x + 63, y + 60, TFT_BLACK);
    canvas->fillTriangle(x + 27, y + 50, x + 47, y + 40, x + 47, y + 60, TFT_BLACK);

    // 减号
    canvas->fillTriangle(x + 135, y + 50, x + 155, y + 40, x + 155, y + 60, TFT_BLACK);

    // 中间长横线
    //    canvas->fillRect(x + 160, y + 78, 220, 2, TFT_BLACK);

    // 加号
    canvas->fillTriangle(x + 415, y + 50, x + 395, y + 40, x + 395, y + 60, TFT_BLACK);

    // 右快进

    canvas->fillTriangle(x + 450 + 63, y + 50, x + 450 + 43, y + 40, x + 450 + 43, y + 60, TFT_BLACK);
    canvas->fillTriangle(x + 450 + 47, y + 50, x + 450 + 27, y + 40, x + 450 + 27, y + 60, TFT_BLACK);

    // ROW III
    // 3. 中间分隔线
    y = y + 100;

    canvas->fillRect(x, y, 540, 2, TFT_BLACK);

    // 4. 底部三个图标
    drawHome(canvas, x + 50, y + 30);
    drawLock(canvas, x + 245, y + 30);
    drawPower(canvas, x + 440, y + 30);
}

// 绘制中间UI区域，带弧形分割
void drawMiddleUI(M5Canvas *canvas, int16_t x, int16_t y)
{
    // 画弧形分割线，原点(0,480)，半径270，角度60-120
    // 左区淡灰
    //    canvas->fillArc(0, 480, 300, 602, 320, 400, !g_config.dark?TFT_WHITE:TFT_BLACK);
    //   canvas->fillArc(0, 480, 301, 306, 320, 400, !g_config.dark?TFT_LIGHTGRAY:TFT_LIGHTGRAY);

    /*
    canvas->fillCircle(280, 370, 32, TFT_WHITE);
    canvas->fillCircle(280, 370, 30, TFT_BLACK);
    canvas->fillCircle(280, 370, 27, TFT_WHITE);
    // ui_push_image_to_canvas("/spiffs/index.png", 280-13, 370-13);
    bin_font_print("", 32, 0, 540, 280-16, 370 - 16, false, canvas, TEXT_ALIGN_LEFT, 540);
    bin_font_print("记书签", 25, !g_config.dark?0:15, 540, 315, 370 - 8, true, canvas, TEXT_ALIGN_LEFT, 540);
    */
    /*
    canvas->fillCircle(300, 480, 32, TFT_WHITE);
    canvas->fillCircle(300, 480, 30, TFT_BLACK);
    canvas->fillCircle(300, 480, 27, TFT_WHITE);
    // ui_push_image_to_canvas("/spiffs/addmark.png", 300-13, 480-13);
    bin_font_print("标", 32, 0, 540, 300-16, 480 - 16, false, canvas, TEXT_ALIGN_LEFT, 540);
    bin_font_print("记书签", 25, !g_config.dark?0:15, 540, 335, 480 - 8, true, canvas, TEXT_ALIGN_LEFT, 540);
    */
    int8_t deltay = 64;
    canvas->fillCircle(280, 590 + deltay, 32, TFT_WHITE);
    canvas->fillCircle(280, 590 + deltay, 30, TFT_BLACK);
    canvas->fillCircle(280, 590 + deltay, 27, TFT_WHITE);
    // ui_push_image_to_canvas("/spiffs/tags.png", 280-13, 590-13);
    bin_font_print("标", 32, 0, 540, 280 - 16, 590 - 16 + deltay, false, canvas, TEXT_ALIGN_LEFT, 540);
    bin_font_print("记显示", 25, !g_config.dark ? 0 : 15, 540, 315, 590 - 8 + deltay, true, canvas, TEXT_ALIGN_LEFT, 540);
}

void drawPower(M5Canvas *canvas, int16_t x, int16_t y)
{
    // 右图标（电源插座样式）
    /*
    canvas->fillRect(x, y + 20, 50, 20, TFT_BLACK);
    canvas->fillRect(x + 10, y, 30, 50, TFT_BLACK);
    canvas->fillRect(x + 20, y, 10, 20, TFT_WHITE);
    */
    canvas->fillRoundRect(x, y, 50, 50, 3, TFT_BLACK);
    canvas->fillArc(x + 25, y + 25, 10, 16, 320, 220, TFT_WHITE);
    canvas->fillRect(x + 22, y + 8, 6, 15, TFT_WHITE);
}

void drawLock(M5Canvas *canvas, int16_t x, int16_t y)
{

    canvas->fillRoundRect(x, y, 50, 50, 3, TFT_BLACK);

    canvas->fillArc(x + 25, y + 20, 10, 13, 180, 360, TFT_WHITE);

    canvas->fillCircle(x + 25, y + 30, 4, TFT_BLACK);
    canvas->fillCircle(x + 25, y + 32, 3, TFT_BLACK);

    canvas->fillRect(x + 10, y + 23, 30, 20, TFT_WHITE);
}

void drawHome(M5Canvas *canvas, int16_t x, int16_t y)
{
    canvas->fillRoundRect(x, y, 50, 50, 3, TFT_BLACK);

    canvas->fillRect(x, y + 35, 20, 5, TFT_WHITE);
    canvas->fillRect(x, y + 25, 20, 2, TFT_WHITE);

    canvas->fillRect(x + 22, y + 2, 6, 46, TFT_WHITE);
    canvas->fillRect(x + 24, y + 2, 1, 46, TFT_BLACK);
    canvas->fillRect(x + 26, y + 2, 1, 46, TFT_BLACK);

    canvas->fillRect(x + 32, y + 2, 6, 46, TFT_WHITE);

    canvas->fillRect(x + 44, y + 15, 2, 15, TFT_WHITE);
}

// Draw a two-rect switch control at (x,y).
// The control consists of two 20x20 rects (left + right). Outer border is black.
// Right rect is white filled. Left rect is black when `on==true`, otherwise light gray.
// `text` is drawn to the right of the control.
void drawSwitch(M5Canvas *canvas, int16_t x, int16_t y, bool on, const char *text, uint8_t fsize, uint16_t tcolor)
{
    const int16_t w = 30;
    const int16_t h = 30;

    // Outer border for both rects together (40x20)
    canvas->fillRect(x - 1, y - 1, w * 2 + 2, h + 2, TFT_WHITE);
    canvas->fillRect(x, y, w * 2, h, TFT_BLACK);
    canvas->fillRect(x + 2, y + 2, w * 2 - 4, h - 4, TFT_WHITE);

    // Left inner fill (state)
    uint16_t leftColor = on ? TFT_BLACK : TFT_LIGHTGRAY;
    canvas->fillRoundRect(x + 2, y + 2, w - 2, h - 4, 0, leftColor);
    canvas->drawWideLine(x + w, y + 2, x + w, y + h - 4, 1, TFT_BLACK);
    // Right inner fill (always white)
    canvas->fillRoundRect(x + w + 2, y + 2, w - 4, h - 4, 0, TFT_WHITE);

    // Draw the text to the right of the switch, slightly vertically centered
    if (text && text[0] != '\0')
    {
        // Use the project's bin_font_print helper for consistent rendering
        // x position: a few pixels after the switch area
        int16_t tx = x + w * 2 + 8;
        int16_t ty = y + 2; // small offset to vertically align with the 20px box
        bin_font_print(text, fsize, tcolor, 160, tx, ty + (30 - fsize) / 2 - 2, true, canvas, TEXT_ALIGN_LEFT, 160);
    }
}

// Draw a screw at the specified position (x, y).
// The screw consists of a circle with a horizontal line through it.
void drawScrew(M5Canvas *canvas, int16_t x, int16_t y)
{
    // Draw the screw head (circle)
    canvas->fillCircle(x, y, 6, TFT_BLACK);
    canvas->fillCircle(x, y, 4, TFT_WHITE);
    // Draw the screw slot (horizontal line)
    canvas->drawLine(x - 4, y, x + 4, y, TFT_BLACK);
}

// Draw a checkbox at the specified position with text.
// x, y: position of the checkbox (30x30 black/white box)
// checked: whether to show the check mark
// text: label text to display
// fsize: font size for the text
// text_x_offset: horizontal offset for the text from the checkbox position
void drawCheckbox(M5Canvas *canvas, int16_t x, int16_t y, bool checked, const char *text, uint8_t fsize, int16_t text_x_offset)
{
    // Draw the checkbox frame (black border with white interior)
    canvas->fillRect(x, y, 30, 30, TFT_BLACK);
    canvas->fillRect(x + 3, y + 3, 24, 24, TFT_WHITE);

    // Draw the text label
    if (text && text[0] != '\0')
    {
        bin_font_print(text, fsize, 0, 540, x + text_x_offset, y + (30 - fsize) / 2, true, nullptr, TEXT_ALIGN_LEFT, 400);
    }

    // Draw the check mark if checked
    if (checked)
    {
        ui_push_image_to_canvas("/spiffs/icon-check.png", x, y);
    }
}

bool show_main_menu(M5Canvas *canvas, bool refresh, int selected, int current_page, bool rescan, bool partial, int8_t refInd)
{
    // RefInd
    //  1- Paging Files
    int16_t p_x = 0;
    int16_t p_y = 0;
    int16_t p_w = 0;
    int16_t p_h = 0;

#if DBG_UI_CANVAS_UTILS
    unsigned long menu_start_time = millis();
    Serial.printf("[MAIN_MENU] === 开始加载主菜单 (selected=%d, page=%d) ===\n", selected, current_page);
    Serial.printf("[MAIN_MENU] 菜单开始时间: %lu ms\n", menu_start_time);
#endif

    if (!canvas)
    {
#if DBG_UI_CANVAS_UTILS
        Serial.printf("[MAIN_MENU] 错误: canvas为空\n");
#endif
        return false;
    }

    // 清理书名列表和TOC缓存，避免索引期间缓存与字体文件状态不一致导致乱码
    // 这些缓存在下次需要时会自动重建
    clearBookNameCache();
    clearTocCache();
#if DBG_UI_CANVAS_UTILS
    Serial.printf("[MAIN_MENU] 已清理书名和TOC缓存以防止索引期间的冲突\n");
#endif

    // Ensure the global page index used by the state machine matches the
    // page that we're about to render. Some callers render page 0 but do
    // not update the global; synchronise here to avoid mismatches.
    current_file_page = current_page;

#if DBG_UI_CANVAS_UTILS
    unsigned long fill_start_time = millis();
    Serial.printf("[MAIN_MENU] 开始填充白色背景: %lu ms\n", fill_start_time);
#endif

    bool invertColor = false;
    // 用全白色矩形填充整个屏幕
    canvas->fillRect(0, 0, 540, 960, TFT_WHITE);

#if DBG_UI_CANVAS_UTILS
    unsigned long fill_end_time = millis();
    Serial.printf("[MAIN_MENU] 白色背景填充完成: %lu ms，耗时: %lu ms\n",
                  fill_end_time, fill_end_time - fill_start_time);
    Serial.printf("[MAIN_MENU] 开始屏幕刷新: %lu ms\n", fill_end_time);
#endif

    // 文件列表

    for (int i = 0; i < 10; i++)
    {
        canvas->fillRect(0, 96 * (i + 1), 360, 2, TFT_BLACK);
        if (i % 2 == 0)
        {
            // canvas->fillRect(0, 96 * i, 360, 96, 0xFFEE);
        }
    }

    // 左右分割线
    //    canvas->fillRect(360, 96 * 0, 3, 96 * 10, invertColor?TFT_WHITE:TFT_BLACK);

    // 上一页
    canvas->drawCircle(450, 60, 27, invertColor ? TFT_WHITE : TFT_BLACK);
    canvas->fillCircle(450, 60, 25, invertColor ? TFT_WHITE : TFT_BLACK);
    canvas->fillTriangle(437, 65, 450, 50, 463, 65, invertColor ? TFT_BLACK : TFT_WHITE);

    // 下一页
    canvas->drawCircle(450, 160, 27, invertColor ? TFT_WHITE : TFT_BLACK);
    canvas->fillCircle(450, 160, 25, invertColor ? TFT_WHITE : TFT_BLACK);
    canvas->fillTriangle(437, 155, 450, 170, 463, 155, invertColor ? TFT_BLACK : TFT_WHITE);

    // 繁简切换开关（显示在“按文件名”上方）
    // 当 g_config.zh_conv_mode == 2 时显示为 繁体 (on), ==1 时显示为 简体 (off)
    bool zh_on = false;
    if (g_config.zh_conv_mode == 2)
        zh_on = true;
    // draw the zh switch slightly to the left of the recent/files switch
    drawSwitch(g_canvas, 373, 230, zh_on, zh_on ? "繁体" : "简体", 24, invertColor ? TFT_WHITE : TFT_BLACK);

    drawSwitch(g_canvas, 373, 300, show_recent, show_recent ? "最近" : "书名", 24, invertColor ? TFT_WHITE : TFT_BLACK);

    // SD status

    /*
    uint64_t totalBytes = SDW::SD.totalBytes();
    uint64_t usedBytes = SDW::SD.usedBytes();
    uint64_t freeBytes = totalBytes - usedBytes;

    //    canvas->fillRect(370, 300, 160, 12, TFT_BLACK);
    //    canvas->fillRect(373, 303, 154 * freeBytes / totalBytes, 6, TFT_WHITE);
        // 在进度条下方显示已用/总大小
        {
            char sd_info[128];
            // helper: convert bytes to human readable string (B, KB, MB, GB)
            auto to_readable = [](uint64_t bytes, char *out, size_t out_sz)
            {
                const char *units[] = {"B", "KB", "MB", "GB", "TB"};
                double v = (double)bytes;
                int u = 0;
                while (v >= 1024.0 && u < 4)
                {
                    v /= 1024.0;
                    ++u;
                }
                if (v >= 100.0)
                    snprintf(out, out_sz, "SD: %.0f%s", v, units[u]);
                else if (v >= 10.0)
                    snprintf(out, out_sz, "SD: %.1f%s", v, units[u]);
                else
                    snprintf(out, out_sz, "SD: %.2f%s", v, units[u]);
            };

            char used_str[32] = {0};
            char total_str[32] = {0};

            to_readable(usedBytes, used_str, sizeof(used_str));
            if (totalBytes > 0)
            {
                to_readable(totalBytes, total_str, sizeof(total_str));
                // snprintf(sd_info, sizeof(sd_info), "%s/%s", used_str, total_str);
                snprintf(sd_info, sizeof(sd_info), "%s", used_str);
            }
            else
            {
                // snprintf(sd_info, sizeof(sd_info), "%s/未知", used_str);
                snprintf(sd_info, sizeof(sd_info), "%s", used_str);
            }
            // 在进度条下方绘制文本（x=400 与进度条对齐，y 设在进度条下方）
    //        canvas->drawLine(360, 384, 540, 384, TFT_BLACK);
            bin_font_print(sd_info, 20, 0, 160, 370, 384 - 24, true, canvas, TEXT_ALIGN_CENTER, 160);
        }
    (void)freeBytes; // suppress unused variable warning
    */

    // FONT TOGGLE - 中心y=528 (96*5+48)

    // FONT TOGGLE - 中心y=528 (96*5+48)
    draw_button(g_canvas, 370, 490 - 72, "字体", false, true);

    // OPEN BOOK - 中心y=528 (96*5+48)
    draw_button(g_canvas, 370, 512, "打开", true);

    // CLEAN BOOKMARK - 中心y=624 (96*6+48)
    draw_button(g_canvas, 370, 608, "清理", false, true);

    // FLIP SCREEN - 中心y=720 (96*7+48)
    draw_button(g_canvas, 370, 704, "显示", false, true);

    // WiFi Connect - 中心y=816 (96*8+48)
    // 按用户要求，主菜单的“连接”按钮不再反色（invert=false）。点击会打开二级菜单“连接方式”。
    draw_button(g_canvas, 370, 800, "连接", false);

    // Return - 中心y=912 (96*9+48) 反色显示
    draw_button(g_canvas, 370, 896, "返回", true);

    // Fetch & show list
    std::vector<std::string> book_files;

#if DBG_UI_CANVAS_UTILS
    unsigned long file_scan_start = millis();
    Serial.printf("[MAIN_MENU] 开始获取文件列表 (rescan=%d, cached=%d): %lu ms\n", rescan, file_list_cached, file_scan_start);
#endif

    // 计算是否需要实际扫描（rescan 或缓存为空）
    bool need_scan = rescan || !file_list_cached || cached_book_files.empty();

    // 如果需要重新扫描或缓存为空，则扫描文件系统
    if (need_scan)
    {
#if DBG_UI_CANVAS_UTILS
        Serial.printf("[MAIN_MENU] 执行文件扫描...\n");
#endif

        // 使用专用的书籍文件管理器
        cached_book_files = BookFileManager::getAllBookNames();

        file_list_cached = true;
#if DBG_UI_CANVAS_UTILS
        Serial.printf("[MAIN_MENU] 文件扫描完成，缓存已更新，找到%d个.txt文件\n", (int)cached_book_files.size());
#endif
    }
    else
    {
#if DBG_UI_CANVAS_UTILS
        Serial.printf("[MAIN_MENU] 使用缓存的文件列表，共%d个文件\n", (int)cached_book_files.size());
#endif
    }

    // 使用缓存的文件列表，或在 show_recent 为 true 时使用 SD 上的 /history.list
    book_files = cached_book_files;

    if (show_recent)
    {
        std::vector<std::string> history_files;
        const char *HPATH = "/history.list";
        if (SDW::SD.exists(HPATH))
        {
            AutoCloseFile hf(SDW::SD.open(HPATH, "r"));
            if (hf)
            {
                while (hf.get().available())
                {
                    String line = hf.get().readStringUntil('\n');
                    line.trim();
                    if (line.length() == 0)
                        continue;
                    std::string s = std::string(line.c_str());
                    // extract basename (remove folders)
                    size_t pos = s.find_last_of("/\\");
                    std::string name = (pos == std::string::npos) ? s : s.substr(pos + 1);
                    // remove extension
                    size_t dot = name.find_last_of('.');
                    if (dot != std::string::npos)
                        name = name.substr(0, dot);
                    history_files.push_back(name);
                }
                // File will be automatically closed when hf goes out of scope
            }
        }

        // If history file missing or empty, disable show_recent and fall back to normal list
        if (history_files.empty())
        {
            show_recent = false;
            // ensure we have a valid scanned list
            if (!file_list_cached || cached_book_files.empty())
            {
                cached_book_files = BookFileManager::getAllBookNames();
                file_list_cached = true;
            }
            book_files = cached_book_files;
        }
        else
        {
            book_files = history_files;
        }
    }

#if DBG_UI_CANVAS_UTILS
    unsigned long file_scan_end = millis();
    if (need_scan)
    {
        Serial.printf("[MAIN_MENU] 文件扫描完成(实际扫描): %lu ms，耗时: %lu ms，找到%d个.txt文件\n",
                      file_scan_end, file_scan_end - file_scan_start, (int)book_files.size());
    }
    else
    {
        Serial.printf("[MAIN_MENU] 文件列表准备完成(使用缓存): %lu ms，耗时: %lu ms，找到%d个.txt文件\n",
                      file_scan_end, file_scan_end - file_scan_start, (int)book_files.size());
    }
#endif

    // 计算分页信息
    int total_files = (int)book_files.size();
    int total_pages = (total_files + FILES_PER_PAGE - 1) / FILES_PER_PAGE; // 向上取整

    // 边界检查：确保 current_page 在有效范围内
    if (current_page < 0)
    {
        current_page = 0;
    }
    else if (total_pages > 0 && current_page >= total_pages)
    {
        current_page = total_pages - 1;
    }

    int page_start = current_page * FILES_PER_PAGE;
    int page_end = std::min(page_start + FILES_PER_PAGE, total_files);

#if DBG_UI_CANVAS_UTILS
    Serial.printf("[MAIN_MENU] 分页信息: 总文件数=%d, 当前页=%d, 总页数=%d, 页面范围=%d-%d\n",
                  total_files, current_page + 1, total_pages, page_start, page_end - 1);
#endif

    // 在右侧显示页数信息
    if (total_files > 0 && total_pages > 0)
    {
        char page_info[64]; // 增加缓冲区大小
        snprintf(page_info, sizeof(page_info), "第%d页/共%d页", current_page + 1, total_pages);
        bin_font_print(page_info, 20, 0, 180, 360, 100, false, g_canvas, TEXT_ALIGN_CENTER, 180);

        //        char file_count[64]; // 增加缓冲区大小
        //       snprintf(file_count, sizeof(file_count), "共%d个文件", total_files);
        //      bin_font_print(file_count, 17, TFT_BLACK, 180, 360, 115, true, g_canvas, TEXT_ALIGN_CENTER, 180); // 8=深灰色
    }
    else if (total_files == 0)
    {
        //        bin_font_print("未找到.txt文件", 18, 8, 160, 380, 40, true, g_canvas, TEXT_ALIGN_LEFT, 150); // 8=深灰色
    }

    // 在左侧显示当前页的文件列表
    canvas->fillRect(0, 0, 3, 960, invertColor ? TFT_WHITE : TFT_BLACK);
    int files_to_show = page_end - page_start;
    for (int i = 0; i < files_to_show; i++)
    {
        int file_index = page_start + i;
        // 计算文本显示位置：每行96px高，文本垂直居中
        int16_t text_y = 96 * i + 48 - 12; // 96*i是行起始位置，+48是行中心，-12是文本高度的一半

        // 如果是选中的项，绘制选中
        if (selected >= 0 && i == selected)
        {
            /*
            g_canvas->fillRect(355, 0, 5, 960, TFT_WHITE);
            g_canvas->fillTriangle(360, 96 * selected, 360, 96 * selected + 96, 355, 96 * selected + 48, TFT_BLACK);
            */
            canvas->fillRect(360, 0, 2, 960, invertColor ? TFT_WHITE : TFT_BLACK);
            canvas->fillRect(360, 96 * selected + 2, 2, 94, invertColor ? TFT_BLACK : TFT_WHITE);
            canvas->fillRect(0, 96 * selected + 2, 2, 94, invertColor ? TFT_BLACK : TFT_WHITE);

            canvas->fillRect(0, 96 * selected, (selected == 0) ? 540 : 360, 2, invertColor ? TFT_WHITE : TFT_BLACK);
            canvas->fillRect(0, 96 * selected + 96, (selected == 9) ? 540 : 360, 2, invertColor ? TFT_WHITE : TFT_BLACK);

#if DBG_UI_CANVAS_UTILS
            Serial.printf("[MAIN_MENU] 高亮选中文件 %d: %s\n", i, book_files[file_index].c_str());
#endif
        }

        // 显示文件名（去掉.txt后的名称），短名处理以便区分同系列卷次
        std::string display_name = shorten_book_name(book_files[file_index], 8);
        bin_font_print(display_name.c_str(), 28, 0, 320, 15, text_y, true, g_canvas, TEXT_ALIGN_LEFT, 320);

#if DBG_UI_CANVAS_UTILS
        Serial.printf("[MAIN_MENU] 显示文件 %d (索引%d): %s at y=%d\n", i, file_index, book_files[file_index].c_str(), text_y);
#endif
    }

    //    canvas->drawTriangle(440, 50, 450, 30, 460, 50);

    // 如果当前页未填满 (例如最后一页)，用白色矩形覆盖右侧/下方的空白槽，
    // 让视觉上显得为空白区域，并且后续触摸逻辑会忽略这些区域。
    // files_per_page 在上方已定义，避免重复声明
    if (files_to_show < FILES_PER_PAGE)
    {
        // 保留最后一个已显示项目底部的分割线（高度为2px），从分割线下面开始清除空白
        int y_start = 96 * files_to_show + 2; // 从分割线下方 2px 开始
        int h = 96 * (FILES_PER_PAGE - files_to_show) - 2;
        if (h < 0)
            h = 0;
        // 覆盖左侧文件列表区域的剩余槽位（不抹掉最后一条分割线）
        canvas->fillRect(0, y_start, 360, h, TFT_WHITE);
        // 也覆盖竖分割线右侧的区域（确保右侧按钮区不受影响）
        // （原本在顶部绘制的分割线与按钮，通过覆盖左侧360px区域即可）
    }

    drawScrew(canvas, 375, 12);
    drawScrew(canvas, 525, 12);

    drawScrew(canvas, 375, 96 * 4);
    drawScrew(canvas, 525, 96 * 4);

    drawScrew(canvas, 375, 948);
    drawScrew(canvas, 525, 948);

    if (refresh) // Proactive refresh
    {
        M5.Display.powerSaveOff();
        canvas->pushSprite(0, 0);
        M5.Display.waitDisplay();
        M5.Display.powerSaveOn();
    }
    else
    {
        if (partial)
        {
            if (refInd == 1)
            { // paging
                p_x = 0;
                p_y = 0;
                p_h = PAPER_S3_HEIGHT;
                p_w = 362;
                bin_font_flush_canvas(false, false, false, NOEFFECT, p_x, p_y, p_w, p_h);
                p_x = 370;
                p_y = 100;
                p_h = 22;
                p_w = 170;
                bin_font_flush_canvas(false, false, false, NOEFFECT, p_x, p_y, p_w, p_h);
            }
            else if (refInd == 2)
            {
                p_x = 0;
                p_y = 0;
                p_h = PAPER_S3_HEIGHT;
                p_w = 362;
                bin_font_flush_canvas(false, false, false, NOEFFECT, p_x, p_y, p_w, p_h);
                p_x = 373;
                p_y = 300;
                p_h = 50;
                p_w = 180;
                bin_font_flush_canvas(false, false, false, NOEFFECT, p_x, p_y, p_w, p_h);
                p_x = 370;
                p_y = 100;
                p_h = 22;
                p_w = 170;
                bin_font_flush_canvas(false, false, false, NOEFFECT, p_x, p_y, p_w, p_h);
             }
            else
            {
                bin_font_flush_canvas();
            }
        }
        else
            bin_font_flush_canvas();
    }

#if DBG_UI_CANVAS_UTILS
    unsigned long menu_end_time = millis();
    Serial.printf("[MAIN_MENU] 屏幕刷新完成: %lu ms\n", menu_end_time);
    Serial.printf("[MAIN_MENU] === 主菜单加载完成，总耗时: %lu ms ===\n",
                  menu_end_time - menu_start_time);
#endif
    return true;
}

// 获取缓存的书籍文件数量
int get_cached_book_count()
{
    if (show_recent)
    {
        // read /history.list and return its line count
        const char *HPATH = "/history.list";
        if (!SDW::SD.exists(HPATH))
            return 0;
        int count = 0;
        AutoCloseFile hf(SDW::SD.open(HPATH, "r"));
        if (!hf)
            return 0;
        while (hf.get().available())
        {
            String line = hf.get().readStringUntil('\n');
            line.trim();
            if (line.length() == 0)
                continue;
            ++count;
        }
        // File will be automatically closed when hf goes out of scope
        return count;
    }

    if (!file_list_cached || cached_book_files.empty())
    {
        // 使用专用的书籍文件管理器
        cached_book_files = BookFileManager::getAllBookNames();
        file_list_cached = true;
    }

    return (int)cached_book_files.size();
}

// 获取指定页面和索引的书籍文件名
std::string get_cached_book_name(int page, int index)
{
    // 确保缓存已加载
    if (!file_list_cached || cached_book_files.empty())
    {
        get_cached_book_count(); // 这会加载缓存
    }

    int absolute_index = page * FILES_PER_PAGE + index;

    if (show_recent)
    {
        // read the history list but return the display name (basename without ext)
        const char *HPATH = "/history.list";
        if (!SDW::SD.exists(HPATH))
            return std::string();
        AutoCloseFile hf(SDW::SD.open(HPATH, "r"));
        if (!hf)
            return std::string();
        int cur = 0;
        while (hf.get().available())
        {
            String line = hf.get().readStringUntil('\n');
            line.trim();
            if (line.length() == 0)
                continue;
            if (cur == absolute_index)
            {
                std::string s = std::string(line.c_str());
                size_t pos = s.find_last_of("/\\");
                std::string name = (pos == std::string::npos) ? s : s.substr(pos + 1);
                size_t dot = name.find_last_of('.');
                if (dot != std::string::npos)
                    name = name.substr(0, dot);
                // File will be automatically closed when hf goes out of scope
                return name;
            }
            ++cur;
        }
        // File will be automatically closed when hf goes out of scope
        return std::string();
    }

    if (absolute_index >= 0 && absolute_index < (int)cached_book_files.size())
    {
        return cached_book_files[absolute_index];
    }

    return ""; // 返回空字符串表示无效索引
}

// Return the full path to the selected book. When show_recent==true read the raw path from /history.list.
// 统一返回以 /sd 开头的路径，仅接受 /sd/book/ 下的文件
std::string get_selected_book_fullpath(int page, int index)
{
    int absolute_index = page * FILES_PER_PAGE + index;
    if (show_recent)
    {
        const char *HPATH = "/history.list";
        if (!SDW::SD.exists(HPATH))
            return std::string();
        AutoCloseFile hf(SDW::SD.open(HPATH, "r"));
        if (!hf)
            return std::string();
        int cur = 0;
        while (hf.get().available())
        {
            String line = hf.get().readStringUntil('\n');
            line.trim();
            if (line.length() == 0)
                continue;
            if (cur == absolute_index)
            {
                std::string s = std::string(line.c_str());
                // File will be automatically closed when hf goes out of scope

                // 验证路径必须以 /sd/book/ 开头
                if (s.rfind("/sd/book/", 0) != 0)
                {
#if DBG_UI_CANVAS_UTILS
                    Serial.printf("[UI] get_selected_book_fullpath: 路径不符合要求 (必须以 /sd/book/ 开头): %s\n", s.c_str());
#endif
                    return std::string();
                }

                return s; // raw path e.g. /sd/book/file.txt
            }
            ++cur;
        }
        // File will be automatically closed when hf goes out of scope
        return std::string();
    }

    // default behavior: construct from cached name
    std::string name = get_cached_book_name(page, index);
    if (name.empty())
        return std::string();
    // 统一返回 /sd/book/<name>.txt 格式
    return std::string("/sd/book/") + name + ".txt";
}

bool show_wire_connect(M5Canvas *canvas, bool refresh)
{
#if DBG_UI_CANVAS_UTILS
    unsigned long wire_start_time = millis();
    Serial.printf("[WIRE_CONNECT] === 开始加载无线连接界面 ===\n");
    Serial.printf("[WIRE_CONNECT] 界面开始时间: %lu ms\n", wire_start_time);
#endif

    if (!canvas)
    {
#if DBG_UI_CANVAS_UTILS
        Serial.printf("[WIRE_CONNECT] 错误: canvas为空\n");
#endif
        return false;
    }

    // 全屏填充白色背景
    canvas->fillScreen(TFT_WHITE);

    // 标题
    bin_font_print("WiFi 文件传输", 36, 0, 540, 0, 60, true, canvas, TEXT_ALIGN_CENTER, 540);

    g_canvas->drawWideLine(0, 156, 540, 156, 1.5, TFT_BLACK);
    // WiFi热点信息显示区域
    int info_y = 156 + 56;
    int line_height = 50;

    if (g_wifi_hotspot && g_wifi_hotspot->isRunning())
    {
        // 检查内存状态
        if (ESP.getFreeHeap() < 8192)
        {
#if DBG_UI_CANVAS_UTILS
            Serial.printf("[WIRE_CONNECT] 内存不足 (%d bytes)，简化显示\n", ESP.getFreeHeap());
#endif
            bin_font_print("WiFi热点已启动", 28, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
            info_y += line_height + 30;
            bin_font_print("内存不足，请稍后刷新", 24, 1, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
        }
        else
        {
            // 显示热点已启动
            bin_font_print("WiFi热点状态: 已启动", 28, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
            info_y += line_height;

            // 安全地获取和显示SSID
            const char *ssid_ptr = g_wifi_hotspot->getSSID();
            if (ssid_ptr && strlen(ssid_ptr) > 0)
            {
                String ssid_text = "网络名称: " + String(ssid_ptr);
                bin_font_print(ssid_text.c_str(), 26, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
            }
            else
            {
                bin_font_print("网络名称: 获取失败", 26, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
            }
            info_y += line_height;

            // 安全地获取和显示密码
            const char *pass_ptr = g_wifi_hotspot->getPassword();
            if (pass_ptr && strlen(pass_ptr) > 0)
            {
                String pass_text = "密码: " + String(pass_ptr);
                bin_font_print(pass_text.c_str(), 26, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
            }
            else
            {
                bin_font_print("密码: 获取失败", 26, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
            }
            info_y += line_height;

            // 安全地获取和显示IP地址
            String ip_text = "IP地址: " + g_wifi_hotspot->getIPAddress();
            if (ip_text.length() < 50)
            { // 限制字符串长度
                bin_font_print(ip_text.c_str(), 26, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
            }
            else
            {
                bin_font_print("IP地址: 获取失败", 26, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
            }
            info_y += line_height;

            // 安全地获取和显示连接设备数
            /*
            int clientCount = g_wifi_hotspot->getConnectedClients();
            if (clientCount >= 0 && clientCount < 100)
            { // 合理的设备数范围
                String client_text = "连接设备: " + String(clientCount) + " 个";
                bin_font_print(client_text.c_str(), 26, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
            }
            else
            {
                bin_font_print("连接设备: 获取失败", 26, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
            }
            */
        }
        info_y += line_height + 6;
        g_canvas->drawWideLine(0, info_y, 540, info_y, 1.5, TFT_BLACK);

        info_y += line_height + 56;

        // 使用说明
        bin_font_print("使用手机或电脑连接WiFi后", 24, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
        info_y += 40;
        bin_font_print("使用浏览器插件（推荐）管理", 24, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
        info_y += 40;
        bin_font_print("或在浏览器中访问上面地址管理", 24, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
    }
    else
    {
        // 显示热点启动失败或未启动
        bin_font_print("WiFi热点状态: 未启动", 28, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
        info_y += line_height + 30;
        bin_font_print("热点启动失败", 24, 1, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
        info_y += 40;
        bin_font_print("可能原因:", 22, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
        info_y += 35;
        bin_font_print("• WiFi模块未正确初始化", 20, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
        info_y += 30;
        bin_font_print("• NVS存储问题", 20, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);
        info_y += 30;
        bin_font_print("• 重启设备可能解决问题", 20, 0, 540, 0, info_y, true, canvas, TEXT_ALIGN_CENTER, 540);

        // 添加重试按钮
        info_y += 60;
        //        canvas->drawRect(270, info_y, 180, 50, TFT_BLACK);
        //       canvas->drawRect(268, info_y - 2, 184, 54, TFT_DARKCYAN);
        // 绘制返回按钮边框
        canvas->drawRect(180, info_y + 168, 180, 50, TFT_BLACK);
        canvas->drawRect(178, info_y + 166, 184, 54, TFT_DARKCYAN);
        bin_font_print("重试启动", 24, 0, 540, 0, info_y + 181, true, canvas, TEXT_ALIGN_CENTER, 180);
    }

    // 绘制返回按钮边框
    canvas->drawRect(180, 768, 180, 50, TFT_BLACK);
    canvas->drawRect(178, 766, 184, 54, TFT_DARKCYAN);

    // 绘制返回按钮文字 "返回"
    bin_font_print("返回菜单", 28, 0, 180, 180, 779, true, canvas, TEXT_ALIGN_CENTER, 180);

#if DBG_UI_CANVAS_UTILS
    unsigned long wire_end_time = millis();
    Serial.printf("[WIRE_CONNECT] 无线连接界面绘制完成: %lu ms，总耗时: %lu ms\n",
                  wire_end_time, wire_end_time - wire_start_time);
#endif

    if (refresh) // Proactive refresh
    {
        M5.Display.powerSaveOff();
        canvas->pushSprite(0, 0);
        M5.Display.waitDisplay();
        M5.Display.powerSaveOn();
    }
    else
    {
        bin_font_flush_canvas();
    }

    return true;
}

// USB MSC mode page: shows instructions and a '复位退出' button that will reboot the device.
bool show_usb_connect(M5Canvas *canvas, bool refresh)
{
    if (!canvas && !g_canvas)
        return false;
    M5Canvas *target = canvas ? canvas : g_canvas;

    target->fillScreen(TFT_WHITE);
    bin_font_print("USB 模式 *", 36, 0, 540, 0, 60, true, target, TEXT_ALIGN_CENTER, 540);
    bin_font_print("* 实验特性，调试或临时应急使用", 26, 0, 540, 0, 120, true, target, TEXT_ALIGN_CENTER, 540);

    g_canvas->drawWideLine(0, 206, 540, 206, 1.5, TFT_BLACK);

    int info_y = 260;

    bin_font_print("* 尽量避免在当前书籍索引过程中使用", 28, 0, 540, 20, info_y, true, target, TEXT_ALIGN_LEFT, 540);
    info_y += 50;
    bin_font_print("* 已知限制，初始耗时较长（~2分钟）", 28, 0, 540, 20, info_y, true, target, TEXT_ALIGN_LEFT, 540);
    info_y += 50;
    bin_font_print("* 设备连接到电脑后，耐心等待", 28, 0, 540, 20, info_y, true, target, TEXT_ALIGN_LEFT, 540);
    info_y += 50;
    bin_font_print("* 用于调试目的，读写速度也低于无线", 28, 0, 540, 20, info_y, true, target, TEXT_ALIGN_LEFT, 540);
    info_y += 50;
    bin_font_print("* 选择'弹出'后,设备自动重启", 28, 0, 540, 20, info_y, true, target, TEXT_ALIGN_LEFT, 540);

    info_y += 84;
    g_canvas->drawWideLine(0, info_y, 540, info_y, 1.5, TFT_BLACK);

    // Draw the reset-exit button near bottom center
    int16_t btn_cx = 270;
    int16_t btn_cy = 720;
    draw_button(target, btn_cx - 82, btn_cy, "强制退出", true);

    if (refresh) // Proactive refresh
    {
        M5.Display.powerSaveOff();
        canvas->pushSprite(0, 0);
        M5.Display.waitDisplay();
        M5.Display.powerSaveOn();
    }
    else
    {
        bin_font_flush_canvas();
    }

    return true;
}