#pragma once
#include <M5Unified.h>
#include <ArduinoJson.h>

/**
 * @brief 从今日诗词 API 获取诗词内容
 *
 * @param out_content   输出：诗词正文
 * @param out_origin    输出：出处（标题·朝代·作者）
 * @return true         获取成功
 */
bool fetch_daily_poem(String &out_content, String &out_origin);

/**
 * @brief 处理 daily_poem 类型组件
 *
 * 调用 fetch_daily_poem 获取数据，然后在画布上渲染。
 *
 * @param component ArduinoJSON 组件对象
 */
void render_daily_poem_component(JsonObject component);
