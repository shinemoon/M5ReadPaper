#include "ui_control.h"
#include "ui_canvas_utils.h"
#include "text/book_handle.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "test/per_file_debug.h"
#include <M5Unified.h>
#include "config/config_manager.h"

// 全局 BookHandle 实例（需要在其他地方初始化）
#include "current_book.h"
extern GlobalConfig g_config;

// 新的6×10网格触摸区域检测 (540×960屏幕，每格90×96px)
TouchZone getTouchZoneGrid(int16_t touch_x, int16_t touch_y)
{
    const int16_t SCREEN_WIDTH = 540;
    const int16_t SCREEN_HEIGHT = 960;
    const int16_t GRID_COLS = 6;
    const int16_t GRID_ROWS = 10;
    const int16_t CELL_WIDTH = SCREEN_WIDTH / GRID_COLS;   // 90px
    const int16_t CELL_HEIGHT = SCREEN_HEIGHT / GRID_ROWS; // 96px

    // 边界检查
    if (touch_x < 0 || touch_x >= SCREEN_WIDTH || touch_y < 0 || touch_y >= SCREEN_HEIGHT)
    {
        return TouchZone::UNKNOWN;
    }

    // 计算网格位置
    int col = touch_x / CELL_WIDTH;
    int row = touch_y / CELL_HEIGHT;

    // 确保在有效范围内
    if (col >= GRID_COLS)
        col = GRID_COLS - 1;
    if (row >= GRID_ROWS)
        row = GRID_ROWS - 1;

    // 映射到枚举值 (row * GRID_COLS + col)
    int grid_index = row * GRID_COLS + col;

    // 转换为对应的枚举值
    switch (grid_index)
    {
    case 0:
        return TouchZone::ONE_ONE;
    case 1:
        return TouchZone::ONE_TWO;
    case 2:
        return TouchZone::ONE_THREE;
    case 3:
        return TouchZone::ONE_FOUR;
    case 4:
        return TouchZone::ONE_FIVE;
    case 5:
        return TouchZone::ONE_SIX;

    case 6:
        return TouchZone::TWO_ONE;
    case 7:
        return TouchZone::TWO_TWO;
    case 8:
        return TouchZone::TWO_THREE;
    case 9:
        return TouchZone::TWO_FOUR;
    case 10:
        return TouchZone::TWO_FIVE;
    case 11:
        return TouchZone::TWO_SIX;

    case 12:
        return TouchZone::THREE_ONE;
    case 13:
        return TouchZone::THREE_TWO;
    case 14:
        return TouchZone::THREE_THREE;
    case 15:
        return TouchZone::THREE_FOUR;
    case 16:
        return TouchZone::THREE_FIVE;
    case 17:
        return TouchZone::THREE_SIX;

    case 18:
        return TouchZone::FOUR_ONE;
    case 19:
        return TouchZone::FOUR_TWO;
    case 20:
        return TouchZone::FOUR_THREE;
    case 21:
        return TouchZone::FOUR_FOUR;
    case 22:
        return TouchZone::FOUR_FIVE;
    case 23:
        return TouchZone::FOUR_SIX;

    case 24:
        return TouchZone::FIVE_ONE;
    case 25:
        return TouchZone::FIVE_TWO;
    case 26:
        return TouchZone::FIVE_THREE;
    case 27:
        return TouchZone::FIVE_FOUR;
    case 28:
        return TouchZone::FIVE_FIVE;
    case 29:
        return TouchZone::FIVE_SIX;

    case 30:
        return TouchZone::SIX_ONE;
    case 31:
        return TouchZone::SIX_TWO;
    case 32:
        return TouchZone::SIX_THREE;
    case 33:
        return TouchZone::SIX_FOUR;
    case 34:
        return TouchZone::SIX_FIVE;
    case 35:
        return TouchZone::SIX_SIX;

    case 36:
        return TouchZone::SEVEN_ONE;
    case 37:
        return TouchZone::SEVEN_TWO;
    case 38:
        return TouchZone::SEVEN_THREE;
    case 39:
        return TouchZone::SEVEN_FOUR;
    case 40:
        return TouchZone::SEVEN_FIVE;
    case 41:
        return TouchZone::SEVEN_SIX;

    case 42:
        return TouchZone::EIGHT_ONE;
    case 43:
        return TouchZone::EIGHT_TWO;
    case 44:
        return TouchZone::EIGHT_THREE;
    case 45:
        return TouchZone::EIGHT_FOUR;
    case 46:
        return TouchZone::EIGHT_FIVE;
    case 47:
        return TouchZone::EIGHT_SIX;

    case 48:
        return TouchZone::NINE_ONE;
    case 49:
        return TouchZone::NINE_TWO;
    case 50:
        return TouchZone::NINE_THREE;
    case 51:
        return TouchZone::NINE_FOUR;
    case 52:
        return TouchZone::NINE_FIVE;
    case 53:
        return TouchZone::NINE_SIX;

    case 54:
        return TouchZone::TEN_ONE;
    case 55:
        return TouchZone::TEN_TWO;
    case 56:
        return TouchZone::TEN_THREE;
    case 57:
        return TouchZone::TEN_FOUR;
    case 58:
        return TouchZone::TEN_FIVE;
    case 59:
        return TouchZone::TEN_SIX;

    default:
        return TouchZone::UNKNOWN;
    }
}

