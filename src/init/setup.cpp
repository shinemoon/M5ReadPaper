#include <M5Unified.h>
#include <SPIFFS.h>
#include "SD/SDWrapper.h"
#include "readpaper.h"
#include "papers3.h"
#include "device/file_manager.h"
#include "device/ui_display.h"
#include "device/powermgt.h"
#include "device/wifi_hotspot_manager.h"
#include "test/test_functions.h"
#include "init/setup.h"
#include "test/per_file_debug.h"
#include "tasks/display_push_task.h"
#include "ui/ui_canvas_image.h"
#include "ui/ui_lock_screen.h"
#include "config/config_manager.h"
#include "text/book_handle.h"
#include "tasks/background_index_task.h"
#include "esp_sleep.h"

extern M5Canvas *g_canvas;
extern GlobalConfig g_config;
#include "current_book.h"
#include "globals.h"
#include <cmath>

// Print boot time marker and elapsed since last marker
static unsigned long setup_start_time = 0;
void printBootTime(const char *label)
{
    unsigned long now = millis();
    if (setup_start_time == 0)
        setup_start_time = now;
#if DBG_SETUP
    Serial.printf("[BOOT] %s: %lu ms (cum=%lu)\n", label, now - setup_start_time, now);
#endif
    setup_start_time = now;
}

