#pragma once
#include <M5Unified.h>
// 是否启用自动旋转（基于 IMU / 陀螺仪）
// 设为 1 启用自动旋转并初始化 IMU；设为 0 则完全不初始化或读取 IMU
#ifndef ENABLE_AUTO_ROTATION
#define ENABLE_AUTO_ROTATION 1
#endif
// 优化：使用条件编译而非运行时判断
//#define DEBUGON

// 显示设置

#define QUALITY_REFRESH epd_mode_t::epd_quality
#define MIDDLE_REFRESH epd_mode_t::epd_fast
//#define NORMAL_REFRESH epd_mode_t::epd_quality //调试需要
#define NORMAL_REFRESH epd_mode_t::epd_text
#define LOW_REFRESH epd_mode_t::epd_fastest

#define FIRST_REFRESH_TH 8
#define SECOND_REFRESH_TH 40


// State Machine (int8)
#define IDLE 0

// Format FONT (need to align with real font)
#define SYSFONTSIZE 32

// 定义PaperS3屏幕尺寸（根据实际型号调整）
#define PAPER_S3_WIDTH 540
#define PAPER_S3_HEIGHT 960

// 统一边距定义
#define MARGIN_TOP 26
#define MARGIN_BOTTOM 6
#define MARGIN_LEFT 27
#define MARGIN_RIGHT 18
#define VERTICAL_TOP_DELTA 0
#define VERTICAL_RIGHT_DELTA 0

#define LINE_MARGIN 24
#define GRAY_THRESHOLD 15 // 1-15

// 字符间距定义（像素）
#define CHAR_SPACING_HORIZONTAL 2  // 横排模式字符间距（X方向）
#define CHAR_SPACING_VERTICAL 6   // 竖排模式字符间距（Y方向）- 测试更大间距效果

// Cache Block
#define CACHE_BLOCK_SIZE 512

// Interrupt Tick in mS, 中断查询 , 决定了系统平日轮询的节奏
#define DEVICE_INTERRUPT_TICK 10
// Touch press gap in ms to avoid rapid repeated processing after a press// 双击判断和消抖- normally 400, but as we ara using pressed, saved 200ms period, so 600
#define TOUCH_PRESS_GAP_MS 200

// 页面摘要字符数量
#define DIGEST_NUM 50

// 通用回收池字符上限
#define RECYCLE_POOL_LIMIT 1500

// 定时
#define IDLE_PWR_WAIT_MIN 30 // 10min go to poweroff
#define READING_IDLE_WAIT_MIN 10 // 10min more in idle go to shutdown

//index timing
#define BATCH_DELAY 20
#define PAGES_DELAY 20

// 文件管理最大返回数量 - 主菜单文件列表限制
#define MAX_MAIN_MENU_FILE_COUNT 99

// Label shift

#define BOOKMARKOFFSET 82 //0.618


/* 全局配置结构体 */
struct GlobalConfig {
    uint_fast8_t rotation = 2;                    // 屏幕旋转方向 (0-3)
    char currentReadFile[256] = "/spiffs/ReadPaper.txt";  // 当前阅读文件路径 (支持/spiffs/或/sd/前缀)
    char fontset[64] = "/spiffs/lite.bin";                  // 字体设置: "default"=内置字体, "customize"=自定义字体
    char pageStyle[16] = "default";                // 页面样式: 'default' 或其他样式名
    char labelposition[16] = "default";           // 标签位置: 'default' / 'top' / 'bottom' / etc
    char marktheme[16] = "dark";                  // 书签主题: 'dark' / 'light' / 'random'
    bool defaultlock = true;                       // 通用壁纸: true=默认壁纸, false=随机壁纸

    // 繁简转换模式: 0 = 不转换 (default), 1 = 转为简体 (simple), 2 = 转为繁体 (tradition)
    uint8_t zh_conv_mode = 1;

    // UI theme flag: dark mode (true) or light mode (false)
    bool dark = false;

    // 优化刷新模式开关: 当为 true 时使用快速（可能是部分刷新的）刷新策略
    // 默认 false（保守模式）
    bool fastrefresh = false;

    // 自动翻页速度，取值范围 1..255，默认 2
    uint8_t autospeed = 2;

    // 主菜单文件列表限制（可通过配置文件调整，默认由宏 MAX_MAIN_MENU_FILE_COUNT 设定）
    uint16_t main_menu_file_count = MAX_MAIN_MENU_FILE_COUNT;

    // 未来可以添加更多配置项：
    // bool auto_brightness = true;
    // uint8_t font_scale = 100;
    // uint32_t sleep_timeout = 300000;  // 休眠超时(ms)
    // etc...
};

// 是否在初始化后进入调试界面（true -> 进入 STATE_DEBUG 而不是默认 IDLE）
extern bool enterDebug;

// 调试宏：用于性能诊断

// 字形预读窗口控制
#define ENABLE_GLYPH_READ_WINDOW 0  // 禁用预读窗口（设为1启用）

// ====== B测试预读窗口开关 ======
// 用于对比有无预读窗口的性能差异
#define ENABLE_PREREAD_WINDOW_IN_B_TEST 0  // 设为1使用预读窗口，设为0使用直接读r


//有一部分配置放到了/src/global.cpp 勿忘
/*
例如比较关键的：决定从v1.6开始使用非内存强吃的字体方式而是代码编入和缓存读取实现的
int8_t fontLoadLoc = 1;
*/


#define TEXT_COLORDEPTH 1
#define TEXT_COLORDEPTH_HIGH 16

//#define GREY_MAP_COLOR 0xA855
//#define GREY_MAP_COLOR 0xA554 // 0xAAAAAA
//#define GREY_MAP_COLOR 0x94B2 // 0x999999
#define GREY_MAP_COLOR 0x8430 // 0x888888