PageTurnResult handleReadingTouch(TouchZone zone)
{
    PageTurnResult result = {false, false, "未处理"};

    if (g_current_book == nullptr)
    {
        result.message = "没有打开的书籍";
        return result;
    }

    // 抑制未使用变量警告（这些变量在某些分支中可能会用到）

    switch (zone)
    {
    case TouchZone::LEFT_THIRD:
    case TouchZone::ONE_ONE:
    case TouchZone::TWO_ONE:
    case TouchZone::THREE_ONE:
    case TouchZone::FOUR_ONE:
    case TouchZone::FIVE_ONE:
    case TouchZone::SIX_ONE:
    case TouchZone::SEVEN_ONE:
    case TouchZone::EIGHT_ONE:
    case TouchZone::NINE_ONE:
    case TouchZone::TEN_ONE:

    case TouchZone::ONE_TWO:
    case TouchZone::TWO_TWO:
    case TouchZone::THREE_TWO:
    case TouchZone::FOUR_TWO:
    case TouchZone::FIVE_TWO:
    case TouchZone::SIX_TWO:
    case TouchZone::SEVEN_TWO:
    case TouchZone::EIGHT_TWO:
    case TouchZone::NINE_TWO:
    case TouchZone::TEN_TWO:

        if ((strcmp(g_config.pageStyle, "default") == 0 && M5.Display.getRotation() == g_config.rotation) ||
            (strcmp(g_config.pageStyle, "default") != 0 && M5.Display.getRotation() != g_config.rotation))
        {
#if DBG_UI_CONTROL
            Serial.println("[UI_CONTROL] 触摸左侧，向前翻页");
#endif
            // 避免在栈上创建大对象，使用指针或引用
            if (g_current_book->prevPage().success)
            {
                result.success = true;
                result.page_changed = true;
                result.message = "PREVPAGE";
            }
            else
            {
                result.success = false;
                result.message = "已是第一页";
            }
        }
        else
        {
#if DBG_UI_CONTROL
            Serial.println("[UI_CONTROL] 触摸左侧，向后翻页");
#endif

            // 避免在栈上创建大对象
            if (g_current_book->nextPage().success)
            {
                result.success = true;
                result.page_changed = true;
                result.message = "NEXTPAGE";
            }
            else
            {
                result.success = false;
                result.message = "已是最后一页";
            }
        }
        break;

    case TouchZone::RIGHT_THIRD:
    case TouchZone::ONE_FIVE:
    case TouchZone::TWO_FIVE:
    case TouchZone::THREE_FIVE:
    case TouchZone::FOUR_FIVE:
    case TouchZone::FIVE_FIVE:
    case TouchZone::SIX_FIVE:
    case TouchZone::SEVEN_FIVE:
    case TouchZone::EIGHT_FIVE:
    case TouchZone::NINE_FIVE:
    case TouchZone::TEN_FIVE:
    case TouchZone::ONE_SIX:
    case TouchZone::TWO_SIX:
    case TouchZone::THREE_SIX:
    case TouchZone::FOUR_SIX:
    case TouchZone::FIVE_SIX:
    case TouchZone::SIX_SIX:
    case TouchZone::SEVEN_SIX:
    case TouchZone::EIGHT_SIX:
    case TouchZone::NINE_SIX:
    case TouchZone::TEN_SIX:
// 向后翻页
#if DBG_UI_CONTROL
        Serial.println("[UI_CONTROL] 触摸右侧，向后翻页");
#endif
        if ((strcmp(g_config.pageStyle, "default") == 0 && M5.Display.getRotation() != g_config.rotation) ||
            (strcmp(g_config.pageStyle, "default") != 0 && M5.Display.getRotation() == g_config.rotation))

        {
            // 避免在栈上创建大对象，使用指针或引用
            if (g_current_book->prevPage().success)
            {
                result.success = true;
                result.page_changed = true;
                result.message = "PREVPAGE";
            }
            else
            {
                result.success = false;
                result.message = "已是第一页";
            }
        }
        else
        {
            // 避免在栈上创建大对象
            if (g_current_book->nextPage().success)
            {
                result.success = true;
                result.page_changed = true;
                result.message = "NEXTPAGE";
            }
            else
            {
                result.success = false;
                result.message = "已是最后一页";
            }
        }
        break;

    case TouchZone::FAKE_CURRENT:
// 触发显示当前页
#if DBG_UI_CONTROL
        Serial.println("[UI_CONTROL] FAKE TRIGGER CURRENT PAGE");
#endif
        {
            // 避免在栈上创建大对象
            result.success = true;
            result.page_changed = true;
            result.message = "CURRENTPAGE";
        }
        break;
    case TouchZone::FAKE_JUMP:
// 触发显示当前页
#if DBG_UI_CONTROL
        Serial.println("[UI_CONTROL] FAKE TRIGGER CURRENT PAGE");
#endif
        {
            // 避免在栈上创建大对象
            result.success = true;
            result.page_changed = true;
            result.message = "JUMPPAGE";
        }
        break;

    // Center Area FOR MENU
    case TouchZone::FIVE_THREE:
    case TouchZone::FIVE_FOUR:
    case TouchZone::SIX_THREE:
    case TouchZone::SIX_FOUR:
#if DBG_UI_CONTROL
        Serial.println("[UI_CONTROL] 触摸中间区域，唤醒菜单");
#endif
        result.success = true;
        result.page_changed = false;
        result.message = "MENU";
        break;

    case TouchZone::UNKNOWN:
#if DBG_UI_CONTROL
        Serial.println("[UI_CONTROL] 触摸未知区域");
#endif
        result.success = false;
        result.message = "未知触摸区域";
        break;
    default:
#if DBG_UI_CONTROL
        Serial.println("[UI_CONTROL] Others");
#endif
        result.success = false;
        result.message = "Other unused areas";
        break;
    }

    return result;
}