void setup()
{
    setup_start_time = millis();

    // 1. Serial init
    Serial.begin(115200);
    delay(100);
    Serial.println("========================================");
    Serial.println("[SETUP] ===== 系统重启 =====");
    Serial.printf("[SETUP] 启动时间: %lu ms\n", millis());
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    Serial.printf("[SETUP] 唤醒原因: %d\n", (int)wake_cause);
    Serial.println("========================================");
    printBootTime("Serial initialized");

    // 2. M5 init
    auto cfg = M5.config();
    cfg.clear_display = false;
    M5.begin(cfg);
    // 设置屏幕旋转（上下翻转） initDisplay中已经做了
    //    M5.Display.setRotation(g_config.rotation);
    // 启用陀螺仪/惯性传感器，读取设备朝向到全局变量
    // 之前为了省电调用了 M5.Imu.sleep()，现在改为唤醒并检测方向
    // 尝试初始化 IMU（多数 M5Unified 版本提供 begin()）
    M5.Imu.begin();
    delay(50);
    // 读取加速度以判断方向（以重力向量为准），保守判定
    {
        float ax = 0, ay = 0, az = 0;
        // getAccelData 接口在 M5Unified 中存在；容错：如果不可用，编译会报错
        M5.Imu.getAccelData(&ax, &ay, &az);
        // 简单阈值判定：以 X/Y 轴分量绝对值比较为横竖屏判定
        int detected = ORIENT_UNKNOWN;
        if (fabsf(ax) > fabsf(ay))
        {
            // X 轴主导 -> 横向（左/右）
            detected = (ax > 0.0f) ? ORIENT_RIGHT : ORIENT_LEFT;
        }
        else
        {
            // Y 轴主导 -> 竖向（上/下）
            detected = (ay > 0.0f) ? ORIENT_DOWN : ORIENT_UP;
        }
#if 1
        // 按照与 DeviceInterruptTask 相同的映射规则保持一致：
        // LEFT->UP, UP->RIGHT, RIGHT->DOWN, DOWN->LEFT
        int mapped = ORIENT_UNKNOWN;
        switch (detected)
        {
        case ORIENT_LEFT:
            mapped = ORIENT_UP;
            break;
        case ORIENT_UP:
            mapped = ORIENT_RIGHT;
            break;
        case ORIENT_RIGHT:
            mapped = ORIENT_DOWN;
            break;
        case ORIENT_DOWN:
            mapped = ORIENT_LEFT;
            break;
        default:
            mapped = ORIENT_UNKNOWN;
            break;
        }
        g_device_orientation = mapped;
#else
        g_device_orientation = detected;
#endif
#if DBG_SETUP
        Serial.printf("[SETUP] IMU accel read: ax=%.3f ay=%.3f az=%.3f -> orientation=%d\n", ax, ay, az, g_device_orientation);
#endif
    }

    // 确保触摸屏被启用
    if (M5.Touch.isEnabled())
    {
#if DBG_SETUP
        Serial.println("[SETUP] 触摸屏已启用");
#endif
        // 设置触摸中断为唤醒源（低电平触发）
        gpio_wakeup_enable((gpio_num_t)GPIO_NUM_48, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();
    }
    else
    {
#if DBG_SETUP
        Serial.println("[SETUP] 警告：触摸屏未启用");
#endif
    }

#if DBG_SETUP
    Serial.printf("[SETUP] M5.begin() done. PSRAM: %u bytes, Free: %u bytes\n", ESP.getPsramSize(), ESP.getFreePsram());
#endif
    printBootTime("M5 initialized");

    // 4. filesystem init
    if (init_filesystem())
    {
        printBootTime("文件系统初始化完成");
    }
    else
    {
        printBootTime("文件系统初始化失败");
    }

    // 3. 配置管理器初始化
#if DBG_SETUP
    Serial.println("[SETUP] ===== 准备初始化配置管理器 =====");
#endif
    bool config_loaded_successfully = false;
    if (config_init())
    {
        printBootTime("配置管理器初始化完成");
        // 应用加载的配置 to initDisplay
        M5.Display.setRotation(g_config.rotation);
        config_loaded_successfully = true;
#if DBG_SETUP
        Serial.printf("[SETUP] ✅ 配置管理器初始化成功，当前书籍: %s\n", g_config.currentReadFile);
#endif
    }
    else
    {
        printBootTime("配置管理器初始化失败");
        config_loaded_successfully = false;
#if DBG_SETUP
        Serial.println("[SETUP] ❌ 配置管理器初始化失败，使用默认配置");
#endif
    }

    // 4. Create global canvas

    g_canvas = new M5Canvas(&M5.Display);
    g_canvas->createSprite(PAPER_S3_WIDTH, PAPER_S3_HEIGHT);

    // 初始化显示推送任务，使得 bin_font_flush_canvas 在 setup 阶段也能成功入队
    initializeDisplayPushTask();
    //   bin_font_clear_canvas();
    //    bin_font_flush_canvas(false,false,true);
    // 4.5 Screen push: 推迟显示启动画面到显示和字体初始化之后，
    // 防止启动画面在字体索引尚未构建时触发对字体的访问，
    // 这可能导致 PROGMEM 字体在上电初始化时出现渲染乱码的问题。

    // 关键修复：确保 PSRAM 完全就绪
    // 在启动早期，PSRAM 可能需要一定时间稳定，过早访问可能导致数据损坏
#if DBG_SETUP
    Serial.println("[SETUP] 等待 PSRAM 稳定...");
#endif
    delay(100); // 给 PSRAM 100ms 的稳定时间
    // 执行一次 PSRAM 写入测试，确保其工作正常
    if (ESP.getPsramSize() > 0)
    {
        void *test_ptr = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
        if (test_ptr)
        {
            memset(test_ptr, 0xAA, 1024);
            heap_caps_free(test_ptr);
#if DBG_SETUP
            Serial.println("[SETUP] PSRAM 稳定性测试通过");
#endif
        }
    }

    // 5. init display and font
    initDisplay();
    printBootTime("显示系统初始化完成");

    // 在显示与字体初始化完成后再显示启动画面
    M5.Display.waitDisplay();
    // 唤醒则跳过开始画面
    if (int(wake_cause) == 0)
        show_start_screen("");

#if DBG_SETUP
    Serial.printf("[BOOT] 总启动时间: %lu ms\n", millis() - setup_start_time);
    Serial.println("[READY] M5Stack Paper S3 Ready!");
#endif

// ===== Canvas UI 演示 =====
//    test::print_sample_pages();

// ===== 根据配置文件初始化 BookHandle =====
// 打印当前读取文件配置
#if DBG_SETUP
    Serial.println("[SETUP] ===== 开始加载书籍 =====");
    Serial.printf("[SETUP] 当前配置的读取文件: '%s'\n", g_config.currentReadFile);
    Serial.printf("[SETUP] 当前配置文件长度: %d\n", (int)strlen(g_config.currentReadFile));
#endif
    if (strlen(g_config.currentReadFile) == 0)
    {
#if DBG_SETUP
        Serial.println("[SETUP] ⚠️ currentReadFile 为空，使用默认文件");
#endif
        strcpy(g_config.currentReadFile, "/spiffs/ReadPaper.txt"); // backward comp purpose
    }
    // 获取显示区域参数
    int16_t area_w = PAPER_S3_WIDTH - MARGIN_LEFT - MARGIN_RIGHT;
    int16_t area_h = PAPER_S3_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM;
    float fsize = (float)get_font_size_from_file();

#if DBG_SETUP
    Serial.printf("[SETUP] 从配置加载书籍: %s\n", g_config.currentReadFile);
#endif

    // 检查文件是否存在
    bool file_exists = false;
    const char *real_file_path = nullptr;

    if (strncmp(g_config.currentReadFile, "/sd/", 4) == 0)
    {
        real_file_path = g_config.currentReadFile + 3; // 跳过"/sd/"前缀
        file_exists = SDW::SD.exists(real_file_path);
#if DBG_SETUP
        Serial.printf("[SETUP] 检查SD卡文件: %s -> %s\n", g_config.currentReadFile, real_file_path);
#endif
    }
    else if (strncmp(g_config.currentReadFile, "/spiffs/", 8) == 0)
    {
        real_file_path = g_config.currentReadFile + 7; // 跳过"/spiffs/"前缀
        file_exists = SPIFFS.exists(real_file_path);
#if DBG_SETUP
        Serial.printf("[SETUP] 检查SPIFFS文件: %s -> %s\n", g_config.currentReadFile, real_file_path);
#endif
    }

    if (file_exists)
    {
        // 如果已有实例，先清理
        // 创建新的BookHandle实例并 atomically publish it
        {
            std::shared_ptr<BookHandle> new_book = std::make_shared<BookHandle>(std::string(g_config.currentReadFile),
                                                                                area_w, area_h, fsize, TextEncoding::AUTO_DETECT);

            // atomically replace global current book
            std::atomic_store(&__g_current_book_shared, new_book);
            // reset autoread when a new book is opened
            autoread = false;

            // 检查BookHandle是否成功创建并打开
            if (new_book != nullptr && new_book->isOpen())
            {
#if DBG_SETUP
                Serial.printf("[SETUP] BookHandle 创建成功，文件: %s\n", g_config.currentReadFile);
#endif
                // 只有在配置成功加载的情况下才保存（避免覆盖正确配置）
                // 如果配置加载失败，说明是使用默认值，此时不应该保存
                if (config_loaded_successfully)
                {
#if DBG_SETUP
                    Serial.println("[SETUP] 配置已成功加载，确认书籍可打开，保存配置");
#endif
                    config_save();
                }
                else
                {
#if DBG_SETUP
                    Serial.println("[SETUP] ⚠️ 配置加载失败，跳过保存以避免覆盖有效配置文件");
#endif
                }
#if DBG_BOOK_HANDLE
                Serial.println("[SETUP] 创建新 BookHandle 成功");
#endif
            }
            else
            {
#if DBG_SETUP
                Serial.printf("[SETUP] BookHandle 创建失败或打开失败，文件: %s\n", g_config.currentReadFile);
#endif
                // 清理失败的实例 by resetting shared_ptr (will delete if no other refs)
                std::atomic_store(&__g_current_book_shared, std::shared_ptr<BookHandle>(nullptr));
                file_exists = false; // 标记为失败，触发回退逻辑
            }
        } // 闭合 std::shared_ptr 作用域块
    } // 闭合 if (file_exists) 块

    if (!file_exists)
    {
        // 文件不存在或打开失败，回退到默认文件
#if DBG_SETUP
        if (strlen(g_config.currentReadFile) > 0)
        {
            Serial.printf("[SETUP] 文件不存在或打开失败: %s，回退到默认文件\n", g_config.currentReadFile);
        }
        else
        {
            Serial.printf("[SETUP] 使用默认文件\n");
        }
#endif

        // 设置默认文件路径
        strcpy(g_config.currentReadFile, "/spiffs/ReadPaper.txt");

        if (SPIFFS.exists("/ReadPaper.txt"))
        {
            // Replace global book with default file's BookHandle
            std::shared_ptr<BookHandle> new_book = std::make_shared<BookHandle>(std::string(g_config.currentReadFile),
                                                                                area_w, area_h, fsize, TextEncoding::AUTO_DETECT);
            std::atomic_store(&__g_current_book_shared, new_book);
            // reset autoread when a new book is opened
            autoread = false;

            if (new_book != nullptr && new_book->isOpen())
            {
#if DBG_SETUP
                Serial.printf("[SETUP] 默认文件BookHandle创建成功: %s\n", g_config.currentReadFile);
#endif
                // 只有在配置加载失败时才保存默认配置
                // 如果配置加载成功但书籍文件不存在，不应该保存默认文件路径
                // （用户可能暂时取出了 SD 卡，不应该覆盖配置）
                if (!config_loaded_successfully)
                {
#if DBG_SETUP
                    Serial.println("[SETUP] 配置加载失败，保存默认配置");
#endif
                    config_save();
                }
                else
                {
#if DBG_SETUP
                    Serial.println("[SETUP] ⚠️ 配置文件存在但书籍文件丢失，不保存默认配置");
                    Serial.println("[SETUP] （用户可能暂时取出了SD卡）");
#endif
                }
            }
            else
            {
#if DBG_SETUP
                Serial.printf("[SETUP] 警告：默认文件也无法打开: %s\n", g_config.currentReadFile);
#endif
                // 清理失败的实例: publish nullptr
                std::atomic_store(&__g_current_book_shared, std::shared_ptr<BookHandle>(nullptr));
                // 清空配置
                g_config.currentReadFile[0] = '\0';
                // 同样只有配置加载失败时才保存
                if (!config_loaded_successfully)
                {
                    config_save();
                }
            }
        }
        else
        {
#if DBG_SETUP
            Serial.printf("[SETUP] 警告：默认文件不存在: /spiffs/ReadPaper.txt\n");
#endif
            // 清空配置，没有可用的文件
            // 但不保存！因为这可能只是 SPIFFS 没有挂载或文件被删除
            // 保留 SD 卡上的配置文件
            std::atomic_store(&__g_current_book_shared, std::shared_ptr<BookHandle>(nullptr));
#if DBG_SETUP
            Serial.println("[SETUP] ⚠️ 默认文件不存在，保持当前配置不变");
#endif
        }
    }

    printBootTime("BookHandle 初始化完成");

    // WiFi热点管理器将在需要时初始化（按需加载）
    // wifi_hotspot_init(); // 移到使用时再初始化

    // update idle time marker
    // Note: idleTime is global in main.cpp; we'll set it via extern if needed
    extern unsigned long idleTime;
    idleTime = millis();
#if DBG_SETUP
    Serial.printf("[DEBUG] setup() 即将结束，耗时: %lu ms\n", millis() - setup_start_time);
#endif
}
