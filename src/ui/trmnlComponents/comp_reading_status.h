#pragma once
#include <M5Unified.h>
#include <ArduinoJson.h>

/**
 * @brief 处理 reading_status 类型组件
 *
 * 从全局 g_current_book 读取当前书名/章节/进度，
 * 然后在画布上渲染。
 *
 * @param component ArduinoJSON 组件对象
 */
void render_reading_status_component(JsonObject component);