// 处理菜单状态下的触摸（使用 TouchZone 枚举）
MenuTouchResult handleMenuTouch(TouchZone zone)
{
    MenuTouchResult result = {false, false, false, false, false, "未处理"};
    /*
    bool success;
    bool button_pressed;  // 是否按下了圆形按钮
    bool panel_clicked;   // 是否点击了菜单面板（非按钮区域）
    bool outside_clicked; // 是否点击了菜单以外的区域
    const char* message;
    */

    switch (zone)
    {
        // In Panel
    case TouchZone::TEN_ONE:
    case TouchZone::TEN_TWO:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] GO HOME");
#endif
        result.success = true;
        result.panel_clicked = true;
        result.outside_clicked = false;
        result.message = "GO HOME";
        break;
    // In Button
    case TouchZone::TEN_THREE:
    case TouchZone::TEN_FOUR:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] BUTTON 内");
#endif
        result.success = true;
        result.button_pressed = true;      // 是否按下了圆形按钮
        result.button_pwr_pressed = false; // 是否按下了圆形按钮
        result.panel_clicked = true;
        result.outside_clicked = false;
        result.message = "CLICK LOCK AREA";
        break;
    // In Button
    case TouchZone::TEN_FIVE:
    case TouchZone::TEN_SIX:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] BUTTON 内");
#endif
        result.success = true;
        result.button_pressed = false;    // 是否按下了圆形按钮
        result.button_pwr_pressed = true; // 是否按下了圆形按钮
        result.panel_clicked = true;
        result.outside_clicked = false;
        result.message = "CLICK PWR AREA";
        break;
    // 前翻
    case TouchZone::NINE_TWO:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] NINE 区域：前翻页");
