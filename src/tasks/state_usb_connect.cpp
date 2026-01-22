#include "state_machine_task.h"
#include "state_debug.h"
#include "readpaper.h"
#include "ui/ui_canvas_utils.h"
#include "ui/ui_canvas_image.h"
#include "device/ui_display.h"
#include "test/per_file_debug.h"
#include <cstring>
// for screenshot
#include "ui/screenshot.h"

extern M5Canvas *g_canvas;
#include "globals.h"

void StateMachineTask::handleUsbConnectState(const SystemMessage_t *msg)
{
#if DBG_STATE_MACHINE_TASK
    sm_dbg_printf("USB_CONNECT 状态处理消息: %d\n", msg->type);
#endif

    switch (msg->type)
    {
    case MSG_TOUCH_PRESSED:
    {
        // Detect press on reset-exit button area roughly at center bottom
        int16_t x = msg->data.touch.x;
        int16_t y = msg->data.touch.y;
        int16_t btn_cx = 270;
        int16_t btn_cy = 720;
        int16_t w = 164;
        int16_t h = 54;
        if (x >= btn_cx - w / 2 && x <= btn_cx + w / 2 && y >= btn_cy - 16 && y <= btn_cy - 16 + h)
        {
            // Perform a restart to exit USB MSC mode
            // Give a brief feedback
            ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
            M5.Display.waitDisplay();
            // Use ESP.restart() to reboot
            ESP.restart();
        }
        break;
    }
    case MSG_DEVICE_ORIENTATION:
        /*
        if (msg->data.orientation.dir == ORIENT_UP)
        {
            display_set_rotation(2);
        }
        else if (msg->data.orientation.dir == ORIENT_DOWN)
        {
            display_set_rotation(0);
        }
        show_usb_connect(g_canvas, true);
        break;
    */

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

    case MSG_TIMER_MIN_TIMEOUT:
    default:
        break;
    }
}
