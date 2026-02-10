#include "config_manager.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include "papers3.h"
#include "../SD/SDWrapper.h"
#include "device/safe_fs.h"
#include "test/per_file_debug.h"

#include "current_book.h"
// 外部全局配置变量
extern GlobalConfig g_config;
#include "../globals.h"

// 内部状态
static bool config_initialized = false;
static ConfigStats stats;

// 调试输出开关

bool config_init()
{
    if (config_initialized)
    {
        return true;
    }

#if DBG_CONFIG_MANAGER
    Serial.println("[CONFIG] 初始化配置管理器...");
#endif

    // 确保SD卡已挂载
    if (!SDW::SD.begin())
    {
#if DBG_CONFIG_MANAGER
    Serial.println("[CONFIG] 错误：SD卡挂载失败");
#endif
    return false;
    }

    // 尝试加载配置，如果失败则使用默认配置
    if (config_load())
    {
#if DBG_CONFIG_MANAGER
        Serial.println("[CONFIG] ✅ 配置加载成功");
        Serial.printf("[CONFIG] 当前书籍: %s\n", g_config.currentReadFile);
        Serial.printf("[CONFIG] 配置序列号: %u\n", stats.sequence);
#endif
    }
    else
    {
#if DBG_CONFIG_MANAGER
        Serial.println("[CONFIG] ❌ 配置加载失败（A/B 文件都不存在或损坏），使用默认配置");
#endif
        config_reset_to_defaults();
        // 保存默认配置到文件
        config_save();
    }

    config_initialized = true;

#if DBG_CONFIG_MANAGER
    Serial.println("[CONFIG] 配置管理器初始化完成");
    Serial.println("[CONFIG] ========================================");
#endif

    return true;
}

