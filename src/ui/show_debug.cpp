#include "readpaper.h"
#include "show_debug.h"
#include "ui/ui_canvas_utils.h"
#include "device/internal_fs.h"
#include "SD/SDWrapper.h"
#include "ui/ui_canvas_utils.h"
#include <string>
#include "tasks/display_push_task.h"

extern M5Canvas *g_canvas;

// Layout constants
static const int16_t kWidth = PAPER_S3_WIDTH;
static const int16_t kHeight = PAPER_S3_HEIGHT;

// Buttons: vertical stack in upper half, centered horizontally
static void compute_button_layout(int index, int16_t &cx, int16_t &cy, int16_t &w, int16_t &h)
{
    int16_t upper_h = kHeight / 2;
    w = 164;
    h = 54;
    cx = kWidth / 2 - w/2;
    // spacing
    int16_t spacing = 18;
    // compute top of stack so entire stack vertically centered within upper_h
    int total_h = 3 * h + 2 * spacing;
    int16_t top = (upper_h - total_h) / 2;
    cy = top + index * (h + spacing);
}

void debug_button_bounds(int index, int16_t &cx, int16_t &cy, int16_t &w, int16_t &h)
{
    compute_button_layout(index, cx, cy, w, h);
}

bool debug_button_hit(int index, int16_t tx, int16_t ty)
{
    int16_t cx, cy, w, h;
    compute_button_layout(index, cx, cy, w, h);
    int16_t left = cx - w/2;
    int16_t top = cy - h/2;
    int16_t right = cx + w/2;
    int16_t bottom = cy + h/2;
//    Serial.printf("debug_button_hit idx=%d cx=%d cy=%d left=%d top=%d right=%d bottom=%d\n",
 //                 index, cx, cy, left, top, right, bottom);
    return (tx >= left && tx <= right && ty >= top && ty <= bottom);
}

bool show_debug(M5Canvas *canvas, bool refresh)
{
    if (!canvas)
        canvas = g_canvas;
    if (!canvas)
        return false;

    // clear canvas
    canvas->fillRect(0, 0, kWidth, kHeight, TFT_WHITE);

    // draw three buttons
    const char *labels[3] = {"A", "B", "C"};
    for (int i = 0; i < 3; ++i)
    {
        int16_t cx, cy, w, h;
        compute_button_layout(i, cx, cy, w, h);
        // draw_button draws centered at (cx,cy)
        draw_button(canvas, cx, cy, labels[i], false, false);
    }

    // leave lower half empty for future content

    if (refresh)
    {
        // enqueue a display push to flush the canvas
        {
            DisplayPushMessage m = {{false, false, false}};
            enqueueDisplayPush(m);
        }
    }
    return true;
}
