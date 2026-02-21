#include "webParser.h"
#include "test/per_file_debug.h"
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <string>
#include <cctype>
#include <algorithm>

extern bool g_wifi_sta_connected;

bool web_fetch_text_by_attr(const char *url,
                            const char *attr_name,
                            const char *attr_value,
                            String &out_text,
                            int match_index,
                            int max_read_bytes)
{
    if (!g_wifi_sta_connected)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[WebParser] WiFi 鏈繛鎺ワ紝璺宠繃: %s\n", url);
#endif
        return false;
    }

    if (!url || !attr_name || !attr_value ||
        strlen(url) == 0 || strlen(attr_name) == 0 || strlen(attr_value) == 0)
        return false;

    if (match_index < 1) match_index = 1;

#if DBG_TRMNL_SHOW
    Serial.printf("[WebParser] url=%s  %s='%s'  match#%d\n",
                  url, attr_name, attr_value, match_index);
#endif

    // 鈹€鈹€ HTTP 瀹㈡埛绔垵濮嬪寲 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.method            = HTTP_METHOD_GET;
    cfg.timeout_ms        = 10000;
    cfg.buffer_size       = 1024;
    cfg.buffer_size_tx    = 512;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200)
    {
#if DBG_TRMNL_SHOW
        Serial.printf("[WebParser] HTTP 鐘舵€佺爜: %d\n", status);
#endif
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    // TLS 鎻℃墜瀹屾垚鍚庣珛鍗抽噴鏀?cert bundle 鍗犵敤鐨勫爢鍐呭瓨
    esp_crt_bundle_detach(NULL);

    // 鈹€鈹€ 娴佸紡瑙ｆ瀽鐘舵€佹満 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    // 闃舵 SEARCHING   : 婊戝姩绐楀彛鍖归厤 attr_value锛岄獙璇佸叾灞炰簬 attr_name= 灞炴€?
    //                    閫氳繃 match_count 璁℃暟锛岃烦杩囧墠 match_index-1 涓尮閰?
    // 闃舵 SKIP_TO_GT  : 璺宠繃寮€鏍囩鍓╀綑鍐呭鐩村埌 '>'
    // 闃舵 COLLECT_TEXT: 鏀堕泦鏂囨湰鑺傜偣锛屽鐞嗗祵濂楁爣绛炬繁搴︼紝鐩村埌鐩爣鍏冪礌闂悎
    // 闃舵 DONE        : 缁撴潫

    enum class State { SEARCHING, SKIP_TO_GT, COLLECT_TEXT, DONE };
    State state = State::SEARCHING;

    const int needle_len   = (int)strlen(attr_value);
    const int attr_name_len = (int)strlen(attr_name);

    // 绐楀彛澶у皬锛氶渶瀹圭撼  attr_name + '=' + '"' + attr_value锛堝惈杈圭晫浣欓噺锛?
    const int WIN_SIZE = std::max(attr_name_len + needle_len + 16, 64);
    std::string window(WIN_SIZE, '\0');
    int win_pos  = 0;
    int win_fill = 0;

    int match_count = 0;   // 宸查獙璇侀€氳繃鐨勫尮閰嶆暟锛堝惈璺宠繃鐨勶級

    static const int TEXT_BUF_SIZE = 2048;
    char text_buf[TEXT_BUF_SIZE] = {};
    int  text_len = 0;
    int  depth    = 0;
    bool in_tag   = false;
    std::string tag_buf;

    char read_buf[1024];
    int  read_total = 0;
    bool found = false;

    while (state != State::DONE)
    {
        int r = esp_http_client_read(client, read_buf, sizeof(read_buf) - 1);
        if (r <= 0) break;
        read_buf[r] = '\0';
        read_total += r;

        for (int i = 0; i < r && state != State::DONE; ++i)
        {
            char c = read_buf[i];

            // 鈹€鈹€ SEARCHING 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
            if (state == State::SEARCHING)
            {
                window[win_pos] = c;
                win_pos = (win_pos + 1) % WIN_SIZE;
                if (win_fill < WIN_SIZE) ++win_fill;

                if (win_fill < needle_len) continue;

                // 妫€鏌ョ獥鍙ｆ湯灏炬槸鍚︿互 attr_value 缁撳熬
                bool match = true;
                for (int k = 0; k < needle_len; ++k)
                {
                    int idx = ((win_pos - needle_len + k) + WIN_SIZE) % WIN_SIZE;
                    if ((char)std::tolower((unsigned char)window[idx]) !=
                        (char)std::tolower((unsigned char)attr_value[k]))
                    {
                        match = false;
                        break;
                    }
                }
                if (!match) continue;

                // 楠岃瘉锛氬湪鍖归厤璧风偣涔嬪墠瀵绘壘 attr_name=锛屼腑闂村厑璁告湁寮曞彿/绌烘牸
                // 绐楀彛涓?attr_value 璧峰浣嶇疆锛堢嚎鎬х储寮曪級
                int startIdx = ((win_pos - needle_len) + WIN_SIZE) % WIN_SIZE;
                bool verified = false;

                // 浠?attr_value 璧峰浣嶇疆寰€鍓嶆渶澶氭壂鎻?attr_name_len+4 涓瓧绗?
                int max_back = std::min(win_fill - needle_len, attr_name_len + 4);
                for (int back = 1; back <= max_back; ++back)
                {
                    int pos = (startIdx - back + WIN_SIZE) % WIN_SIZE;
                    char wc = (char)std::tolower((unsigned char)window[pos]);

                    // 璺宠繃寮曞彿鍜岀┖鏍?
                    if (wc == '"' || wc == '\'' || wc == ' ') continue;

                    // 閬囧埌 '=' 鏃讹紝鍚戝墠鏍稿 attr_name
                    if (wc == '=')
                    {
                        if (back + attr_name_len > win_fill) break;
                        bool nameMatch = true;
                        for (int k = 0; k < attr_name_len; ++k)
                        {
                            int ni = (pos - attr_name_len + k + WIN_SIZE) % WIN_SIZE;
                            if ((char)std::tolower((unsigned char)window[ni]) !=
                                (char)std::tolower((unsigned char)attr_name[k]))
                            {
                                nameMatch = false;
                                break;
                            }
                        }
                        if (nameMatch) verified = true;
                        break;  // 鏃犺鏄惁鍖归厤閮藉彧鐪嬫渶杩戠殑 '='
                    }

                    // 閬囧埌鍏朵粬瀛楃锛堜笉鏄?= " ' 绌烘牸锛夎鏄?attr_value 鍜?= 涔嬮棿鏈夋棤鍏冲唴瀹?
                    break;
                }

                if (!verified) continue;

                ++match_count;
#if DBG_TRMNL_SHOW
                Serial.printf("[WebParser] 鎵惧埌绗?%d 涓尮閰?(鐩爣 %d)\n",
                              match_count, match_index);
#endif
                if (match_count < match_index)
                {
                    // 璺宠繃姝ゅ尮閰嶏紝缁х画鎵弿
                    continue;
                }

                // 鍒拌揪鐩爣鍖归厤锛岃浆鍏ユ敹闆嗛樁娈?
                state = State::SKIP_TO_GT;
            }
            // 鈹€鈹€ SKIP_TO_GT 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
            else if (state == State::SKIP_TO_GT)
            {
                if (c == '>') state = State::COLLECT_TEXT;
            }
            // 鈹€鈹€ COLLECT_TEXT 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
            else if (state == State::COLLECT_TEXT)
            {
                if (!in_tag)
                {
                    if (c == '<')
                    {
                        in_tag = true;
                        tag_buf.clear();
                        tag_buf.push_back(c);
                    }
                    else
                    {
                        if (text_len < TEXT_BUF_SIZE - 1)
                            text_buf[text_len++] = c;
                    }
                }
                else
                {
                    tag_buf.push_back(c);
                    if (c == '>')
                    {
                        bool is_end_tag  = (tag_buf.size() >= 2 && tag_buf[1] == '/');
                        bool is_decl     = (tag_buf.size() >= 2 && tag_buf[1] == '!');
                        bool self_close  = (tag_buf.size() >= 2 && tag_buf[tag_buf.size()-2] == '/');

                        if (!is_decl)
                        {
                            if (is_end_tag)
                            {
                                // 鎻愬彇缁撴潫鏍囩鍚?
                                size_t p = 2;
                                std::string name;
                                while (p < tag_buf.size() && std::isalpha((unsigned char)tag_buf[p]))
                                    name.push_back((char)std::tolower((unsigned char)tag_buf[p++]));

                                if (depth == 0)
                                {
                                    // 鐩爣鍏冪礌闂悎锛屾敹闆嗙粨鏉?
                                    state = State::DONE;
                                    found = true;
                                }
                                else
                                    --depth;
                            }
                            else if (!self_close)
                            {
                                ++depth; // 瀛愬厓绱犲紑鏍囩锛屾繁搴?1
                            }
                        }
                        in_tag = false;
                        tag_buf.clear();
                    }
                }
            }
        } // for each byte

        if (read_total >= max_read_bytes) break;
    }
    text_buf[text_len] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

#if DBG_TRMNL_SHOW
    Serial.printf("[WebParser] 鎬昏鍙? %d 瀛楄妭, found=%d, text_len=%d\n",
                  read_total, (int)found, text_len);
#endif

    if (!found || text_len == 0) return false;

    // 鈹€鈹€ 瑙勮寖鍖栵細鎶樺彔杩炵画绌虹櫧锛屽幓闄ら灏?鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    std::string raw(text_buf, text_len);
    std::string norm;
    norm.reserve(raw.size());
    bool last_space = false;
    for (char ch : raw)
    {
        if (std::isspace((unsigned char)ch))
        {
            if (!last_space) { norm.push_back(' '); last_space = true; }
        }
        else { norm.push_back(ch); last_space = false; }
    }
    size_t s = 0;
    while (s < norm.size() && norm[s] == ' ') ++s;
    size_t e = norm.size();
    while (e > s && norm[e-1] == ' ') --e;
    std::string trimmed = (s >= e) ? "" : norm.substr(s, e - s);

    out_text = String(trimmed.c_str());

#if DBG_TRMNL_SHOW
    Serial.printf("[WebParser] 鎻愬彇缁撴灉: %s\n", trimmed.c_str());
#endif

    return out_text.length() > 0;
}

// 鈹€鈹€ 鍚戝悗鍏煎鍖呰 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
bool web_fetch_text_by_class(const char *url,
                             const char *css_class,
                             String &out_text,
                             int match_index,
                             int max_read_bytes)
{
    return web_fetch_text_by_attr(url, "class", css_class,
                                  out_text, match_index, max_read_bytes);
}

