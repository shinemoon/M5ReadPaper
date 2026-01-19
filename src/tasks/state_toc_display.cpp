#include "readpaper.h"
#include "state_machine_task.h"
#include "state_debug.h"
#include "test/per_file_debug.h"
#include <cstring>
// bring in orientation constants and display helper
#include "globals.h"
#include "device/ui_display.h"
// for show_reading_menu
#include "ui/ui_canvas_utils.h"
// for TOC loading and display
#include "ui/toc_display.h"
// for tag display
#include "ui/index_display.h"
#include "ui/ui_canvas_image.h"
// for screenshot
#include "ui/screenshot.h"

extern M5Canvas *g_canvas;
extern float font_size;
// Handler for STATE_TOC_DISPLAY
// Similar to STATE_INDEX_DISPLAY but shows .idx file contents instead of tags

void StateMachineTask::handleTocDisplayState(const SystemMessage_t *msg)
{
    if (!msg)
        return;

#if DBG_STATE_MACHINE_TASK
    sm_dbg_printf("STATE_TOC_DISPLAY 收到消息: %d\n", msg->type);
#endif

    switch (msg->type)
    {
    case MSG_TIMER_MIN_TIMEOUT:
        // Keep state active
        break;

    case MSG_BATTERY_STATUS_CHANGED:
    case MSG_CHARGING_STATUS_CHANGED:
        // Battery/charging events - log only
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("STATE_TOC_DISPLAY: 电池事件 voltage=%.2f, pct=%d, charging=%d\n",
                      msg->data.power.voltage, msg->data.power.percentage, msg->data.power.isCharging);
#endif
        break;

    case MSG_TOUCH_PRESSED:
    {
        // Touch handling: left area is the TOC list, right area returns to reading menu
        int16_t tx = msg->data.touch.x;
        int16_t ty = msg->data.touch.y - 32;
        int16_t raw_y = msg->data.touch.y; // preserve raw screen Y for pagination area check

        // Pagination touch area at bottom: y in [920,960]
        // x < 225 => previous page; 240 < x < 450 => next page
        if (raw_y >= 900 && raw_y <= 960) // A bit margin
        {
            if (tx < 225)
            {
                // previous page
                toc_prev_page();
                show_toc_ui(g_canvas);
                return;
            }
            if (tx > 240 && tx < 450)
            {
                // next page
                toc_next_page();
                show_toc_ui(g_canvas);
                return;
            }
        }

        // If touch on the right-side (outside the TOC list), return to reading menu
        const int16_t TOC_AREA_W = 450; // must match show_toc_ui
        if (tx > TOC_AREA_W)
        {
            ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
            g_current_book->renderCurrentPage(font_size);
            currentState_ = STATE_READING;
            return;
        }

        // Check if touch on tab area to switch to tags view
        const int16_t TAB_Y = 60; // take a bit margin
        if (ty < TAB_Y)
        {
            // Touch on tab area - check if touching "书签" tab
            // Tab spans roughly x=270 to x=370 for "书签"
            if (tx > 250 && tx < 380)
            {
                // Switch to index display (tags view)
                show_tag_ui(g_canvas);
                currentState_ = STATE_INDEX_DISPLAY;
                return;
            }
        }

        // Touch is inside TOC list area -> map to row and handle
        const int rows = 10;
        const int total_h = 960;
        const int row_h = total_h * 0.9 / rows; // ~86
        int row = ty / row_h;
        if (row < 0 || row >= rows)
            break; // outside rows

        if (!g_current_book)
            break;

        // Calculate actual TOC entry index considering current page offset
        int current_page = toc_get_current_page();
        int toc_index = current_page * rows + row;

#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("STATE_TOC_DISPLAY: clicked row=%d, current_page=%d, toc_index=%d\n",
                      row, current_page, toc_index);
#endif

        if (toc_index < 0)
            break;

        TocEntry selected_entry;
        if (!fetch_toc_entry(g_current_book->filePath(), toc_index, selected_entry))
            break;

        size_t toc_pos = selected_entry.position;

        // If TOC position is beyond current indexing progress and indexing not complete, ignore click
        if (!g_current_book->isIndexingComplete() && toc_pos > g_current_book->getIndexingCurrentPos())
        {
            // not yet indexed — no response
            break;
        }

        // Map file offset to page index and jump
        size_t page_idx = 0;
        bool mapped = g_current_book->findPageIndexForPosition(toc_pos, page_idx);
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("STATE_TOC_DISPLAY: toc_pos=%zu, indexing_complete=%d, indexing_pos=%zu, mapped=%d, page_idx=%zu\n",
                      toc_pos, g_current_book->isIndexingComplete() ? 1 : 0, g_current_book->getIndexingCurrentPos(), mapped ? 1 : 0, page_idx);
#endif
        // Additional diagnostic: log surrounding page start positions
        if (mapped) {
            size_t prev = (page_idx > 0) ? g_current_book->getPageStart(page_idx - 1) : (size_t)-1;
            size_t cur = g_current_book->getPageStart(page_idx);
            size_t next = g_current_book->getPageStart(page_idx + 1);
    #if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("STATE_TOC_DISPLAY: surrounding page starts: prev_idx=%zu prev_pos=%zu, cur_idx=%zu cur_pos=%zu, next_idx=%zu next_pos=%zu\n",
                  (page_idx>0?page_idx-1: (size_t)-1), prev, page_idx, cur, (page_idx+1), next);
    #endif
        }
        if (mapped)
        {
            bool jumped = g_current_book->jumpToPage(page_idx);
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("STATE_TOC_DISPLAY: jumpToPage(page_idx=%zu) returned %d\n", page_idx, jumped ? 1 : 0);
#endif
            if (jumped)
            {
                ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                g_current_book->renderCurrentPage(font_size);
                currentState_ = STATE_READING;
                return;
            }
        }

        // Fallback: set absolute position and render
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("STATE_TOC_DISPLAY: findPageIndexForPosition failed or jump failed, falling back to setPosition(%zu)\n", toc_pos);
#endif
        ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
        g_current_book->setPosition(toc_pos);
        g_current_book->renderCurrentPage(font_size);
        currentState_ = STATE_READING;
        return;
    }
    break;
    case MSG_TOUCH_RELEASED:
    case MSG_TOUCH_EVENT:
        // Touch events are accepted by this state
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("STATE_TOC_DISPLAY: 触摸事件 (x=%d,y=%d)\n", msg->data.touch.x, msg->data.touch.y);
#endif
        break;

    case MSG_DOUBLE_TOUCH_PRESSED:
        // 检查是否在截图区域
        if (isInScreenshotArea(msg->data.touch.x, msg->data.touch.y))
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("双击截图区域，开始截图\n");
#endif
            if (screenShot())
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("截图成功\n");
#endif
            }
        }
        break;

    case MSG_DEVICE_ORIENTATION:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("STATE_TOC_DISPLAY: 方向变化 %d\n", msg->data.orientation.dir);
#endif
        // Adjust rotation
        if (msg->data.orientation.dir == ORIENT_UP)
        {
            display_set_rotation(2);
        }
        else if (msg->data.orientation.dir == ORIENT_DOWN)
        {
            display_set_rotation(0);
        }
        break;

    case MSG_USER_ACTIVITY:
        // update last activity timestamp (handled by state machine core)
        break;

    default:
        // other messages ignored
        break;
    }
}
