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
#include "ui/ui_canvas_image.h"
// for tag loading
#include "text/tags_handle.h"
// for TOC display
#include "ui/toc_display.h"
// for screenshot
#include "ui/screenshot.h"

extern M5Canvas *g_canvas;
extern float font_size;
// Minimal handler for STATE_INDEX_DISPLAY
// The state should accept timer, battery and various touch events.
// Currently we only provide an entry/logging implementation and ignore other events.

void StateMachineTask::handleIndexDisplayState(const SystemMessage_t *msg)
{
    if (!msg)
        return;

#if DBG_STATE_MACHINE_TASK
    sm_dbg_printf("STATE_INDEX_DISPLAY 收到消息: %d\n", msg->type);
#endif

    switch (msg->type)
    {
    case MSG_TIMER_MIN_TIMEOUT:
        // For now, do nothing special on timer. Keep state active.
        break;

    case MSG_BATTERY_STATUS_CHANGED:
    case MSG_CHARGING_STATUS_CHANGED:
        // Battery/charging events received - currently no action other than optional logging.
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("STATE_INDEX_DISPLAY: 电池事件 voltage=%.2f, pct=%d, charging=%d\n",
                      msg->data.power.voltage, msg->data.power.percentage, msg->data.power.isCharging);
#endif
        break;

    case MSG_TOUCH_PRESSED:
    {
        // Touch handling: left area is the tag list, right area returns to reading menu
        int16_t tx = msg->data.touch.x;
        int16_t ty = msg->data.touch.y - 32;

        // If touch on the right-side (outside the tag list), return to reading menu
        const int16_t TAG_AREA_W = 450; // must match show_tag_ui
        if (tx > TAG_AREA_W)
        {
            ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
            g_current_book->renderCurrentPage(font_size);
            currentState_ = STATE_READING;
            return;
        }

        // Check if touch on tab area to switch to TOC view
        const int16_t TAB_Y = 60; // Take a bit margin
        if (ty < TAB_Y)
        {
            // Touch on tab area - check if touching "目录" tab
            // Tab spans roughly x=0 to x=240 for "目录"
            if (tx > 0 && tx < 250 && g_current_book && g_current_book->isIndexed())
            {

                ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
                    // Switch to TOC display: request a refresh and show UI
                    toc_refresh = true;
                    // Switch to TOC display
                    show_toc_ui(g_canvas);
                    currentState_ = STATE_TOC_DISPLAY;
                return;
            }
            else
            { // if no indexed, then just keep no change.
                return;
            }
        }

        // Touch is inside tag list area -> map to row and handle
        const int rows = 10;
        const int total_h = 960;
        const int row_h = total_h * 0.9 / rows; // 96
        int row = ty / row_h;
        if (row < 0 || row >= rows)
            break; // outside rows

        if (!g_current_book)
            break;

        // Load tags for current book (small, safe call)
        std::vector<TagEntry> tags = loadTagsForFile(g_current_book->filePath());
        if ((size_t)row >= tags.size())
            break; // no tag at this row

        size_t tag_pos = tags[row].position;

        // If tag is beyond current indexing progress and indexing not complete, ignore click
        if (!g_current_book->isIndexingComplete() && tag_pos > g_current_book->getIndexingCurrentPos())
        {
            // not yet indexed — no response
            break;
        }

        // Map file offset to page index and jump
        size_t page_idx = 0;
        bool mapped = g_current_book->findPageIndexForPosition(tag_pos, page_idx);
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("STATE_INDEX_DISPLAY: tag_pos=%zu, indexing_complete=%d, indexing_pos=%zu, mapped=%d, page_idx=%zu\n",
                      tag_pos, g_current_book->isIndexingComplete() ? 1 : 0, g_current_book->getIndexingCurrentPos(), mapped ? 1 : 0, page_idx);
#endif
        if (mapped)
        {
            bool jumped = g_current_book->jumpToPage(page_idx);
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("STATE_INDEX_DISPLAY: jumpToPage(page_idx=%zu) returned %d\n", page_idx, jumped ? 1 : 0);
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
        sm_dbg_printf("STATE_INDEX_DISPLAY: findPageIndexForPosition failed or jump failed, falling back to setPosition(%zu)\n", tag_pos);
#endif
        ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
        g_current_book->setPosition(tag_pos);
        g_current_book->renderCurrentPage(font_size);
        currentState_ = STATE_READING;
        return;
    }
    break;
    case MSG_TOUCH_RELEASED:
    case MSG_TOUCH_EVENT:
        // Touch events are accepted by this state
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("STATE_INDEX_DISPLAY: 触摸事件 (x=%d,y=%d)\n", msg->data.touch.x, msg->data.touch.y);
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
        sm_dbg_printf("STATE_INDEX_DISPLAY: 方向变化 %d\n", msg->data.orientation.dir);
#endif
        // Optionally adjust rotation (defer actual UI updates until implementation)
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
        // update last activity timestamp if needed (handled by state machine core)
        break;

    default:
        // other messages ignored for now
        break;
    }
}