bool config_save()
{
    // 如果配置管理器尚未初始化，尝试挂载SD卡并继续保存
    if (!config_initialized)
    {
#if DBG_CONFIG_MANAGER
        Serial.println("[CONFIG] config_manager 未初始化，尝试挂载 SD 并保存配置...");
#endif
        bool sd_ok = false;

        // 先尝试默认方式（库会使用默认 SPI 总线）
        if (SDW::SD.begin())
        {
            sd_ok = true;
        }
        else
        {
        }

        if (!sd_ok)
        {
#if DBG_CONFIG_MANAGER
            Serial.println("[CONFIG] 无法挂载 SD 卡，配置保存失败");
#endif
            return false;
        }

        // 标记已初始化以避免后续重复尝试
        config_initialized = true;
    }

#if DBG_CONFIG_MANAGER
    Serial.println("[CONFIG] 保存配置到文件（双写备份策略）...");
#endif

    // 序列号递增
    stats.sequence++;

#if DBG_CONFIG_MANAGER
    Serial.printf("[CONFIG] 写入双份配置文件 (sequence=%u)\n", stats.sequence);
#endif

    // 保存前确保约束条件：dark 模式下强制启用快刷
    if (g_config.dark)
    {
        g_config.fastrefresh = true;
    }

    // Lambda 函数：写入配置内容（A 和 B 使用相同的写入逻辑）
    auto write_config = [&](File &config_file) -> bool {
        // 写入配置文件头（包含序列号）
        config_file.printf("# ReaderPaper 配置文件\n");
        config_file.printf("# 版本: %d\n", CONFIG_VERSION);
        config_file.printf("# 序列号: %u\n", stats.sequence);
        config_file.printf("# 生成时间: %lu\n", millis());
        config_file.printf("\n");

        // 写入序列号（用于加载时判断哪个文件最新）
        config_file.printf("sequence=%u\n", stats.sequence);
        
        // 写入配置项
        config_file.printf("version=%d\n", CONFIG_VERSION);
        config_file.printf("rotation=%d\n", g_config.rotation);
        // 写入字体设置
        config_file.printf("fontset=%s\n", g_config.fontset);
        // 页面样式和标签位置
        config_file.printf("pageStyle=%s\n", g_config.pageStyle);
        config_file.printf("labelposition=%s\n", g_config.labelposition);
        config_file.printf("marktheme=%s\n", g_config.marktheme);
        config_file.printf("defaultlock=%d\n", g_config.defaultlock ? 1 : 0);
        config_file.printf("currentReadFile=%s\n", g_config.currentReadFile);
        // 繁简转换配置
        config_file.printf("zh_conv_mode=%d\n", g_config.zh_conv_mode);
        // UI theme: dark mode
        config_file.printf("dark=%s\n", g_config.dark ? "true" : "false");
        // sync runtime global autospeed into saved config
        g_config.autospeed = ::autospeed;
        config_file.printf("autospeed=%u\n", g_config.autospeed);

        // fastrefresh: whether to use fast partial refresh strategy
        config_file.printf("fastrefresh=%s\n", g_config.fastrefresh ? "true" : "false");

        // 主菜单文件限制
        config_file.printf("main_menu_file_count=%u\n", (unsigned int)g_config.main_menu_file_count);

        // WebDAV 配置
        config_file.printf("webdav_url=%s\n", g_config.webdav_url);
        config_file.printf("webdav_user=%s\n", g_config.webdav_user);
        config_file.printf("webdav_pass=%s\n", g_config.webdav_pass);

        // WiFi 配置
        config_file.printf("wifi_ssid=%s\n", g_config.wifi_ssid);
        config_file.printf("wifi_pass=%s\n", g_config.wifi_pass);

        // 未来扩展的配置项可以在这里添加
        // config_file.printf("auto_brightness=%s\n", g_config.auto_brightness ? "true" : "false");
        // config_file.printf("font_scale=%d\n", g_config.font_scale);
        // config_file.printf("sleep_timeout=%lu\n", g_config.sleep_timeout);

        config_file.printf("\n# 文件结束\n");
        return true;
    };

    // 写入 A 文件
    bool ok_a = SafeFS::safeWrite(CONFIG_FILE_A, write_config);
    
    if (!ok_a)
    {
#if DBG_CONFIG_MANAGER
        Serial.printf("[CONFIG] ⚠️ 无法写入 %s\n", CONFIG_FILE_A);
#endif
    }
    else
    {
#if DBG_CONFIG_MANAGER
        Serial.printf("[CONFIG] ✅ %s 写入成功\n", CONFIG_FILE_A);
#endif
    }

    // 延迟 100ms，避免两次写入冲突
    delay(100);

    // 写入 B 文件
    bool ok_b = SafeFS::safeWrite(CONFIG_FILE_B, write_config);
    
    if (!ok_b)
    {
#if DBG_CONFIG_MANAGER
        Serial.printf("[CONFIG] ⚠️ 无法写入 %s\n", CONFIG_FILE_B);
#endif
    }
    else
    {
#if DBG_CONFIG_MANAGER
        Serial.printf("[CONFIG] ✅ %s 写入成功\n", CONFIG_FILE_B);
#endif
    }

    // 只要有一个文件写入成功就算成功
    bool ok = ok_a || ok_b;
    
    if (!ok)
    {
#if DBG_CONFIG_MANAGER
        Serial.println("[CONFIG] ❌ 错误：A 和 B 文件都无法写入！");
#endif
        return false;
    }

    // 更新统计信息
    stats.total_saves++;
    stats.last_save_time = millis();

#if DBG_CONFIG_MANAGER
    if (ok_a && ok_b) {
        Serial.printf("[CONFIG] ✅ 配置双写成功 (第 %lu 次保存, seq=%u)\n", 
                      stats.total_saves, stats.sequence);
    } else if (ok_a) {
        Serial.printf("[CONFIG] ⚠️ 仅 A 文件写入成功 (第 %lu 次保存, seq=%u)\n", 
                      stats.total_saves, stats.sequence);
    } else {
        Serial.printf("[CONFIG] ⚠️ 仅 B 文件写入成功 (第 %lu 次保存, seq=%u)\n", 
                      stats.total_saves, stats.sequence);
    }
#endif

    return true;
}

