#pragma once
#include <M5Unified.h>
#include <ArduinoJson.h>

/**
 * @brief 渲染列表项（分号分隔的文本，带 bullet 点）
 *
 * @param content       分号分隔的文本内容
 * @param x             起始 x 坐标（像素）
 * @param y             起始 y 坐标（像素）
 * @param area_width    可用宽度
 * @param area_height   可用高度
 * @param fontSize      字体大小
 * @param textColor     文本颜色（0-15 灰度）
 * @param margin        行间距（像素）
 * @return int          实际渲染的项数
 */
int render_list_items(const char *content,
                      int16_t x, int16_t y,
                      int16_t area_width, int16_t area_height,
                      uint8_t fontSize, uint8_t textColor,
                      int16_t margin);

/**
 * @brief 处理 list 类型组件（静态列表）
 *
 * 从 component JSON 对象中读取 position/size/config，
 * 调用 render_list_items 渲染分号分隔的列表内容。
 *
 * @param component ArduinoJSON 组件对象
 */
void render_list_component(JsonObject component);
