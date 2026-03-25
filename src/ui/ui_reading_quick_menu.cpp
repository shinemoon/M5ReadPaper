#include "ui_reading_quick_menu.h"
#include "ui/ui_canvas_utils.h"
#include "text/bin_font_print.h"
#include "globals.h"

extern GlobalConfig g_config;

// rectangle dimensions
static const int QUICK_MENU_WIDTH = PAPER_S3_WIDTH; // 540
static const int QUICK_MENU_HEIGHT = 336; // 字体比例 + 联线待机 + 手动全刷 + Auto Reading
static const int QUICK_MENU_TOP = PAPER_S3_HEIGHT - QUICK_MENU_HEIGHT; // 960 - 336 = 624

void draw_reading_quick_menu(M5Canvas *canvas, uint8_t preview_font_scale_pct, bool has_pending_scale_change)
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
    canvas->fillRoundRect(50, 890, 490, 60, 10, TFT_BLACK);
    canvas->fillRoundRect(52, 892, 486, 54, 10, TFT_WHITE);


    canvas->fillRect(52+g_config.autospeed*100, 894, 100, 50, TFT_LIGHTGRAY);

    bin_font_print("I", 30, 0,100,37, 905,true,canvas,TEXT_ALIGN_CENTER,100);
    canvas->fillRect(152, 894, 2, 50, TFT_LIGHTGRAY);
    bin_font_print("II", 30, 0,100,137, 905,true,canvas,TEXT_ALIGN_CENTER,100);
    canvas->fillRect(252, 894, 2, 50, TFT_LIGHTGRAY);
    bin_font_print("III", 30, 0,100,237, 905,true,canvas,TEXT_ALIGN_CENTER,100);
    canvas->fillRect(352, 894, 2, 50, TFT_LIGHTGRAY);
    bin_font_print("IV", 30, 0,100,337, 905,true,canvas,TEXT_ALIGN_CENTER,100);
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
     canvas->drawRoundRect(247, 788, 304, 63, 10, TFT_WHITE);
    canvas->drawRoundRect(251, 789, 300, 61, 10, TFT_BLACK);
    canvas->fillRoundRect(252, 792, 298, 54, 10, TFT_BLACK);
