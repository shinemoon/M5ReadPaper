#pragma once
#include <M5Unified.h>
#include <ArduinoJson.h>

/**
 * @brief 从黄历网站抓取 div.yi-ji 文本（宜忌信息）
 *
 * @param out_title 输出：宜忌文本
 * @return true     获取成功
 */
bool fetch_huangli_title(String &out_title);

/**
 * @brief 处理 huangli 类型组件
 *
 * 调用 fetch_huangli_title 获取数据，然后在画布上渲染。
 *
 * @param component ArduinoJSON 组件对象
 */
void render_huangli_component(JsonObject component);