#endif
        result.success = true;
        result.message = "BWD 1%";
        break;
    case TouchZone::NINE_FIVE:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] NINE 区域：后翻页");
#endif
        result.success = true;
        result.message = "FWD 1%";
        break;
        // NINE_* 区域：单独处理，但暂不执行任何动作
    // 快速前翻
    case TouchZone::NINE_ONE:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] NINE 区域：前翻页F");
#endif
        result.success = true;
        result.message = "FBWD 10%";
        break;
    case TouchZone::NINE_SIX:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] NINE 区域：后翻页F");
#endif
        result.success = true;
        result.message = "FFWD 10%";
        break;
    case TouchZone::NINE_FOUR:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] NINE 区域：后翻页M");
#endif
        result.success = true;
        result.message = "MFWD 0.1%";
        break;
    case TouchZone::NINE_THREE:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] NINE 区域：前翻页M");
#endif
        result.success = true;
        result.message = "MBWD 0.1%";
        break;
    // Reindex
    case TouchZone::TWO_TWO:
    case TouchZone::TWO_THREE:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] TWO 区域：Reindex");
#endif
        result.success = true;
        result.message = "TWO 区域：ReIndex";
        break;
    case TouchZone::ONE_ONE:
    case TouchZone::ONE_TWO:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] ONE 区域：label control");
#endif
        result.success = true;
        result.message = "Switch Label";
        break;
    // ONE_* and TWO_* 区域：暂不处理
    case TouchZone::ONE_THREE:
    case TouchZone::ONE_FOUR:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] ONE 区域：drawBottom control");
#endif
        result.success = true;
        result.message = "Switch DrawBottom";
        break;
    case TouchZone::ONE_FIVE:
    case TouchZone::ONE_SIX:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] ONE 区域：swipe control");
#endif
        result.success = true;
        result.message = "Switch Swipe";
        break;
    case TouchZone::TWO_ONE:
    case TouchZone::TWO_FOUR:
    case TouchZone::TWO_FIVE:
    case TouchZone::TWO_SIX:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] ONE/TWO 区域：不在此处理");
#endif
        result.success = true;
        result.message = "ONE/TWO 区域：无动作";
        break;
    case TouchZone::THREE_ONE:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] THREE 区域：keepOrg control");
#endif
        result.success = true;
        result.message = "Switch KeepOrg";
        break;
    case TouchZone::THREE_FOUR:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] THREE 区域：Vertical control");
#endif
        result.success = true;
        result.message = "Switch Vertical";
        break;
    case TouchZone::THREE_TWO:
    case TouchZone::THREE_THREE:
    case TouchZone::THREE_FIVE:
    case TouchZone::THREE_SIX:
        result.success = true;
        result.panel_clicked = true;
        result.message = "No Action in THREE_*";
        break;
    case TouchZone::EIGHT_ONE:
        result.success = true;
        result.panel_clicked = true;
        result.message = "Switch DARK";
        break;
    case TouchZone::EIGHT_FOUR:
        result.success = true;
        result.panel_clicked = true;
        result.message = "Switch FAST";
        break;
    case TouchZone::FOUR_ONE:
    case TouchZone::FOUR_TWO:
    case TouchZone::FOUR_THREE:
    case TouchZone::FOUR_FOUR:
    case TouchZone::FOUR_FIVE:
    case TouchZone::FOUR_SIX:
        result.success = true;
        result.panel_clicked = true;
        result.message = "Star Row";
        break;
    case TouchZone::EIGHT_TWO:
    case TouchZone::EIGHT_THREE:
    case TouchZone::EIGHT_FIVE:
    case TouchZone::EIGHT_SIX:
        result.success = true;
        result.panel_clicked = true;
        result.message = "No Action in EIGHT_*";
        break;
    default:
#if DBG_UI_CONTROL
        Serial.println("[MENU TOUCH] 触摸PANEL外");
#endif
        result.success = true;
        result.button_pressed = false; // 是否按下了圆形按钮
        result.panel_clicked = false;
        result.outside_clicked = true;
        result.message = "PANEL 外触摸";
        break;
    }
    return result;
}