//    canvas->fillRect(260, 800, 192, 38, TFT_BLACK);
    bin_font_print("手动全刷", 30, 0,200,237, 805,false,canvas,TEXT_ALIGN_CENTER,200,false,false,false,true);
    canvas->fillRect(452, 794, 2, 50, TFT_LIGHTGRAY);
    drawScrew(canvas,495, 819);

    /*
        字体比例菜单（500宽，5分段：标题 | - | 当前值 | + | 螺丝）- 全黑底白字
    */
    const int16_t scale_x = 51;
    const int16_t scale_y = 624;
    const int16_t scale_w = 500;
    const int16_t scale_h = 63;

    // === 字体比例预览字符：在scale bar正上方居中显示对应档位的汉字 ===
    // 15档比例（80%~150%）对应"我知这世界如露水般短暂然而然而"，字号按对应比例渲染
    {
        static const uint8_t sc_vals[15] = {80, 85, 90, 95, 100, 105, 110, 115, 120, 125, 130, 135, 140, 145, 150};
        static const char* const prev_chars[15] = {"我","知","这","世","界","如","露","水","般","短","暂","然","而","然","而"};
        int idx = 4; // 默认100%
        for (int i = 0; i < 15; ++i) {
            if (preview_font_scale_pct == sc_vals[i]) { idx = i; break; }
        }
        uint8_t bfont = get_font_size_from_file();
        if (bfont == 0) bfont = SYSFONTSIZE;
        uint8_t cfsize = (uint8_t)((float)bfont * sc_vals[idx] / 100.0f + 0.5f);
        if (cfsize < 8) cfsize = 8;
        bool dark = g_config.dark;
        // 清空预览区域（scale bar顶部向上72像素，匹配最大字号48px×1.5=72px的方形区域）
        canvas->fillRect(0, scale_y - 72, PAPER_S3_WIDTH, 72, dark ? TFT_BLACK : TFT_WHITE);
        // 居中绘制单个汉字，底部距banner顶留6px间隙
        int16_t cy = scale_y - (int16_t)cfsize - 6;
        bin_font_print(prev_chars[idx], cfsize, 0,
                       PAPER_S3_WIDTH, 0, cy,
                       false, canvas, TEXT_ALIGN_CENTER, PAPER_S3_WIDTH,
                       false, false, false, dark);
    }

    canvas->drawRoundRect(scale_x - 1, scale_y, scale_w + 2, scale_h, 10, TFT_WHITE);
    canvas->drawRoundRect(scale_x, scale_y + 1, scale_w, scale_h - 2, 10, TFT_BLACK);
    canvas->fillRoundRect(scale_x + 2, scale_y + 4, scale_w - 4, scale_h - 8, 10, TFT_BLACK);

    const int16_t seg_w_title = 190;
    const int16_t seg_w_minus = 70;
    const int16_t seg_w_value = 110;
    const int16_t seg_w_plus = 70;
    const int16_t seg_w_screw = 60;

    const int16_t x_title = scale_x + 2;
    const int16_t x_minus = x_title + seg_w_title;
    const int16_t x_value = x_minus + seg_w_minus;
    const int16_t x_plus = x_value + seg_w_value;
    const int16_t x_screw = x_plus + seg_w_plus;

    const int16_t inner_y = scale_y + 6;
    const int16_t inner_h = scale_h - 12;

    (void)has_pending_scale_change;

    canvas->fillRect(x_minus, inner_y + 2, 2, inner_h - 4, TFT_LIGHTGRAY);
    canvas->fillRect(x_value, inner_y + 2, 2, inner_h - 4, TFT_LIGHTGRAY);
    canvas->fillRect(x_plus, inner_y + 2, 2, inner_h - 4, TFT_LIGHTGRAY);
    canvas->fillRect(x_screw, inner_y + 2, 2, inner_h - 4, TFT_LIGHTGRAY);

    bin_font_print("字体比例", 30, 0, seg_w_title, x_title - 15, scale_y + 18, false, canvas, TEXT_ALIGN_CENTER, seg_w_title, false, false, false, true);

    bin_font_print("-", 30, 0, seg_w_minus, x_minus - 15, scale_y + 15, true, canvas, TEXT_ALIGN_CENTER, seg_w_minus, false, false, false, true);

    char scale_buf[8];
    snprintf(scale_buf, sizeof(scale_buf), "%u%%", (unsigned)preview_font_scale_pct);
    bin_font_print(scale_buf, 24, 0, seg_w_value, x_value - 12, scale_y + 19, true, canvas, TEXT_ALIGN_CENTER, seg_w_value, false, false, false, true);

    bin_font_print("+", 30, 0, seg_w_plus, x_plus - 15, scale_y + 15, true, canvas, TEXT_ALIGN_CENTER, seg_w_plus, false, false, false, true);

    drawScrew(canvas, x_screw + seg_w_screw / 2, scale_y + scale_h / 2);

    /*
        联线待机菜单（在手动全刷上方，下边氤20像素）
    */
    canvas->drawRoundRect(247, 706, 304, 63, 10, TFT_WHITE);
    canvas->fillRoundRect(251, 707, 300, 61, 10, TFT_BLACK);
    canvas->fillRoundRect(252, 710, 298, 54, 10, TFT_WHITE);
    bin_font_print("联线待机", 30, 0, 200, 237, 723, false, canvas, TEXT_ALIGN_CENTER, 200, false, false, false);
    canvas->fillRect(452, 712, 2, 50, TFT_LIGHTGRAY);
    drawScrew(canvas, 495, 737);

}

bool is_point_in_reading_quick_menu(int16_t x, int16_t y)
{
    return (x >= 0 && x < QUICK_MENU_WIDTH && y >= QUICK_MENU_TOP && y < (QUICK_MENU_TOP + QUICK_MENU_HEIGHT));
}
