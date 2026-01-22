/**
 * @brief 处理处于无线连接状态 (STATE_WIRE_CONNECT) 时的系统消息分发与响应。
 *
 * 该函数根据传入的 SystemMessage_t 消息类型执行对应的无线连接状态逻辑：
 *  - 计时器超时 (MSG_TIMER_MIN_TIMEOUT)
 *    - 处理超时逻辑，显示锁屏并切换到 STATE_IDLE
 *
 *  - 触摸按下 (MSG_TOUCH_PRESSED)
 *    - 处理返回按钮点击
 *    - 暂时只处理返回功能
 *
 *  - 充电相关事件
 *    - 响应充电状态变化
 *
 * @param msg 指向 SystemMessage_t 的指针，表示接收到的系统消息（应为非空且格式合法）
 * @return 无
 */
#include "readpaper.h"
#include "state_machine_task.h"
#include "../ui/ui_canvas_utils.h"
#include "state_debug.h"
#include "text/bin_font_print.h"
#include "device/ui_display.h"
#include "device/wifi_hotspot_manager.h"
#include "ui/ui_control.h"
#include "ui/ui_lock_screen.h"
#include "test/per_file_debug.h"
#include <cstring>
#include "globals.h"
#include "text/tags_handle.h"
// for screenshot
#include "ui/screenshot.h"

extern M5Canvas *g_canvas;

void StateMachineTask::handleWireConnectState(const SystemMessage_t *msg)
{
#if DBG_STATE_MACHINE_TASK
    sm_dbg_printf("无线连接状态处理消息: %d\n", msg->type);
#endif

    // 在处理消息前让出CPU控制权，防止watchdog超时
    yield();

    // 处理Web服务器客户端请求 - 增加更多保护措施
    if (g_wifi_hotspot && g_wifi_hotspot->isRunning())
    {
        // 检查内存状态
        size_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < 32768)
        { // 提高内存阈值到32KB
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("内存不足 (%u bytes)，跳过Web服务器处理\n", freeHeap);
#endif
            // 内存不足时进行垃圾收集
            yield();
            delay(10);
        }
        else
        {
            // 检查是否正在上传文件，如果是则完全跳过处理
            bool isUploading = g_wifi_hotspot->isUploadInProgress();

            if (isUploading)
            {
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("文件上传进行中，完全跳过Web服务器处理以避免LWIP冲突\n");
#endif
                // 上传过程中不处理任何Web请求
            }
            else
            {
                // 非上传状态下正常处理，但添加更多保护
                static unsigned long lastClientHandle = 0;
                if (millis() - lastClientHandle > 200)
                { // 降低处理频率到每200ms一次
                    lastClientHandle = millis();
                    g_wifi_hotspot->handleClient();
                    yield();
                }
            }
        }
    }

    // 无线连接状态处理逻辑
    switch (msg->type)
    {
    case MSG_TIMER_MIN_TIMEOUT: // 超时进入idle
    {
        // 如果正在上传文件，暂时跳过超时处理以避免与网络处理冲突
        if (g_wifi_hotspot && g_wifi_hotspot->isUploadInProgress())
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("文件上传进行中，跳过超时处理以避免网络冲突\n");
#endif
            shutCnt = 0; // 重置计数器
            break;       // 跳过超时处理
        }

        // 使用局部变量保护，避免直接访问可能有问题的全局状态
        int localShutCnt = shutCnt + 1;

#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("超时处理: count=%d, 内存=%d\n", localShutCnt, ESP.getFreeHeap());
#endif

        // 内存不足时立即进入IDLE
        if (ESP.getFreeHeap() < 16384 || localShutCnt >= READING_IDLE_WAIT_MIN)
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("进入IDLE状态\n");
#endif
            shutCnt = 0;

            // 安全停止WiFi热点
            if (g_wifi_hotspot)
            {
                g_wifi_hotspot->stop();
            }

            // Before entering IDLE, save an automatic tag for current book (slot0)
            if (g_current_book)
            {
                TextPageResult tp = g_current_book->currentPage();
                if (tp.success)
                {
                    insertAutoTagForFile(g_current_book->filePath(), tp.file_pos);
                    g_current_book->refreshTagsCache();
                }
            }
            // 设置状态为IDLE
            currentState_ = STATE_IDLE;

            // 只有在内存充足时才显示锁屏
            if (ESP.getFreeHeap() > 16384)
            {
                show_lockscreen(PAPER_S3_WIDTH, PAPER_S3_HEIGHT, 30, "双击屏幕解锁");
            }
        }
        else
        {
            shutCnt = localShutCnt;
        }
    }
    break;

    case MSG_TOUCH_PRESSED:
    {
        shutCnt = 0;
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("无线连接状态收到触摸: (%d, %d)\n", msg->data.touch.x, msg->data.touch.y);
#endif
        lastActivityTime_ = millis();

        TouchZone zone = getTouchZoneGrid(msg->data.touch.x, msg->data.touch.y);

        // 检查是否点击了返回按钮区域 (NINE_THREE 和 NINE_FOUR)
        if (zone == TouchZone::NINE_THREE || zone == TouchZone::NINE_FOUR)
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("无线连接状态收到返回按钮点击\n");
#endif
            // 停止WiFi热点
            if (g_wifi_hotspot)
            {
                g_wifi_hotspot->stop();
#if DBG_STATE_MACHINE_TASK
                sm_dbg_printf("WiFi热点已停止\n");
#endif
            }

            // 返回到主菜单（强制重扫文件列表，确保新上传或删除的文件能立即生效）
            show_main_menu(g_canvas, true, 0, 0, true);
            currentState_ = STATE_MAIN_MENU;
        }
        // 检查是否点击了重试按钮区域（中间位置，大约在FIVE_THREE和FIVE_FOUR区域）
        else if (zone == TouchZone::FIVE_THREE || zone == TouchZone::FIVE_FOUR)
        {
            /*
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("无线连接状态收到重试按钮点击\n");
#endif
            // 尝试重新启动WiFi热点
            if (g_wifi_hotspot)
            {
                g_wifi_hotspot->stop();
                delay(1000);

                if (g_wifi_hotspot->start())
                {
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("WiFi热点重试启动成功\n");
#endif
                }
                else
                {
#if DBG_STATE_MACHINE_TASK
                    sm_dbg_printf("WiFi热点重试启动失败\n");
#endif
                }

                // 刷新界面显示
                show_wire_connect(g_canvas, true);
            }
                */
        }
        else
        {
#if DBG_STATE_MACHINE_TASK
            sm_dbg_printf("无线连接状态收到其他区域触摸，暂不处理\n");
#endif
        }
        break;
    }

    case MSG_TOUCH_RELEASED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("无线连接状态收到触摸释放\n");