// 辅助函数：从单个配置文件加载并返回序列号
// 返回值：成功返回序列号，失败返回 -1
static int32_t config_load_from_file(const char* path, GlobalConfig& out_config, bool silent = false)
{
    // 尝试从 .tmp 恢复
    SafeFS::restoreFromTmpIfNeeded(path);
    
    if (!SDW::SD.exists(path))
    {
        if (!silent) {
#if DBG_CONFIG_MANAGER
            Serial.printf("[CONFIG] 配置文件不存在: %s\n", path);
#endif
        }
        return -1;
    }

    File config_file = SDW::SD.open(path, "r");
    if (!config_file)
    {
        if (!silent) {
#if DBG_CONFIG_MANAGER
            Serial.printf("[CONFIG] 无法打开配置文件: %s\n", path);
#endif
        }
        return -1;
    }

    // 临时配置，默认值
    GlobalConfig temp_config = out_config;
    
    int loaded_version = 0;
    uint32_t loaded_sequence = 0;
    bool version_found = false;
    bool sequence_found = false;

    // 逐行读取配置文件
    while (config_file.available())
    {
        String line = config_file.readStringUntil('\n');
        line.trim();

        // 跳过注释和空行
        if (line.length() == 0 || line.startsWith("#"))
        {
            continue;
        }

        // 查找等号分隔符
        int eq_pos = line.indexOf('=');
        if (eq_pos < 0)
        {
            continue;
        }

        String key = line.substring(0, eq_pos);
        String value = line.substring(eq_pos + 1);
        key.trim();
        value.trim();

        // 解析配置项
        if (key == "sequence")
        {
            loaded_sequence = value.toInt();
            sequence_found = true;
        }
        else if (key == "version")
        {
            loaded_version = value.toInt();
            version_found = true;
        }
        else if (key == "rotation")
        {
            int rotation = value.toInt();
            if (rotation >= 0 && rotation <= 3)
            {
                temp_config.rotation = rotation;
            }
        }
        else if (key == "fontset")
        {
            strncpy(temp_config.fontset, value.c_str(), sizeof(temp_config.fontset) - 1);
            temp_config.fontset[sizeof(temp_config.fontset) - 1] = '\0';
        }
        else if (key == "pageStyle")
        {
            strncpy(temp_config.pageStyle, value.c_str(), sizeof(temp_config.pageStyle) - 1);
            temp_config.pageStyle[sizeof(temp_config.pageStyle) - 1] = '\0';
        }
        else if (key == "labelposition")
        {
            strncpy(temp_config.labelposition, value.c_str(), sizeof(temp_config.labelposition) - 1);
            temp_config.labelposition[sizeof(temp_config.labelposition) - 1] = '\0';
        }
        else if (key == "marktheme")
        {
            strncpy(temp_config.marktheme, value.c_str(), sizeof(temp_config.marktheme) - 1);
            temp_config.marktheme[sizeof(temp_config.marktheme) - 1] = '\0';
        }
        else if (key == "defaultlock")
        {
            temp_config.defaultlock = (value == "1" || value == "true");
        }
        else if (key == "zh_conv_mode")
        {
            int m = value.toInt();
            if (m >= 0 && m <= 2)
                temp_config.zh_conv_mode = (uint8_t)m;
        }
        else if (key == "currentReadFile")
        {
            strncpy(temp_config.currentReadFile, value.c_str(), sizeof(temp_config.currentReadFile) - 1);
            temp_config.currentReadFile[sizeof(temp_config.currentReadFile) - 1] = '\0';
#if DBG_CONFIG_MANAGER
            if (!silent) {
                Serial.printf("[CONFIG] 从文件加载 currentReadFile: '%s' (长度: %d)\n", 
                              temp_config.currentReadFile, strlen(temp_config.currentReadFile));
            }
#endif
        }
        else if (key == "dark")
        {
            temp_config.dark = (value == "true" || value == "1");
            // dark 模式下强制启用快刷
            if (temp_config.dark)
            {
                temp_config.fastrefresh = true;
            }
        }
        else if (key == "fastrefresh")
        {
            temp_config.fastrefresh = (value == "true" || value == "1");
            // 如果已经加载了 dark 模式，确保快刷不会被关闭
            // 注意：这里依赖于配置文件中 dark 在 fastrefresh 之前
        }
        else if (key == "autospeed")
        {
            int v = value.toInt();
            if (v < 1) v = 1;
            if (v > 255) v = 255;
            temp_config.autospeed = (uint8_t)v;
        }
        else if (key == "main_menu_file_count")
        {
            int v = value.toInt();
            if (v < 1) v = 1;
            // 上限以编译期宏为准，防止运行时设置过大导致内存耗尽
            if (v > MAX_MAIN_MENU_FILE_COUNT) v = MAX_MAIN_MENU_FILE_COUNT;
            temp_config.main_menu_file_count = (uint16_t)v;
        }
        else if (key == "webdav_url")
        {
            strncpy(temp_config.webdav_url, value.c_str(), sizeof(temp_config.webdav_url) - 1);
            temp_config.webdav_url[sizeof(temp_config.webdav_url) - 1] = '\0';
        }
        else if (key == "webdav_user")
        {
            strncpy(temp_config.webdav_user, value.c_str(), sizeof(temp_config.webdav_user) - 1);
            temp_config.webdav_user[sizeof(temp_config.webdav_user) - 1] = '\0';
        }
        else if (key == "webdav_pass")
        {
            strncpy(temp_config.webdav_pass, value.c_str(), sizeof(temp_config.webdav_pass) - 1);
            temp_config.webdav_pass[sizeof(temp_config.webdav_pass) - 1] = '\0';
        }
        else if (key == "wifi_ssid")
        {
            strncpy(temp_config.wifi_ssid, value.c_str(), sizeof(temp_config.wifi_ssid) - 1);
            temp_config.wifi_ssid[sizeof(temp_config.wifi_ssid) - 1] = '\0';
        }
        else if (key == "wifi_pass")
        {
            strncpy(temp_config.wifi_pass, value.c_str(), sizeof(temp_config.wifi_pass) - 1);
            temp_config.wifi_pass[sizeof(temp_config.wifi_pass) - 1] = '\0';
        }
    }

    config_file.close();

    // 检查版本兼容性
    if (!version_found || loaded_version != CONFIG_VERSION)
    {
        if (!silent) {
#if DBG_CONFIG_MANAGER
            Serial.printf("[CONFIG] 警告：%s 版本不匹配 (文件: %d, 期望: %d)\n",
                          path, loaded_version, CONFIG_VERSION);
#endif
        }
        // 版本不匹配时仍然加载，但返回序列号 0 表示优先级低
        loaded_sequence = 0;
    }

    // 输出结果
    out_config = temp_config;
    
    // 验证关键字段：如果 currentReadFile 为空，使用默认值
    if (temp_config.currentReadFile[0] == '\0') {
#if DBG_CONFIG_MANAGER
        if (!silent) {
            Serial.printf("[CONFIG] ⚠️ %s 中 currentReadFile 为空，使用默认值\n", path);
        }
#endif
        strcpy(out_config.currentReadFile, "/spiffs/ReadPaper.txt");
    }
    
    if (!sequence_found && !silent) {
#if DBG_CONFIG_MANAGER
        Serial.printf("[CONFIG] %s 无序列号，视为旧版本配置\n", path);
#endif
        return 0;  // 旧版本配置文件，序列号为 0
    }

    return (int32_t)loaded_sequence;
}

