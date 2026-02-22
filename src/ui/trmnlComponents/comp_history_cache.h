#pragma once
#include <Arduino.h>

/**
 * @file comp_history_cache.h
 * @brief 动态组件历史数据缓存
 *
 * 将各动态 trmnl 组件上一次成功的 fetch 结果持久化到
 * SD 卡 /rdt/history.cache（JSON 格式）。
 * 当组件因网络或其他原因 fetch 失败时，可回退到缓存数据继续渲染。
 *
 * 缓存格式示例：
 * {
 *   "daily_poem_1": { "v1": "诗词正文", "v2": "出处" },
 *   "weather_2":    { "v1": "今天天气", "v2": "明天天气" },
 *   "rss_3":        { "v1": "标题1;标题2;标题3" }
 * }
 *
 * 查找键由组件 type + "_" + zIndex 组成。
 * 每次 RDT 文件更新（WebDAV 下载或热点重写）后需调用
 * cache_clear_history() 清空缓存，以保证数据与布局同步。
 */

/**
 * @brief 删除 /rdt/history.cache，清空所有组件缓存
 *
 * 应在 RDT 文件被更新（WebDAV 下载或热点 commit）后调用。
 */
void cache_clear_history();

/**
 * @brief 将组件的 fetch 结果写入缓存（双字段）
 *
 * @param type   组件 type 字段（如 "daily_poem"、"weather"）
 * @param zIndex 组件 zIndex（作为区分同类多实例的辅助键）
 * @param v1     第一个数据字段
 * @param v2     第二个数据字段（传 nullptr 则不写入 v2）
 */
void cache_save(const char *type, int zIndex, const char *v1, const char *v2 = nullptr);

/**
 * @brief 从缓存读取双字段数据
 *
 * @param type     组件 type
 * @param zIndex   组件 zIndex
 * @param out_v1   输出：第一个数据字段
 * @param out_v2   输出：第二个数据字段
 * @return true    缓存命中且两个字段均非空
 */
bool cache_load(const char *type, int zIndex, String &out_v1, String &out_v2);

/**
 * @brief 从缓存读取单字段数据
 *
 * @param type     组件 type
 * @param zIndex   组件 zIndex
 * @param out_v1   输出：第一个数据字段
 * @return true    缓存命中且字段非空
 */
bool cache_load(const char *type, int zIndex, String &out_v1);
