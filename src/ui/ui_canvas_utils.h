#pragma once
#include <M5Unified.h>
#include <string>

// 每页显示的文件数量，主菜单分页使用此常量
static constexpr int FILES_PER_PAGE = 10;

// 阅读菜单显示区域枚举
enum readingMenuArea {
    FULL = 0,      // 完整菜单
    LOCKBM,        // 书签锁定区域
    UNDERLINE,     // 下划线区域
    SKIPCONV,      // 跳过转换区域
    DARKMODE,      // 暗黑模式区域
    FASTMODE       // 快速模式区域
};

bool show_reading_menu(M5Canvas *canvas, bool refresh = true, readingMenuArea area = FULL);
bool show_main_menu(M5Canvas *canvas, bool refresh = true, int selected = 0, int current_page = 0, bool rescan = true, bool partial = false, int8_t refInd = 0);
bool show_wire_connect(M5Canvas *canvas, bool refresh = true);
bool show_usb_connect(M5Canvas *canvas, bool refresh = true);
int get_cached_book_count();
std::string get_cached_book_name(int page, int index);
// When show_recent is true, this returns the full path from /history.list for the selected item.
std::string get_selected_book_fullpath(int page, int index);
// Toggle source for main menu: when true, show recent history from /history.list
extern bool show_recent;
// Shorten book name for display: if name has >=2 trailing ASCII digits and
// length >= cutlength+4, move the last two digits to after the cutlength-th
// character and append an ellipsis marker. UTF-8 safe.
std::string shorten_book_name(const std::string &orig, size_t cutlength = 6);
void drawTopUI(M5Canvas *canvas, int16_t x=0, int16_t  y=0);
void drawBottomUI(M5Canvas *canvas, int16_t x=0, int16_t  y=0);
void drawMiddleUI(M5Canvas *canvas, int16_t x=0, int16_t y=0);
void drawHome(M5Canvas *canvas, int16_t x=0, int16_t y=0);
void drawLock(M5Canvas *canvas, int16_t x=0, int16_t y=0);
void drawPower(M5Canvas *canvas, int16_t x=0, int16_t y=0);
void draw_button(M5Canvas *canvas, int16_t cx, int16_t cy, const char *text, bool inverted=false, bool second=false, float ratio = 1.0f);
void draw_label(M5Canvas *canvas, int16_t cx, int16_t cy, const char *text, bool inverted=false, bool second=false);
void drawSwitch(M5Canvas *canvas, int16_t x, int16_t y, bool on, const char *text, uint8_t fsize=20, u16_t tcolor=0);
void drawScrew(M5Canvas *canvas, int16_t x, int16_t y);
void drawCheckbox(M5Canvas *canvas, int16_t x, int16_t y, bool checked, const char *text, uint8_t fsize=26, int16_t text_x_offset=48);