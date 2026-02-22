#include "comp_history_cache.h"
#include "SD/SDWrapper.h"
#include "test/per_file_debug.h"
#include <ArduinoJson.h>

// SD 卡上历史缓存文件的路径
static const char *CACHE_FILE_PATH = "/rdt/history.cache";

// JSON 文档容量（字节）：支持约 6-8 个组件各存 2 条中等长度字符串
static const size_t CACHE_DOC_CAPACITY = 8192;

// ──────────────────────────────────────────────
// 内部辅助：构建查找键   type + "_" + zIndex
// ──────────────────────────────────────────────
static String make_key(const char *type, int zIndex)
{
    String key = String(type);
    key += '_';
    key += String(zIndex);
    return key;
}

// ──────────────────────────────────────────────
// 内部辅助：从 SD 卡读取缓存 JSON
// 返回 false 表示文件不存在或解析失败
// ──────────────────────────────────────────────
static bool read_cache_doc(DynamicJsonDocument &doc)
{
    if (!SDW::SD.exists(CACHE_FILE_PATH))
        return false;

    File f = SDW::SD.open(CACHE_FILE_PATH, "r");
    if (!f)
        return false;

    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err != DeserializationError::Ok)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[CACHE] 解析缓存文件失败: %s\n", err.c_str());
#endif
        return false;
    }
    return true;
}

// ──────────────────────────────────────────────
// 内部辅助：将 JSON 文档写回 SD 卡
// ──────────────────────────────────────────────
static bool write_cache_doc(DynamicJsonDocument &doc)
{
    // 确保目录存在
    if (!SDW::SD.exists("/rdt"))
        SDW::SD.mkdir("/rdt");

    if (SDW::SD.exists(CACHE_FILE_PATH))
        SDW::SD.remove(CACHE_FILE_PATH);

    File f = SDW::SD.open(CACHE_FILE_PATH, "w");
    if (!f)
    {
#if DBG_TRMNL_SHOW
        Serial.println("[CACHE] 无法写入缓存文件");
#endif
        return false;
    }

    serializeJson(doc, f);
    f.close();
    return true;
}

// ──────────────────────────────────────────────
// cache_clear_history
// ──────────────────────────────────────────────
void cache_clear_history()
{
    if (SDW::SD.exists(CACHE_FILE_PATH))
    {
        SDW::SD.remove(CACHE_FILE_PATH);
#if DBG_TRMNL_SHOW
        Serial.println("[CACHE] 历史缓存已清空（RDT 文件已更新）");
#endif
    }
}

// ──────────────────────────────────────────────
// cache_save
// ──────────────────────────────────────────────
void cache_save(const char *type, int zIndex, const char *v1, const char *v2)
{
    if (!type || !v1) return;

    DynamicJsonDocument doc(CACHE_DOC_CAPACITY);

    // 先读取已有缓存（不存在时以空文档开始）
    read_cache_doc(doc);

    String key = make_key(type, zIndex);

    // 写入或覆盖对应条目
    JsonObject entry = doc[key].as<JsonObject>();
    if (entry.isNull())
        entry = doc.createNestedObject(key);

    entry["v1"] = v1;
    if (v2 != nullptr)
        entry["v2"] = v2;

    if (write_cache_doc(doc))
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[CACHE] 已缓存 [%s]: v1='%.40s'%s\n",
                      key.c_str(), v1,
                      (v2 ? "  v2=..." : ""));
#endif
    }
}

// ──────────────────────────────────────────────
// cache_load（双字段）
// ──────────────────────────────────────────────
bool cache_load(const char *type, int zIndex, String &out_v1, String &out_v2)
{
    DynamicJsonDocument doc(CACHE_DOC_CAPACITY);
    if (!read_cache_doc(doc)) return false;

    String key = make_key(type, zIndex);
    JsonObject entry = doc[key].as<JsonObject>();
    if (entry.isNull()) return false;

    const char *v1 = entry["v1"];
    const char *v2 = entry["v2"];
    if (!v1 || strlen(v1) == 0) return false;

    out_v1 = String(v1);
    out_v2 = (v2 && strlen(v2) > 0) ? String(v2) : String("");

#if DBG_TRMNL_SHOW
    Serial.printf("[CACHE] 缓存命中 [%s]: v1='%.40s'\n", key.c_str(), v1);
#endif
    return true;
}

// ──────────────────────────────────────────────
// cache_load（单字段）
// ──────────────────────────────────────────────
bool cache_load(const char *type, int zIndex, String &out_v1)
{
    DynamicJsonDocument doc(CACHE_DOC_CAPACITY);
    if (!read_cache_doc(doc)) return false;

    String key = make_key(type, zIndex);
    JsonObject entry = doc[key].as<JsonObject>();
    if (entry.isNull()) return false;

    const char *v1 = entry["v1"];
    if (!v1 || strlen(v1) == 0) return false;

    out_v1 = String(v1);

#if DBG_TRMNL_SHOW
    Serial.printf("[CACHE] 缓存命中 [%s]: v1='%.40s'\n", key.c_str(), v1);
#endif
    return true;
}