// 内部函数：初始化配置结构体为默认值（不输出日志）
static void init_config_defaults(GlobalConfig& config)
{
    config.rotation = 2;
    strcpy(config.currentReadFile, "/spiffs/ReadPaper.txt");
    strncpy(config.fontset, "/spiffs/lite.bin", sizeof(config.fontset) - 1);
    config.fontset[sizeof(config.fontset) - 1] = '\0';
    strcpy(config.pageStyle, "default");
    strcpy(config.labelposition, "default");
    strcpy(config.marktheme, "dark");
    config.defaultlock = true;
    config.zh_conv_mode = 1;
    config.dark = false;
    config.fastrefresh = false;
    config.autospeed = 2;
    // 主菜单文件默认上限
    config.main_menu_file_count = MAX_MAIN_MENU_FILE_COUNT;

    // WebDAV 默认配置（空字符串）
    config.webdav_url[0] = '\0';
    config.webdav_user[0] = '\0';
    config.webdav_pass[0] = '\0';

    // WiFi 默认配置（空字符串）
    config.wifi_ssid[0] = '\0';
    config.wifi_pass[0] = '\0';
}

bool config_load()
{
#if DBG_CONFIG_MANAGER
    Serial.println("[CONFIG] 从双文件策略加载配置...");
#endif

    // 尝试从 A 和 B 两个文件加载
    GlobalConfig config_a, config_b;
    init_config_defaults(config_a);
    init_config_defaults(config_b);

    int32_t seq_a = config_load_from_file(CONFIG_FILE_A, config_a, false);
    int32_t seq_b = config_load_from_file(CONFIG_FILE_B, config_b, false);

#if DBG_CONFIG_MANAGER
    Serial.printf("[CONFIG] %s: seq=%d, %s: seq=%d\n", 
                  CONFIG_FILE_A, seq_a, CONFIG_FILE_B, seq_b);
#endif

    // 选择序列号更大的（更新的）配置
    bool use_a = false;
    bool use_b = false;

    if (seq_a > 0 && seq_b > 0)
    {
        // 两个文件都存在，选择序列号更大的
        if (seq_a > seq_b) {
            use_a = true;
        } else {
            use_b = true;
        }
    }
    else if (seq_a > 0)
    {
        // 只有 A 存在
        use_a = true;
    }
    else if (seq_b > 0)
    {
        // 只有 B 存在
        use_b = true;
    }
    else
    {
        // 两个文件都不存在，尝试从旧版本 readpaper.cfg 加载
#if DBG_CONFIG_MANAGER
        Serial.printf("[CONFIG] ⚠️ A/B 双文件都无法加载 (A: seq=%d, B: seq=%d)\n", seq_a, seq_b);
        Serial.printf("[CONFIG] 尝试从旧版本单文件 %s 加载...\n", CONFIG_FILE_PATH);
#endif
        GlobalConfig config_old;
        init_config_defaults(config_old);
        
        int32_t seq_old = config_load_from_file(CONFIG_FILE_PATH, config_old, false);
        if (seq_old >= 0)
        {
            // 成功加载旧版本配置
            g_config = config_old;
            stats.sequence = 0;  // 重置序列号
            stats.total_loads++;
            stats.last_load_time = millis();
            
            // 确保 currentReadFile 不为空
            if (g_config.currentReadFile[0] == '\0') {
#if DBG_CONFIG_MANAGER
                Serial.println("[CONFIG] ⚠️ 旧版本配置中 currentReadFile 为空，使用默认值");
#endif
                strcpy(g_config.currentReadFile, "/spiffs/ReadPaper.txt");
            }
            
#if DBG_CONFIG_MANAGER
            Serial.printf("[CONFIG] ✅ 从旧版本配置加载成功 (currentReadFile: %s)\n", g_config.currentReadFile);
            Serial.printf("[CONFIG] 下次保存将自动升级到 A/B 双文件策略\n");
#endif
        // sync autospeed
        ::autospeed = g_config.autospeed;
            
            // 确保约束条件：dark 模式下强制启用快刷
            if (g_config.dark)
            {
                g_config.fastrefresh = true;
            }
            
            return true;
        }
        
#if DBG_CONFIG_MANAGER
        Serial.println("[CONFIG] ❌❌ 严重错误: 所有配置文件都不存在或损坏");
        Serial.printf("[CONFIG]    检查项:\n");
        Serial.printf("[CONFIG]    - %s: %s\n", CONFIG_FILE_A, seq_a > 0 ? "有效" : "无效");
        Serial.printf("[CONFIG]    - %s: %s\n", CONFIG_FILE_B, seq_b > 0 ? "有效" : "无效");
        Serial.printf("[CONFIG]    - %s: %s\n", CONFIG_FILE_PATH, seq_old >= 0 ? "有效" : "无效");
        Serial.printf("[CONFIG]    系统将使用硬编码的默认配置\n");
#endif
        return false;
    }

    // 应用选择的配置
    if (use_a)
    {
        g_config = config_a;
        stats.sequence = (uint32_t)seq_a;
#if DBG_CONFIG_MANAGER
        Serial.printf("[CONFIG] 使用 %s (seq=%d)\n", CONFIG_FILE_A, seq_a);
        Serial.printf("[CONFIG] 应用后 currentReadFile: '%s'\n", g_config.currentReadFile);
#endif
    }
    else if (use_b)
    {
        g_config = config_b;
        stats.sequence = (uint32_t)seq_b;
#if DBG_CONFIG_MANAGER
        Serial.printf("[CONFIG] 使用 %s (seq=%d)\n", CONFIG_FILE_B, seq_b);
        Serial.printf("[CONFIG] 应用后 currentReadFile: '%s'\n", g_config.currentReadFile);
#endif
    }

    // 将配置中的 autospeed 同步到运行时全局变量
    ::autospeed = g_config.autospeed;

    // 确保约束条件：dark 模式下强制启用快刷
    if (g_config.dark)
    {
        g_config.fastrefresh = true;
    }

    // 更新统计信息
    stats.total_loads++;
    stats.last_load_time = millis();

    // 最后的安全检查：确保 currentReadFile 不为空
    if (g_config.currentReadFile[0] == '\0') {
#if DBG_CONFIG_MANAGER
        Serial.println("[CONFIG] ⚠️ 警告: currentReadFile 为空，强制使用默认值");
#endif
        strcpy(g_config.currentReadFile, "/spiffs/ReadPaper.txt");
    }

#if DBG_CONFIG_MANAGER
    Serial.printf("[CONFIG] 配置加载完成 (第 %lu 次, seq=%u)\n", 
                  stats.total_loads, stats.sequence);
    Serial.printf("[CONFIG] - 屏幕旋转: %d\n", g_config.rotation);
    Serial.printf("[CONFIG] - 当前书籍: %s\n", g_config.currentReadFile);
#endif

    return true;
}

