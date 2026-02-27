#include "comp_stock.h"
#include "comp_history_cache.h"
#include "readpaper.h"
#include "globals.h"
#include "text/bin_font_print.h"
#include "test/per_file_debug.h"

#include <vector>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <ArduinoJson.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <string>
#include <strings.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <time.h>
//#include <M5GFX.h>

extern M5Canvas *g_canvas;
extern bool g_wifi_sta_connected;

// 渲染股票代码列表（首行空行，从第二行开始显示每个代码）
int render_stock_items(
    const char *content,
    int16_t x, int16_t y,
    int16_t area_width, int16_t area_height,
    uint8_t fontSize, uint8_t textColor,
    int16_t margin,
    const char *comp_type = "stock",
    int comp_zindex = 0)
{
    if (!content || strlen(content) == 0)
        return 0;

    // 计算行高
    uint8_t base_font_size = get_font_size_from_file();
    if (base_font_size == 0)
        base_font_size = 24;
    float scale_factor = (fontSize > 0) ? ((float)fontSize / (float)base_font_size) : 1.0f;
    int16_t line_height = (int16_t)(base_font_size * scale_factor) + margin;

    int max_lines = area_height / line_height;
    if (max_lines <= 0)
        max_lines = 1;

#if DBG_TRMNL_SHOW
    Serial.printf("[STOCK] 股票渲染: area_height=%d, fontSize=%d, base=%d, 行高=%d, margin=%d, 最大行数=%d\n",
                  area_height, fontSize, base_font_size, line_height, margin, max_lines);
#endif

    String textStr = String(content);
    // 分号分割，最多16个
    std::vector<String> codes;
    int start_pos = 0;
    while ((int)codes.size() < 16)
    {
        int semicolon_pos = textStr.indexOf(';', start_pos);
        String item;
        if (semicolon_pos == -1)
        {
            item = textStr.substring(start_pos);
        }
        else
        {
            item = textStr.substring(start_pos, semicolon_pos);
            start_pos = semicolon_pos + 1;
        }
        item.trim();
        if (item.length() > 0)
            codes.push_back(item);
        if (semicolon_pos == -1)
            break;
    }

    int available_lines = max_lines - 1; // 首行留空
    if (available_lines <= 0)
        return 0;

    int displayCount = std::min((int)codes.size(), available_lines);
    if (displayCount <= 0)
        return 0;

    // 构造请求：使用 ulist.np/get 并通过 secids 精确查询用户输入的代码
    // 我们只需要部分字段用于展示：f12=代码, f14=名称, f18=开盘价, f2=最新价, f4=涨跌额, f3=涨跌幅
    const char *fields = "f12,f14,f18,f2,f4,f3";

    // 将用户输入 codes 转换为 secid 格式（6 开头视为上交所 -> prefix 1., 其他为深交所 -> prefix 0.）
    String secids;
    for (size_t i = 0; i < codes.size(); ++i)
    {
        String c = codes[i];
        c.trim();
        if (c.length() == 0)
            continue;
        // 取首字符判断交易所
        char first = c.charAt(0);
        String sid = (first == '6') ? String("1.") + c : String("0.") + c;
        if (secids.length() > 0)
            secids += ",";
        secids += sid;
    }

    // 始终请求上证指数 (secid=1.000001)，如果未包含则追加
    if (secids.indexOf("1.000001") == -1)
    {
        if (secids.length() > 0)
            secids += ",";
        secids += String("1.000001");
    }

    if (secids.length() == 0)
    {
        Serial.println("[STOCK] 没有有效的股票代码，跳过请求");
        return 0;
    }

    String params = String("/api/qt/ulist.np/get?fltt=2&fields=") + String(fields) + String("&secids=") + secids + String("&_=") + String((uint32_t)millis());

    if (!g_wifi_sta_connected)
    {
        Serial.println("[STOCK] WiFi 未连接，尝试读取历史缓存");
        String cached_v1, cached_v2;
        if (cache_load(comp_type, comp_zindex, cached_v1, cached_v2))
        {
            // 显示缓存时间（若有）并在同一高度显示缓存中的首行（指数信息）
            String display_time = cached_v2.length() ? cached_v2 : String("");
            String cached_index_line = "";
            int start = 0;
            int nl = cached_v1.indexOf('\n', start);
            if (nl == -1)
            {
                // 整个缓存只有一行
                cached_index_line = cached_v1;
                start = cached_v1.length();
            }
            else if (nl > 0)
            {
                cached_index_line = cached_v1.substring(0, nl);
                start = nl + 1;
            }

            if (cached_index_line.length() > 0)
            {
                bin_font_print(
                    cached_index_line.c_str(),
                    fontSize * 0.8f,
                    textColor,
                    area_width,
                    x,
                    y,
                    false,
                    g_canvas,
                    TEXT_ALIGN_LEFT,
                    area_width,
                    false);
            }

            if (display_time.length() > 0)
            {
                bin_font_print(
                    display_time.c_str(),
                    fontSize * 0.8f,
                    textColor,
                    area_width,
                    x,
                    y,
                    false,
                    g_canvas,
                    TEXT_ALIGN_RIGHT,
                    area_width,
                    false);
            }

            int16_t current_y_cached = y + line_height;
            int printed = 0;
            while (start < (int)cached_v1.length() && printed < available_lines)
            {
                int nl2 = cached_v1.indexOf('\n', start);
                String line;
                if (nl2 == -1)
                {
                    line = cached_v1.substring(start);
                    start = cached_v1.length();
                }
                else
                {
                    line = cached_v1.substring(start, nl2);
                    start = nl2 + 1;
                }
                line.trim();
                if (line.length() == 0)
                    continue;
                bin_font_print(
                    line.c_str(),
                    fontSize,
                    textColor,
                    area_width,
                    x,
                    current_y_cached - fontSize / 2,
                    false,
                    g_canvas,
                    TEXT_ALIGN_LEFT,
                    area_width,
                    false);
                current_y_cached += line_height;
                printed++;
            }
            return printed;
        }
        else
        {
            Serial.println("[STOCK] 未命中缓存，跳过获取");
            return 0;
        }
    }

    // 先做 DNS 解析，避免 esp_http_client 在内置解析时出现问题
    const char *hostname = "push2.eastmoney.com";
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(hostname, "443", &hints, &res);
    if (gai != 0 || res == NULL)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[STOCK] DNS 解析失败: %d\n", gai);
#endif
        return 0;
    }

    static char ip_str[INET_ADDRSTRLEN];
    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &(addr->sin_addr), ip_str, INET_ADDRSTRLEN);
    freeaddrinfo(res);

    Serial.printf("[STOCK] DNS 解析: %s -> %s\n", hostname, ip_str);

    esp_http_client_config_t cfg = {};
    memset(&cfg, 0, sizeof(cfg));
    cfg.host = ip_str;
    cfg.port = 443;
    cfg.path = params.c_str();
    cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;
    cfg.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    String body;
    String api_time_raw = "";
    Serial.println("[STOCK] 开始从东方财富获取行情数据...");
    if (client)
    {
        esp_http_client_set_header(client, "User-Agent", "ReadPaper-Stock");
        esp_err_t err = esp_http_client_open(client, 0);
        if (err == ESP_OK)
        {
            int content_length = esp_http_client_fetch_headers(client);
            // 尝试从响应头获取 Date 字段
            char *date_hdr = nullptr;
            if (esp_http_client_get_header(client, "Date", &date_hdr) == ESP_OK && date_hdr)
            {
                api_time_raw = String(date_hdr);
            }
            else if (esp_http_client_get_header(client, "date", &date_hdr) == ESP_OK && date_hdr)
            {
                api_time_raw = String(date_hdr);
            }
            if (content_length > 0)
                body.reserve(content_length + 8);

            const int BUF_SIZE = 1024;
            char *buf = (char *)malloc(BUF_SIZE + 1);
            if (buf)
            {
                int read_len = 0;
                while ((read_len = esp_http_client_read_response(client, buf, BUF_SIZE)) > 0)
                {
                    buf[read_len] = '\0';
                    body += String(buf, read_len);
                }
                free(buf);
            }
        }
        else
        {
            Serial.printf("[STOCK] esp_http_client_open failed: %d\n", err);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    else
    {
        Serial.println("[STOCK] esp_http_client_init returned null");
    }

    Serial.printf("[STOCK] 首次请求返回长度: %d\n", (int)body.length());

    // 如果 HTTPS 请求没有返回内容，尝试 HTTP 回退以便诊断（有时TLS或时间不同步会失败）
    if (body.length() == 0)
    {
        Serial.println("[STOCK] 首次HTTPS请求无响应，尝试HTTP回退...");
        String http_url = String("http://push2.eastmoney.com") + params;
        esp_http_client_config_t cfg2 = {};
        cfg2.url = http_url.c_str();
        cfg2.method = HTTP_METHOD_GET;
        cfg2.timeout_ms = 8000;
        cfg2.buffer_size = 2048;
        cfg2.buffer_size_tx = 1024;
        esp_http_client_handle_t client2 = esp_http_client_init(&cfg2);
        if (client2)
        {
            esp_http_client_set_header(client2, "User-Agent", "ReadPaper-Stock-HTTP-Fallback");
            esp_err_t err2 = esp_http_client_open(client2, 0);
            if (err2 == ESP_OK)
            {
                int content_length = esp_http_client_fetch_headers(client2);
                // fetch Date header from HTTP fallback response as well
                char *date_hdr2 = nullptr;
                if (esp_http_client_get_header(client2, "Date", &date_hdr2) == ESP_OK && date_hdr2)
                {
                    api_time_raw = String(date_hdr2);
                }
                else if (esp_http_client_get_header(client2, "date", &date_hdr2) == ESP_OK && date_hdr2)
                {
                    api_time_raw = String(date_hdr2);
                }
                if (content_length > 0)
                    body.reserve(content_length + 8);
                const int BUF_SIZE = 1024;
                char *buf = (char *)malloc(BUF_SIZE + 1);
                if (buf)
                {
                    int read_len = 0;
                    while ((read_len = esp_http_client_read_response(client2, buf, BUF_SIZE)) > 0)
                    {
                        buf[read_len] = '\0';
                        body += String(buf, read_len);
                    }
                    free(buf);
                }
            }
            else
            {
                Serial.printf("[STOCK] HTTP 回退 esp_http_client_open failed: %d\n", err2);
            }
            esp_http_client_close(client2);
            esp_http_client_cleanup(client2);
        }
        else
        {
            Serial.println("[STOCK] HTTP 回退 esp_http_client_init 返回 null");
        }
        Serial.printf("[STOCK] 回退请求返回长度: %d\n", (int)body.length());
    }

    // 东财有时返回 jQuery(...); 包装，尝试提取 JSON 对象
    int idx = body.indexOf('{');
    if (idx >= 0)
    {
        body = body.substring(idx);
    }

    // 解析 JSON 并只提取所需字段到轻量结构，避免保留大型 DOM
    struct StockInfo
    {
        String code;
        String name;
        float open;
        float price;
        float pct;
        float chg;
    };
    std::vector<StockInfo> stocks;
    // 解析使用较保守的内存
    DynamicJsonDocument doc(32 * 1024);
    DeserializationError derr = deserializeJson(doc, body);
    if (!derr)
    {
        // 支持多种可能的响应结构：data.diff, data.data, data(数组)
        JsonVariant dataVar = doc["data"];
        JsonArray diffArr;
        if (dataVar.is<JsonObject>())
        {
            JsonObject dobj = dataVar.as<JsonObject>();
            if (dobj["diff"].is<JsonArray>())
                diffArr = dobj["diff"].as<JsonArray>();
            else if (dobj["data"].is<JsonArray>())
                diffArr = dobj["data"].as<JsonArray>();
        }
        else if (dataVar.is<JsonArray>())
        {
            diffArr = dataVar.as<JsonArray>();
        }

        if (!diffArr.isNull())
        {
            // robust numeric extractor: handles numeric types or numeric-like strings
            auto getNum = [](JsonObject &obj, const char *key) -> double
            {
                JsonVariant v = obj[key];
                if (v.is<double>() || v.is<int>())
                {
                    return (double)v.as<double>();
                }
                const char *s = v.as<const char *>();
                if (!s)
                    return 0.0;
                std::string str(s);
                std::string filtered;
                filtered.reserve(str.size());
                for (char ch : str)
                {
                    if ((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.' || ch == 'e' || ch == 'E')
                        filtered.push_back(ch);
                }
                if (filtered.empty())
                    return 0.0;
                return atof(filtered.c_str());
            };

            for (JsonVariant v : diffArr)
            {
                JsonObject item = v.as<JsonObject>();
                const char *codeField = item["f12"] | "";
                if (codeField && strlen(codeField) > 0)
                {
                    StockInfo si;
                    si.code = String(codeField);
                    // 规范化 code：去掉可能的 ".SZ/.SH" 后缀
                    int dotp = si.code.indexOf('.');
                    if (dotp > 0)
                        si.code = si.code.substring(0, dotp);
                    const char *n = item["f14"] | "";
                    si.name = String(n);
                    si.open = (float)getNum(item, "f18");
                    si.price = (float)getNum(item, "f2");
                    si.chg = (float)getNum(item, "f4");
                    si.pct = (float)getNum(item, "f3");
                    stocks.push_back(si);
                }
            }
        }
    }
    else
    {
        Serial.printf("[STOCK] JSON 解析错误: %s\n", derr.c_str());
    }
    // 解析完成，打印已解析条数
    Serial.printf("[STOCK] 解析到 %d 条股票记录\n", (int)stocks.size());

    // 尝试从响应中提取时间字段（多种可能的键）
    if (!derr)
    {
        if (doc.containsKey("data"))
        {
            JsonVariant d = doc["data"];
            if (d.is<JsonObject>())
            {
                JsonObject dobj = d.as<JsonObject>();
                if (dobj.containsKey("timestamp"))
                    api_time_raw = String(dobj["timestamp"].as<const char *>());
                else if (dobj.containsKey("time"))
                    api_time_raw = String(dobj["time"].as<const char *>());
                else if (dobj.containsKey("update_time"))
                    api_time_raw = String(dobj["update_time"].as<const char *>());
            }
        }
        if (api_time_raw.length() == 0)
        {
            if (doc.containsKey("timestamp"))
                api_time_raw = String(doc["timestamp"].as<const char *>());
            else if (doc.containsKey("rt"))
            {
                long rtval = doc["rt"].as<long>();
                // only treat as unix timestamp if it's large enough (avoid small status codes like 11)
                if (rtval >= 1000000000L)
                    api_time_raw = String(rtval);
            }
        }
    }

    Serial.printf("[STOCK] api_time_raw(from header/json)='%s'\n", api_time_raw.c_str());

    // 格式化时间为 YYYY-MM-DD HH:MM，如果上游返回的是数字时间戳则尝试转换
    String api_time_fmt = "";
    if (api_time_raw.length() > 0)
    {
        String s = api_time_raw;
        s.trim();
        // 尝试解析 RFC1123 格式: Fri, 27 Feb 2026 12:45:58 GMT
        int day = 0, year = 0, hour = 0, min = 0, sec = 0;
        char wk[16] = {0}, mon[16] = {0};
        int matched = sscanf(s.c_str(), "%15[^,], %d %3s %d %d:%d:%d", wk, &day, mon, &year, &hour, &min, &sec);
        if (matched >= 7)
        {
            // month string -> month number
            const char *mons[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            int mnum = 0;
            for (int mi = 0; mi < 12; ++mi)
            {
                if (strcasecmp(mon, mons[mi]) == 0)
                {
                    mnum = mi + 1;
                    break;
                }
            }
            if (mnum > 0)
            {
                char tbuf[64] = {0};
                snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d", year, mnum, day, hour, min);
                api_time_fmt = String(tbuf);
            }
            else
            {
                api_time_fmt = s;
            }
        }
        else
        {
            // 若是纯数字时间戳
            bool all_digits = true;
            for (size_t i = 0; i < s.length(); ++i)
            {
                char ch = s.charAt(i);
                if (ch < '0' || ch > '9')
                {
                    all_digits = false;
                    break;
                }
            }
            if (all_digits)
            {
                unsigned long tse = 0;
                if (s.length() > 10)
                    tse = (unsigned long)(s.toInt() / 1000);
                else
                    tse = (unsigned long)s.toInt();
                time_t t = (time_t)tse;
                struct tm *tmv = localtime(&t);
                char tbuf[64] = {0};
                if (tmv)
                    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", tmv);
                api_time_fmt = String(tbuf);
            }
            else
            {
                // 其它格式直接使用原始字段
                api_time_fmt = s;
            }
        }
    }

    // 若仍无时间信息，使用本地时间作为回退
    if (api_time_fmt.length() == 0)
    {
        time_t now = time(NULL);
        struct tm *tmv = localtime(&now);
        char tbuf[64] = {0};
        if (tmv)
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", tmv);
        api_time_fmt = String(tbuf);
    }

    Serial.printf("[STOCK] api_time_fmt(after parse/fallback)='%s'\n", api_time_fmt.c_str());

    // 找到上证指数（000001）的涨跌幅并把指数信息作为缓存首行
    String index_line = "";
    for (auto &si : stocks)
    {
        if (si.code == String("000001"))
        {
            char ibuf[64];
            snprintf(ibuf, sizeof(ibuf), "%s %+.2f%%", si.name.c_str(), si.pct);
            index_line = String(ibuf);
            break;
        }
    }

    // 在渲染前，将股票数据序列化为供缓存使用的文本（每行一条），并保存到 history.cache
    String cache_v1 = "";
    if (index_line.length() > 0)
        cache_v1 += index_line;
    for (int i = 0; i < displayCount; i++)
    {
        String inCode = codes[i];
        int dot = inCode.indexOf('.');
        String bare = (dot > 0) ? inCode.substring(0, dot) : inCode;
        bare.trim();
        bool found = false;
        StockInfo matchedSi;
        for (auto &si : stocks)
        {
            if (si.code == bare)
            {
                matchedSi = si;
                found = true;
                break;
            }
        }
        String line;
        if (!found)
        {
            line = inCode;
        }
        else
        {
            line = matchedSi.name + " ";
            line += String(matchedSi.price, 2);
            line += " ";
            line += String(matchedSi.chg, 2);
            line += " ";
            line += String(matchedSi.pct, 2);
            line += "%";
        }
        if (cache_v1.length() > 0)
            cache_v1 += "\n";
        cache_v1 += line;
    }

    // 保存缓存：v1为内容（首行为指数），v2为时间
    cache_save("stock", comp_zindex, cache_v1.c_str(), api_time_fmt.c_str());

    // 清理大对象释放内存
    body = String();
    doc.clear();

    // 名称左对齐：先打印上证指数（若有）在左侧，同一高度打印时间在右侧
    if (index_line.length() > 0)
    {
        bin_font_print(
            index_line.c_str(),
            fontSize,
            textColor,
            area_width,
            x,
            y,
            false,
            g_canvas,
            TEXT_ALIGN_LEFT,
            area_width,
            false);
    }
    bin_font_print(
        api_time_fmt.c_str(),
        fontSize * 0.7f,
        textColor,
        area_width,
        x,
        y+fontSize*0.2f,
        false,
        g_canvas,
        TEXT_ALIGN_RIGHT,
        area_width,
        false);

    g_canvas->drawWideLine(x, y + line_height, x + area_width, y + line_height, 2.0f, TFT_BLACK);
    int16_t current_y = y + 30 + line_height; // 从第二行开始 (首行留空)
    for (int i = 0; i < displayCount; i++)
    {
        String inCode = codes[i];
        // 去掉后缀 .SZ/.SH
        int dot = inCode.indexOf('.');
        String bare = (dot > 0) ? inCode.substring(0, dot) : inCode;
        bare.trim();

#if DBG_TRMNL_SHOW
        Serial.printf("[STOCK] 请求结果匹配查找: %s\n", bare.c_str());
#endif

        String lineText;
        bool found = false;
        // 寻找匹配的股票记录并保存匹配项
        StockInfo matchedSi;
        for (auto &si : stocks)
        {
            if (si.code == bare)
            {
                matchedSi = si;
                found = true;
                break;
            }
        }
        if (!found)
        {
            // 若未找到则仅打印代码, 使用整行宽度左对齐
            bin_font_print(
                inCode.c_str(),
                fontSize,
                textColor,
                area_width,
                x,
                current_y - fontSize / 2,
                false,
                g_canvas,
                TEXT_ALIGN_LEFT,
                area_width,
                false);
        }
        else
        {
            // 按列分别打印：Name | Open | Price | Change | Pct
            int total_w = area_width;
            // 名称占比约 45%，其余字段平均分配
            int name_w = (int)(total_w * 0.35f);
            int num_w = (total_w - name_w) / 3;
            /*
            if (num_w < 20)
            {
                num_w = (total_w - name_w) / 4;
            }
                */

            int col0 = x;
            int col1 = col0 + name_w;
            int col2 = col1 + num_w;
            int col3 = col2 + num_w;
            //            int col4 = col3 + num_w;

            //           Serial.printf("[STOCK] num_w=%d, col1=%d, col2=%d, col3=%d, col4=%d\n", num_w, col1, col2, col3, col4);
            Serial.printf("[STOCK] num_w=%d, col1=%d, col2=%d, col3=%d ", num_w, col1, col2, col3);

            // 格式化数值为字符串
            char buf_open[32];
            char buf_price[32];
            char buf_chg[32];
            char buf_pct[32];
            snprintf(buf_open, sizeof(buf_open), "%.2f", matchedSi.open);
            snprintf(buf_price, sizeof(buf_price), "%.2f", matchedSi.price);
            snprintf(buf_chg, sizeof(buf_chg), "%+.2f", matchedSi.chg);
            snprintf(buf_pct, sizeof(buf_pct), "%+.2f%%", matchedSi.pct);

            Serial.printf("[STOCK] buf_open=%s, buf_price=%s, buf_chg=%s, buf_pct=%s\n", buf_open, buf_price, buf_chg, buf_pct);

            // 名称左对齐
            bin_font_print(
                matchedSi.name.c_str(),
                fontSize,
                textColor,
                name_w,
                col0,
                current_y - fontSize / 2,
                false,
                g_canvas,
                TEXT_ALIGN_LEFT,
                name_w,
                false);

            /*
            bin_font_print(
                buf_open,
                fontSize * 0.8f,
                textColor,
                num_w,
                col1,
                current_y - fontSize / 2,
                false,
                g_canvas,
                TEXT_ALIGN_LEFT,
                num_w,
                false);
            */
            bin_font_print(
                buf_price,
                fontSize * 0.8f,
                textColor,
                num_w,
                col1,
                current_y - fontSize / 2,
                false,
                g_canvas,
                TEXT_ALIGN_LEFT,
                num_w,
                false);

            bin_font_print(
                buf_chg,
                fontSize * 0.8f,
                textColor,
                num_w,
                col2,
                current_y - fontSize / 2,
                false,
                g_canvas,
                TEXT_ALIGN_LEFT,
                num_w,
                false);

            bin_font_print(
                buf_pct,
                fontSize * 0.8f,
                textColor,
                num_w,
                col3,
                current_y - fontSize / 2,
                false,
                g_canvas,
                TEXT_ALIGN_LEFT,
                num_w,
                false);
        }

        current_y += line_height;
    }

    return displayCount;
}

// 处理 stock 类型组件
void render_stock_component(JsonObject component)
{
    int pos_x = 0, pos_y = 0;
    if (component.containsKey("position"))
    {
        JsonObject position = component["position"].as<JsonObject>();
        pos_x = position["x"] | 0;
        pos_y = position["y"] | 0;
    }
    JsonObject areaSize = component["size"].as<JsonObject>();
    float cell_w = areaSize["width"].as<float>();
    if (cell_w < 0.01f)
        cell_w = 1.0f;
    float cell_h = areaSize["height"].as<float>();
    if (cell_h < 0.01f)
        cell_h = 1.0f;

    const char *text = "";
    int fontSize = 24, textColor = 0, xOffset = 0, yOffset = 0, margin = 12;
    if (component.containsKey("config"))
    {
        JsonObject config = component["config"].as<JsonObject>();
        text = config["text"] | "";
        fontSize = config["fontSize"] | 24;
        textColor = config["textColor"] | 0;
        xOffset = config["xOffset"] | 0;
        yOffset = config["yOffset"] | 0;
        // 支持 margin 可选配置
        margin = config["margin"] | 6;
    }

    const int CELL_WIDTH = 60, CELL_HEIGHT = 60;
    int16_t x = pos_x * CELL_WIDTH + 20 + xOffset;
    int16_t y = pos_y * CELL_HEIGHT + yOffset;
    int16_t a_w = (int)(cell_w * CELL_WIDTH) - 40;
    int16_t a_h = (int)(cell_h * CELL_HEIGHT);

#if DBG_TRMNL_SHOW
    Serial.printf("[STOCK] 渲染股票组件: 单元格(%d,%d) 像素(%d,%d) 字号%d 颜色%d 宽度%d 高度%d\n",
                  pos_x, pos_y, x, y, fontSize, textColor, a_w, a_h);
#endif

    const char *comp_type = component["type"] | "stock";
    int comp_zindex = component["zIndex"] | 0;
    render_stock_items(text, x, y, a_w, a_h, (uint8_t)fontSize, (uint8_t)textColor, (int16_t)margin, comp_type, comp_zindex);
}