MenuTouchResult handleMainMenuTouch(TouchZone zone)
{
    MenuTouchResult result = {false, false, false, false, false, "未处理"};
    /*
    bool success;
    bool button_pressed;  // 是否按下了圆形按钮
    bool panel_clicked;   // 是否点击了菜单面板（非按钮区域）
    bool outside_clicked; // 是否点击了菜单以外的区域
    const char* message;
    */

    switch (zone)
    {
    case TouchZone::ONE_ONE:
    case TouchZone::ONE_TWO:
    case TouchZone::ONE_THREE:
    case TouchZone::ONE_FOUR:
    case TouchZone::TWO_ONE:
    case TouchZone::TWO_TWO:
    case TouchZone::TWO_THREE:
    case TouchZone::TWO_FOUR:
    case TouchZone::THREE_ONE:
    case TouchZone::THREE_TWO:
    case TouchZone::THREE_THREE:
    case TouchZone::THREE_FOUR:
    case TouchZone::FOUR_ONE:
    case TouchZone::FOUR_TWO:
    case TouchZone::FOUR_THREE:
    case TouchZone::FOUR_FOUR:
    case TouchZone::FIVE_ONE:
    case TouchZone::FIVE_TWO:
    case TouchZone::FIVE_THREE:
    case TouchZone::FIVE_FOUR:
    case TouchZone::SIX_ONE:
    case TouchZone::SIX_TWO:
    case TouchZone::SIX_THREE:
    case TouchZone::SIX_FOUR:
    case TouchZone::SEVEN_ONE:
    case TouchZone::SEVEN_TWO:
    case TouchZone::SEVEN_THREE:
    case TouchZone::SEVEN_FOUR:
    case TouchZone::EIGHT_ONE:
    case TouchZone::EIGHT_TWO:
    case TouchZone::EIGHT_THREE:
    case TouchZone::EIGHT_FOUR:
    case TouchZone::NINE_ONE:
    case TouchZone::NINE_TWO:
    case TouchZone::NINE_THREE:
    case TouchZone::NINE_FOUR:
    case TouchZone::TEN_ONE:
    case TouchZone::TEN_TWO:
    case TouchZone::TEN_THREE:
    case TouchZone::TEN_FOUR:
#if DBG_UI_CONTROL
        Serial.println("[MAIN_MENU TOUCH] 触摸区域：文件选择");
#endif
        result.success = true;
        result.panel_clicked = true;
        result.message = "SELECT BOOK:";
        {
            const char *zone_name = "UNKNOWN";
            switch (zone)
            {
            case TouchZone::ONE_ONE:
            case TouchZone::ONE_TWO:
            case TouchZone::ONE_THREE:
            case TouchZone::ONE_FOUR:
                zone_name = "0";
                break;
            case TouchZone::TWO_ONE:
            case TouchZone::TWO_TWO:
            case TouchZone::TWO_THREE:
            case TouchZone::TWO_FOUR:
                zone_name = "1";
                break;
            case TouchZone::THREE_ONE:
            case TouchZone::THREE_TWO:
            case TouchZone::THREE_THREE:
            case TouchZone::THREE_FOUR:
                zone_name = "2";
                break;
            case TouchZone::FOUR_ONE:
            case TouchZone::FOUR_TWO:
            case TouchZone::FOUR_THREE:
            case TouchZone::FOUR_FOUR:
                zone_name = "3";
                break;
            case TouchZone::FIVE_ONE:
            case TouchZone::FIVE_TWO:
            case TouchZone::FIVE_THREE:
            case TouchZone::FIVE_FOUR:
                zone_name = "4";
                break;
            case TouchZone::SIX_ONE:
            case TouchZone::SIX_TWO:
            case TouchZone::SIX_THREE:
            case TouchZone::SIX_FOUR:
                zone_name = "5";
                break;
            case TouchZone::SEVEN_ONE:
            case TouchZone::SEVEN_TWO:
            case TouchZone::SEVEN_THREE:
            case TouchZone::SEVEN_FOUR:
                zone_name = "6";
                break;
            case TouchZone::EIGHT_ONE:
            case TouchZone::EIGHT_TWO:
            case TouchZone::EIGHT_THREE:
            case TouchZone::EIGHT_FOUR:
                zone_name = "7";
                break;
            case TouchZone::NINE_ONE:
            case TouchZone::NINE_TWO:
            case TouchZone::NINE_THREE:
            case TouchZone::NINE_FOUR:
                zone_name = "8";
                break;
            case TouchZone::TEN_ONE:
            case TouchZone::TEN_TWO:
            case TouchZone::TEN_THREE:
            case TouchZone::TEN_FOUR:
                zone_name = "9";
                break;
            default:
                zone_name = "UNKNOWN";
                break;
            }

            static char selbuf[64];
            snprintf(selbuf, sizeof(selbuf), "SELECT BOOK:%s", zone_name);
            result.message = selbuf;
        }
        break;
#if DBG_UI_CONTROL
        Serial.println("[MAIN_MENU TOUCH] 触摸区域：切换最近/文件来源");
#endif
    case TouchZone::THREE_FIVE:
    case TouchZone::FOUR_FIVE:
#if DBG_UI_CONTROL
#endif
        result.success = true;
        result.panel_clicked = true;
        // FOUR_FOUR 将用于 繁简切换（TOGGLE_ZH_CONV），FOUR_FIVE 保持为最近/按文件名切换
        {
            if (zone == TouchZone::THREE_FIVE)
            {
                result.message = "TOGGLE_ZH_CONV";
            }
            else
            {
                result.message = "TOGGLE_RECENT";
            }
        }
        break;
    case TouchZone::ONE_FIVE:
    case TouchZone::ONE_SIX:
#if DBG_UI_CONTROL
        Serial.println("[MAIN_MENU TOUCH] 触摸区域：上一页");
#endif
        result.success = true;
        result.button_pressed = true;
        result.message = "PREV PAGE";
        break;
    case TouchZone::TWO_FIVE:
    case TouchZone::TWO_SIX:
#if DBG_UI_CONTROL
        Serial.println("[MAIN_MENU TOUCH] 触摸区域：下一页");
#endif
        result.success = true;
        result.button_pressed = true;
        result.message = "NEXT PAGE";
        break;
    case TouchZone::FIVE_FIVE:
    case TouchZone::FIVE_SIX:
#if DBG_UI_CONTROL
        Serial.println("[MAIN_MENU TOUCH] 触摸区域：字体切换");
#endif
        result.success = true;
        result.panel_clicked = true;
        result.message = "FONT TOGGLE";
        break;
    case TouchZone::SIX_FIVE:
    case TouchZone::SIX_SIX:
#if DBG_UI_CONTROL
        Serial.println("[MAIN_MENU TOUCH] 触摸区域：打开书籍");
#endif
        result.success = true;
        result.panel_clicked = true;
        result.message = "OPEN BOOK";
        break;
    case TouchZone::SEVEN_FIVE:
    case TouchZone::SEVEN_SIX:
#if DBG_UI_CONTROL
        Serial.println("[MAIN_MENU TOUCH] 触摸区域：清理书签");
#endif
        result.success = true;
        result.panel_clicked = true;
        result.message = "CLEAN BOOKMARK";
        break;
    case TouchZone::EIGHT_FIVE:
    case TouchZone::EIGHT_SIX:
#if DBG_UI_CONTROL
        Serial.println("[MAIN_MENU TOUCH] 触摸区域：显示设置");
#endif
        result.success = true;
        result.panel_clicked = true;
        result.message = "DISPLAY SETTING";
        break;
    case TouchZone::NINE_FIVE:
    case TouchZone::NINE_SIX:
#if DBG_UI_CONTROL
        Serial.println("[MAIN_MENU TOUCH] 触摸区域：无线连接");
#endif
        result.success = true;
        result.panel_clicked = true;
        result.message = "WIRE CONNECT";
        break;
    case TouchZone::TEN_FIVE:
    case TouchZone::TEN_SIX:
#if DBG_UI_CONTROL
        Serial.println("[MAIN_MENU TOUCH] 触摸区域：返回阅读");
#endif
        result.success = true;
        result.panel_clicked = true;
        result.message = "RETURN READ";
        break;
    default:
#if DBG_UI_CONTROL
        Serial.println("[MAIN_MENU TOUCH] 触摸区域：打印测试");
#endif
        result.success = true;
        result.panel_clicked = true;
        result.message = "主菜单触摸";
        break;
    }
    return result;
}