void config_reset_to_defaults()
{
#if DBG_CONFIG_MANAGER
    Serial.println("[CONFIG] 🔄 重置全局配置为默认值（所有配置文件都不可用）");
#endif

    g_config.rotation = 2;                                     // 默认旋转方向
    strcpy(g_config.currentReadFile, "/spiffs/ReadPaper.txt"); // 默认阅读文件
    // 默认字体设置
    strncpy(g_config.fontset, "/spiffs/lite.bin", sizeof(g_config.fontset) - 1);
    g_config.fontset[sizeof(g_config.fontset) - 1] = '\0';
    // 默认页面样式与标签位置
    strcpy(g_config.pageStyle, "default");
    strcpy(g_config.labelposition, "default");
    strcpy(g_config.marktheme, "dark");
    g_config.defaultlock = true;
    g_config.zh_conv_mode = 1; // 默认显示简体（0=不转换,1=简体,2=繁体）
    // UI theme: default to light mode
    g_config.dark = false;
    g_config.fastrefresh = false;
    g_config.autospeed = 2;

    // 同步运行时全局变量
    ::autospeed = g_config.autospeed;

    // 主菜单文件默认上限
    g_config.main_menu_file_count = MAX_MAIN_MENU_FILE_COUNT;

    // WebDAV 默认配置（空字符串）
    g_config.webdav_url[0] = '\0';
    g_config.webdav_user[0] = '\0';
    g_config.webdav_pass[0] = '\0';

    // 未来的默认配置可以在这里添加
    // g_config.auto_brightness = true;
    // g_config.font_scale = 100;
    // g_config.sleep_timeout = 300000;
}

