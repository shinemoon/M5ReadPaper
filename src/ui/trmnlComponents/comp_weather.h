#pragma once
#include <M5Unified.h>
#include <ArduinoJson.h>

/**
 * @brief 通过高德天气 API 获取天气信息
 *
 * @param citycode          城市编码（如 "110000"）
 * @param apiKey            高德 API Key
 * @param out_today_info    输出：今日天气文本
 * @param out_tomorrow_info 输出：明日天气文本
 * @return true             获取成功
 */
bool fetch_weather(const String &citycode, const String &apiKey,
                   String &out_today_info, String &out_tomorrow_info);

/**
 * @brief 处理 weather 类型组件
 *
 * 调用 fetch_weather 获取数据，然后在画布上渲染今明两天天气。
 *
 * @param component ArduinoJSON 组件对象
 */
void render_weather_component(JsonObject component);
