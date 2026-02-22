#pragma once
#include <M5Unified.h>
#include <ArduinoJson.h>

/**
 * @brief 从 ONE 一言 API 获取一言内容
 *
 * @param out_content   输出：一言正文
 * @param out_origin    输出：来源（tag·name·origin）
 * @return true         获取成功
 */
bool fetch_one_sentence(String &out_content, String &out_origin);

/**
 * @brief 处理 one 类型组件（ONE 一言）
 *
 * 调用 fetch_one_sentence 获取数据，然后在画布上渲染。
 *
 * @param component ArduinoJSON 组件对象
 */
void render_one_component(JsonObject component);