bool config_file_exists()
{
    return SDW::SD.exists(CONFIG_FILE_PATH);
}

bool config_delete()
{
    if (!SDW::SD.exists(CONFIG_FILE_PATH))
    {
        return true; // 文件不存在，认为删除成功
    }

    bool result = SDW::SD.remove(CONFIG_FILE_PATH);

#if DBG_CONFIG_MANAGER
    if (result)
    {
        Serial.println("[CONFIG] 配置文件删除成功");
    }
    else
    {
        Serial.println("[CONFIG] 配置文件删除失败");
    }
#endif

    return result;
}

bool config_get_file_info(size_t *file_size, unsigned long *last_modified)
{
    if (!SDW::SD.exists(CONFIG_FILE_PATH))
    {
        return false;
    }
    File config_file = SDW::SD.open(CONFIG_FILE_PATH, "r");
    if (!config_file)
    {
        return false;
    }

    if (file_size)
    {
        *file_size = config_file.size();
    }

    if (last_modified)
    {
        *last_modified = config_file.getLastWrite();
    }

    config_file.close();
    return true;
}

ConfigStats config_get_stats()
{
    return stats;
}

bool config_set_current_file(const char *file_path)
{
    if (!file_path)
    {
        // 清空当前文件
        g_config.currentReadFile[0] = '\0';
    }
    else
    {
        // 复制文件路径，确保不溢出
        strncpy(g_config.currentReadFile, file_path, sizeof(g_config.currentReadFile) - 1);
        g_config.currentReadFile[sizeof(g_config.currentReadFile) - 1] = '\0';
    }

    // 立即保存配置
    return config_save();
}

