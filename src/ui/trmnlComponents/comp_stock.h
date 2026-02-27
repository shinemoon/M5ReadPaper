#pragma once
#include <M5Unified.h>
#include <ArduinoJson.h>

/**
 * @brief 处理 stock 类型组件（股票代码列表，首行留空，从第二行开始每行显示一个代码）
 *
 * @param component ArduinoJSON 组件对象
 */
void render_stock_component(JsonObject component);
