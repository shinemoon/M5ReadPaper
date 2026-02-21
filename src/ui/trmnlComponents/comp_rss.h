#pragma once
#include <M5Unified.h>
#include <ArduinoJson.h>

/**
 * @brief 流式获取 RSS/Atom feed 并提取标题列表
 *
 * @param url        RSS 源 URL
 * @param out_titles 输出：分号分隔的标题列表
 * @param max_items  最多提取的条目数（默认 10）
 * @return true      至少获取到 1 条标题
 */
bool fetch_rss_feed(const String &url, String &out_titles, int max_items = 10);

/**
 * @brief 处理 rss 类型组件
 *
 * 调用 fetch_rss_feed 获取数据，然后通过 render_list_items 渲染。
 *
 * @param component ArduinoJSON 组件对象
 */
void render_rss_component(JsonObject component);