#endif
        break;

    case MSG_USER_ACTIVITY:
        lastActivityTime_ = millis();
        break;

    case MSG_BATTERY_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("无线连接状态收到电池状态变化: %.2fV, %d%%\n", msg->data.power.voltage, msg->data.power.percentage);
#endif
        break;

    case MSG_CHARGING_STATUS_CHANGED:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("无线连接状态收到充电状态变化: %s, %.2fV, %d%%\n",
                      msg->data.power.isCharging ? "开始充电" : "停止充电",
                      msg->data.power.voltage, msg->data.power.percentage);
#endif
        break;

    case MSG_POWER_EVENT:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("无线连接状态收到电源事件\n");
#endif
        break;

    case MSG_DEVICE_ORIENTATION:
#if DBG_STATE_MACHINE_TASK
        sm_dbg_printf("WIRE_CONNECT 收到方向事件: %s\n", DeviceOrientationToString(msg->data.orientation.dir));
#endif
        if (msg->data.orientation.dir == ORIENT_UP)
        {
            display_set_rotation(2);
        }
        else if (msg->data.orientation.dir == ORIENT_DOWN)
        {
            display_set_rotation(0);
        }
        show_wire_connect(g_canvas, true);
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

    default:
#if DBG_STATE_MACHINE_TASK
        Serial.printf("[STATE_MACHINE] 无线连接状态收到消息: %d\n", msg->type);
#endif
        break;
    }
}