#include "text/book_handle.h"
#include <SPIFFS.h>

BookHandle *config_update_current_book(const char *file_path, int16_t area_w, int16_t area_h, float fsize)
{
    if (!file_path)
        return nullptr;

    // only accept /sd/book/ paths
    if (strncmp(file_path, "/sd/book/", 9) != 0)
    {
#if DBG_CONFIG_MANAGER
        Serial.printf("[CONFIG] 路径不符合要求 (必须以 /sd/book/ 开头): %s\n", file_path);
#endif
        return nullptr;
    }

    const char *real_file_path = file_path + 3; // skip "/sd"
    bool file_exists = SDW::SD.exists(real_file_path);

#if DBG_CONFIG_MANAGER
    Serial.printf("[CONFIG] 尝试打开配置中的书籍: %s\n", file_path);
    Serial.printf("[CONFIG] SD卡检查: 文件%s\n", file_exists ? "存在" : "不存在");
#endif

    // Try opening the requested file first
    if (file_exists)
    {
        auto new_sp = std::make_shared<BookHandle>(std::string(file_path), area_w, area_h, fsize, TextEncoding::AUTO_DETECT);
        BookHandle *new_book = new_sp ? new_sp.get() : nullptr;

        if (new_book && new_book->isOpen())
        {
#if DBG_CONFIG_MANAGER
            Serial.printf("[CONFIG] ✅ 成功打开书籍文件: %s\n", file_path);
#endif
            // CRITICAL FIX: only write config after we successfully open the new book.
            // This prevents overwriting a valid config with a new one that fails mid-write due to power loss.
            if (!config_set_current_file(file_path))
            {
#if DBG_CONFIG_MANAGER
                Serial.printf("[CONFIG] 警告: 保存新书籍路径配置失败: %s\n", file_path);
#endif
            }

            // attempt a graceful handover: notify background indexer and mark old book for close
            auto old_sp = std::atomic_load(&__g_current_book_shared);
            if (old_sp && old_sp.get() != new_book)
            {
                if (old_sp->isIndexingInProgress())
                {
#if DBG_CONFIG_MANAGER
                    Serial.printf("[CONFIG] 旧书索引正在进行，调用 stopIndexingAndWait(5000)...\n");
#endif
                    old_sp->stopIndexingAndWait(5000);
#if DBG_CONFIG_MANAGER
                    Serial.println("[CONFIG] 旧书索引停止请求已发出，继续替换旧实例");
#endif
                }
                
                // 【关键修复】在切换书籍前，强制更新旧书的.page文件count字段
                // 确保即使索引未完成，下次打开时也能正确加载所有已索引的页面
                if (!old_sp->isIndexingComplete() && old_sp->getTotalPages() > 0)
                {
#if DBG_CONFIG_MANAGER
                    Serial.printf("[CONFIG] 更新旧书.page文件count=%zu\n", old_sp->getTotalPages());
#endif
                    extern bool patchPageFileCount(const std::string &page_file, uint32_t count);
                    std::string old_page_file = old_sp->getPageFileName();
                    patchPageFileCount(old_page_file, (uint32_t)old_sp->getTotalPages());
                }
                
                old_sp->markForClose();
                old_sp->stopIndexingAndWait(5000);
            }

            // publish new book
            std::atomic_store(&__g_current_book_shared, new_sp);
            // reset autoread when switching to a new book
            autoread = false;
#if DBG_CONFIG_MANAGER
            Serial.printf("[CONFIG] 成功切换到新书籍: %s\n", file_path);
#endif
            return new_book;
        }
        else
        {
            // 文件存在但无法打开
#if DBG_CONFIG_MANAGER
            Serial.printf("[CONFIG] ❌ 回退原因: 文件存在但无法打开 (BookHandle 创建失败或 isOpen() 返回 false)\n");
            Serial.printf("[CONFIG]    问题文件: %s\n", file_path);
            Serial.printf("[CONFIG]    可能原因: 文件损坏、格式不支持、内存不足等\n");
#endif
        }
    }
    else
    {
        // 文件不存在
#if DBG_CONFIG_MANAGER
        Serial.printf("[CONFIG] ❌ 回退原因: SD卡上找不到配置中的文件\n");
        Serial.printf("[CONFIG]    配置路径: %s\n", file_path);
        Serial.printf("[CONFIG]    SD卡路径: %s\n", real_file_path);
        Serial.printf("[CONFIG]    可能原因: SD卡未挂载、文件被删除、路径错误等\n");
#endif
    }

    // Fallback: try default file in SPIFFS
#if DBG_CONFIG_MANAGER
    Serial.printf("[CONFIG] 🔄 尝试回退到默认文件: /spiffs/ReadPaper.txt\n");
#endif
    const char *default_file = "/spiffs/ReadPaper.txt";
    // CRITICAL FIX: before writing default_file to config, try to read current config first
    // to avoid overwriting a valid currentReadFile if safeWrite fails mid-way during power loss.
    // Only update config if we successfully open the default book.
    if (SPIFFS.exists("/ReadPaper.txt"))
    {
#if DBG_CONFIG_MANAGER
        Serial.printf("[CONFIG] SPIFFS 中找到默认文件，尝试打开...\n");
#endif
        auto def_sp = std::make_shared<BookHandle>(std::string(default_file), area_w, area_h, fsize, TextEncoding::AUTO_DETECT);
        BookHandle *def_book = def_sp ? def_sp.get() : nullptr;
        if (def_book && def_book->isOpen())
        {
#if DBG_CONFIG_MANAGER
            Serial.printf("[CONFIG] ✅ 默认文件打开成功，保存到配置\n");
#endif
            // Save current file path to config only after confirming successful open
            if (!config_set_current_file(default_file))
            {
#if DBG_CONFIG_MANAGER
                Serial.printf("[CONFIG] 警告: 保存默认文件配置失败，后续启动可能回退到旧配置\n");
#endif
            }
            auto old_sp = std::atomic_load(&__g_current_book_shared);
            if (old_sp && old_sp.get() != def_book)
            {
                old_sp->markForClose();
                old_sp->stopIndexingAndWait(2000);
            }
            std::atomic_store(&__g_current_book_shared, def_sp);
            // reset autoread when falling back to default book
            autoread = false;
#if DBG_CONFIG_MANAGER
            Serial.printf("[CONFIG] ✅ 回退完成: 当前使用默认文件 %s\n", default_file);
#endif
            return def_book;
        }
        else
        {
#if DBG_CONFIG_MANAGER
            Serial.printf("[CONFIG] ❌ 严重错误: 默认文件存在但无法打开\n");
            Serial.printf("[CONFIG]    文件路径: %s\n", default_file);
#endif
        }
    }
    else
    {
#if DBG_CONFIG_MANAGER
        Serial.printf("[CONFIG] ❌ 严重错误: SPIFFS 中找不到默认文件 /ReadPaper.txt\n");
#endif
    }

#if DBG_CONFIG_MANAGER
    Serial.printf("[CONFIG] ❌❌ 致命错误: 无法打开任何文件（包括默认文件），系统无可用书籍\n");
#endif
    config_set_current_file(nullptr);
    return nullptr;
}