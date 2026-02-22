#pragma once
/**
 * @file webParser.h
 * @brief 轻量级流式 HTML 解析工具
 *
 * 提供从远程 URL 获取指定 HTML 属性定位元素文本内容的通用能力，
 * 采用流式读取、O(1) 内存，适合嵌入式 ESP32 环境。
 */

#include <M5Unified.h>

/**
 * @brief 从远程 URL 的响应 HTML 中，按任意属性定位元素并提取其文本内容
 *
 * 策略：
 *  1. 流式扫描响应流，在滑动窗口中匹配 attr_name="...{attr_value}..." 属性
 *  2. 支持任意属性名：class、id、data-*、style 等
 *  3. 通过 match_index 跳过前 N-1 个匹配，只提取第 N 个（1-based）
 *  4. 跳过开标签剩余部分（直到 '>'）
 *  5. 收集文本节点内容，直到目标元素的闭合标签为止
 *  6. 规范化：折叠连续空白、去除首尾空白
 *
 * @param url            目标 URL（支持 HTTPS）
 * @param attr_name      HTML 属性名（如 "class"、"id"、"data-type"）
 * @param attr_value     要匹配的属性值（精确匹配单个 token）
 * @param out_text       输出：提取到的文本内容（规范化后）
 * @param match_index    第几个匹配项（1-based，默认 1）
 * @param max_read_bytes 最大读取字节数，防止无限读取（默认 128KB）
 * @return true          成功找到并提取到文本
 * @return false         未找到、WiFi 未连接或 HTTP 请求失败
 */
bool web_fetch_text_by_attr(const char *url,
                            const char *attr_name,
                            const char *attr_value,
                            String &out_text,
                            int match_index = 1,
                            int max_read_bytes = 131072);

/**
 * @brief 向后兼容包装：按 CSS class 提取第 match_index 个匹配元素的文本
 */
bool web_fetch_text_by_class(const char *url,
                             const char *css_class,
                             String &out_text,
                             int match_index = 1,
                             int max_read_bytes = 131072);
