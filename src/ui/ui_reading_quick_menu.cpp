#include "ui_reading_quick_menu.h"
#include "ui/ui_canvas_utils.h"
#include "text/bin_font_print.h"
#include "globals.h"

extern GlobalConfig g_config;

// rectangle dimensions
static const int QUICK_MENU_WIDTH = PAPER_S3_WIDTH; // 540
static const int QUICK_MENU_HEIGHT = 200;// One Auto Reading + One Force Fresh
static const int QUICK_MENU_TOP = PAPER_S3_HEIGHT - QUICK_MENU_HEIGHT; // 960 - 200 = 760

void draw_reading_quick_menu(M5Canvas *canvas)
{
    if (canvas == nullptr)
        return;
    /* 
        自动阅读菜单 
    */
    // 清除并在底部绘制白色矩形
    // 不清整张画布，直接绘制底部矩形
    //    canvas->fillRect(0, QUICK_MENU_TOP, QUICK_MENU_WIDTH, QUICK_MENU_HEIGHT, TFT_WHITE);
    // 可选地在矩形上绘制占位文本
    //    bin_font_print("快速菜单", 28, TFT_BLACK, QUICK_MENU_WIDTH, 200, QUICK_MENU_TOP + 16, false, canvas, TEXT_ALIGN_CENTER);
    canvas->drawRoundRect(49, 889, 492, 62, 10, TFT_WHITE);
    canvas->drawRoundRect(50, 890, 490, 60, 10, TFT_BLACK);
    canvas->fillRoundRect(52, 892, 486, 54, 10, TFT_WHITE);


    canvas->fillRect(52+g_config.autospeed*100, 894, 100, 50, TFT_LIGHTGRAY);

    bin_font_print("I", 30, 0,100,52, 905,true,canvas,TEXT_ALIGN_CENTER,100);
    canvas->fillRect(152, 894, 2, 50, TFT_LIGHTGRAY);
    bin_font_print("II", 30, 0,100,152, 905,true,canvas,TEXT_ALIGN_CENTER,100);
    canvas->fillRect(252, 894, 2, 50, TFT_LIGHTGRAY);
    bin_font_print("III", 30, 0,100,252, 905,true,canvas,TEXT_ALIGN_CENTER,100);
    canvas->fillRect(352, 894, 2, 50, TFT_LIGHTGRAY);
    bin_font_print("IV", 30, 0,100,352, 905,true,canvas,TEXT_ALIGN_CENTER,100);
    canvas->fillRect(452, 894, 2, 50, TFT_LIGHTGRAY);




    canvas->fillArc(540, 960, 0, 80, 180, 270, TFT_WHITE);
    canvas->fillArc(540, 960, 0, 65, 180, 270, TFT_BLACK);
    canvas->drawArc(540, 960, 0, 80, 180, 270, TFT_BLACK);
    if (!autoread)
    {
        canvas->fillTriangle(505, 918, 505, 948, 534, 933, TFT_WHITE);
    }
    else
    {
        canvas->fillRect(505, 920, 10, 30, TFT_WHITE);
        canvas->fillRect(520, 920, 10, 30, TFT_WHITE);
    }
    /* 
        手动全刷ccacaca菜单
    */
     canvas->drawRoundRect(249, 789, 302, 62, 10, TFT_WHITE);
    canvas->drawRoundRect(250, 790, 300, 60, 10, TFT_BLACK);
    canvas->fillRoundRect(252, 792, 298, 54, 10, TFT_BLACK);
//    canvas->fillRect(260, 800, 192, 38, TFT_BLACK);
    bin_font_print("手动全刷", 30, 0,200,252, 805,false,canvas,TEXT_ALIGN_CENTER,200,false,false,false,true);
    canvas->fillRect(452, 794, 2, 50, TFT_LIGHTGRAY);
    drawScrew(canvas,495, 819);

}

bool is_point_in_reading_quick_menu(int16_t x, int16_t y)
{
    return (x >= 0 && x < QUICK_MENU_WIDTH && y >= QUICK_MENU_TOP && y < (QUICK_MENU_TOP + QUICK_MENU_HEIGHT));
}
