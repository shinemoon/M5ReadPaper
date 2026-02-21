#pragma once
#include <M5Unified.h>
#include <ArduinoJson.h>

/**
 * @brief 从日历 API 获取今日节日与农历信息
 *
 * @param out_content   输出：日历文本（阳历+农历+节日，\n 分隔）
 * @param out_origin    输出：星座信息
 * @return true         获取成功
 */
bool fetch_day_info(String &out_content, String &out_origin);

/**
 * @brief 处理 day 类型组件（日历）
 *
 * 调用 fetch_day_info 获取数据，然后在画布上渲染。
 *
 * @param component ArduinoJSON 组件对象
 */
void render_day_component(JsonObject component);
