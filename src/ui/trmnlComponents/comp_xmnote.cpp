#include "comp_xmnote.h"
#include "readpaper.h"
#include "device/ui_display.h"
#include "test/per_file_debug.h"
#include "SD/SDWrapper.h"
#include "text/line_handle.h"
#include "ui/ui_canvas_image.h"
#include <vector>
#include <string>

extern M5Canvas *g_canvas;
extern GlobalConfig g_config;

// Helper: read file content (up to limit) into String
static bool read_file_to_string(const char *path, String &out, size_t max_len = 8192)
{
    if (!SDW::SD.exists(path))
        return false;
    File f = SDW::SD.open(path, "r");
    if (!f)
        return false;
    out.clear();

    // Try to read full file when possible. Query file size to adjust limits.
    size_t file_size = 0;
#if defined(ESP32)
    file_size = f.size();
#endif

    size_t to_read = max_len;
    const size_t HARD_CAP = 131072; // 128KB
    if (file_size > 0)
    {
        if (file_size > to_read)
        {
            to_read = file_size;
            if (to_read > HARD_CAP)
                to_read = HARD_CAP;
        }
    }

    size_t read = 0;
    while (f.available() && read < to_read)
    {
        char c = f.read();
        out += c;
        ++read;
    }
    f.close();

    Serial.printf("[XMNOTE] read_file_to_string '%s' file_size=%u read=%u (cap=%u)\n", path, (unsigned)file_size, (unsigned)read, (unsigned)to_read);
    return out.length() > 0;
}

// Stream-read a JSON array file and pick a random object from the top-level array using reservoir sampling.
static bool pick_random_object_from_array(const char *path, String &out_obj)
{
    if (!SDW::SD.exists(path))
        return false;
    File f = SDW::SD.open(path, "r");
    if (!f)
        return false;

    // Skip leading whitespace until '['
    bool found_array = false;
    while (f.available())
    {
        char c = f.read();
        if (c == '[')
        {
            found_array = true;
            break;
        }
        if (!isspace((unsigned char)c))
            break; // not array
    }
    if (!found_array)
    {
        f.close();
        return false;
    }

    unsigned long count = 0;
    bool in_object = false;
    bool in_string = false;
    bool escape = false;
    int depth = 0;
    String cur;
    randomSeed(millis());
    while (f.available())
    {
        char c = f.read();
        if (!in_object)
        {
            if (c == '{')
            {
                in_object = true;
                depth = 1;
                in_string = false;
                escape = false;
                cur = "{";
            }
            else if (c == ']')
            {
                break; // end of array
            }
            else
            {
                // skip (commas/whitespace)
                continue;
            }
        }
        else
        {
            cur += c;
            if (in_string)
            {
                if (escape)
                    escape = false;
                else if (c == '\\')
                    escape = true;
                else if (c == '"')
                    in_string = false;
            }
            else
            {
                if (c == '"')
                    in_string = true;
                else if (c == '{')
                    depth++;
                else if (c == '}')
                {
                    depth--;
                    if (depth == 0)
                    {
                        // finished one object
                        count++;
                        // reservoir sampling: replace chosen with probability 1/count
                        if (count == 1)
                        {
                            out_obj = cur;
                        }
                        else
                        {
                            long r = random(count); // 0..count-1
                            if (r == 0)
                                out_obj = cur;
                        }
                        in_object = false;
                        cur.clear();
                    }
                }
            }
        }
    }

    Serial.printf("[XMNOTE] pick_random_object_from_array '%s' scanned=%u chosen_len=%u\n", path, (unsigned)count, (unsigned)out_obj.length());
    f.close();
    return count > 0 && out_obj.length() > 0;
}

// Try to read a window (max_window bytes) from a random offset and extract complete JSON objects inside it.
// If successful, pick one at random from that window. Returns true if an object was chosen.
static bool pick_random_object_from_array_window(const char *path, String &out_obj, size_t max_window = 10240, int attempts = 5)
{
    if (!SDW::SD.exists(path))
        return false;
    File f = SDW::SD.open(path, "r");
    if (!f)
        return false;

    size_t file_size = 0;
#if defined(ESP32)
    file_size = f.size();
#endif
    if (file_size == 0)
    {
        f.close();
        return false;
    }

    randomSeed(millis());
    const size_t OVERLAP = 1024; // include some bytes before the random start to catch object starts

    // Attempt sequence: start with requested max_window (default 10KB). Only if all attempts fail
    // progressively try larger windows before falling back to streaming reservoir sampling.
    const size_t progressive_windows[] = {max_window, max_window * 2, 51200, 131072};
    const int progressive_attempts[] = {attempts, 3, 2, 1};

    for (size_t wi = 0; wi < sizeof(progressive_windows) / sizeof(progressive_windows[0]); ++wi)
    {
        size_t w = progressive_windows[wi];
        int att = progressive_attempts[wi];
        if (w <= 0)
            continue;
        if (w > file_size)
            w = file_size;

        for (int a = 0; a < att; ++a)
        {
            unsigned long start_base = 0;
            if (file_size > w)
                start_base = random((unsigned long)(file_size - w + 1));
            unsigned long seek_pos = (start_base > OVERLAP) ? (start_base - OVERLAP) : 0;
            if (!f.seek(seek_pos))
                continue;

            // Read up to w + overlap bytes into chunk (bounded by file)
            size_t max_read = w + (size_t)OVERLAP;
            if (seek_pos + max_read > file_size)
                max_read = file_size - seek_pos;

            String chunk;
            chunk.reserve(max_read + 8);
            size_t read = 0;
            const int BLK = 1024;
            char buf[BLK];
            while (f.available() && read < max_read)
            {
                int want = (int)std::min((size_t)BLK, max_read - read);
                int r = f.readBytes(buf, want);
                if (r <= 0)
                    break;
                for (int i = 0; i < r; ++i)
                    chunk += buf[i];
                read += r;
            }

            // scan chunk for complete objects
            std::vector<String> objs;
            size_t L = chunk.length();
            bool in_string = false;
            bool escape = false;
            for (size_t i = 0; i < L; ++i)
            {
                if (chunk.charAt(i) != '{')
                    continue;
                // attempt to parse object from i
                int depth = 0;
                in_string = false;
                escape = false;
                size_t j = i;
                bool finished = false;
                for (; j < L; ++j)
                {
                    char c = chunk.charAt(j);
                    if (in_string)
                    {
                        if (escape)
                            escape = false;
                        else if (c == '\\')
                            escape = true;
                        else if (c == '"')
                            in_string = false;
                    }
                    else
                    {
                        if (c == '"')
                        {
                            in_string = true;
                        }
                        else if (c == '{')
                        {
                            depth++;
                        }
                        else if (c == '}')
                        {
                            depth--;
                            if (depth == 0)
                            {
                                finished = true;
                                break;
                            }
                        }
                    }
                }
                if (finished)
                {
                    String so = chunk.substring(i, j + 1);
                    objs.push_back(so);
                    i = j; // continue after this object
                }
            }

            Serial.printf("[XMNOTE] window try w=%u attempt=%d start_base=%u seek=%u read=%u objs=%u\n", (unsigned)w, a, (unsigned)start_base, (unsigned)seek_pos, (unsigned)read, (unsigned)objs.size());
            if (!objs.empty())
            {
                int pick = random((int)objs.size());
                out_obj = objs[pick];
                f.close();
                Serial.printf("[XMNOTE] picked object from window len=%u (window=%u)\n", (unsigned)out_obj.length(), (unsigned)w);
                return true;
            }
        }
        // if we tried the initial (small) window and found nothing, continue to next larger window
    }

    f.close();
    // final fallback: do full streaming reservoir sampling (slower but guaranteed)
    Serial.printf("[XMNOTE] window attempts failed, falling back to stream sampling\n");
    return pick_random_object_from_array(path, out_obj);
}

// Flexible field lookup from a JsonObject
static const char *get_string_field(JsonObject &obj, const std::vector<const char *> &candidates)
{
    for (auto k : candidates)
    {
        if (obj.containsKey(k))
        {
            const char *v = obj[k];
            if (v && v[0] != '\0')
                return v;
        }
    }
    return nullptr;
}

void render_xmnote_component(JsonObject component)
{
    Serial.printf("[XMNOTE] render_xmnote_component called\n");
    // compute pixel area
    int pos_x = 0, pos_y = 0;
    int a_w = 0, a_h = 0;
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

    const int CELL_W = 60, CELL_H = 60;
    int16_t x = pos_x * CELL_W + 10;
    int16_t y = pos_y * CELL_H + 30;
    // a_w = (int)(cell_w * CELL_W) - 40;
    a_w = (int)(cell_w * CELL_W) - 20;
    a_h = (int)(cell_h * CELL_H) - 40;

    // config
    int baseFont = 24;
    if (component.containsKey("config"))
    {
        JsonObject cfg = component["config"].as<JsonObject>();
        baseFont = cfg["fontSize"] | baseFont;
    }

    // Select one excerpt object: prefer windowed random read (fast, small reads),
    // fall back to exceptions file, then full streaming reservoir sampling.
    String chosen_obj_str;
    bool got = pick_random_object_from_array_window("/xmnote/excerpts.json", chosen_obj_str);
    if (!got)
    {
        Serial.printf("[XMNOTE] window selection failed for excerpts.json, trying exceptions.json window\n");
        got = pick_random_object_from_array_window("/xmnote/exceptions.json", chosen_obj_str);
    }
    if (!got)
    {
        Serial.printf("[XMNOTE] window selection failed, falling back to full stream sampling\n");
        got = pick_random_object_from_array("/xmnote/excerpts.json", chosen_obj_str);
        if (!got)
            got = pick_random_object_from_array("/xmnote/exceptions.json", chosen_obj_str);
    }
    if (!got)
    {
        Serial.printf("[XMNOTE] no excerpts array found under /xmnote or file empty\n");
        return;
    }

    Serial.printf("[XMNOTE] chosen excerpt object len=%d\n", (int)chosen_obj_str.length());

    // parse the chosen object only
    size_t chosen_len = chosen_obj_str.length();
    size_t chosen_cap = chosen_len + 1024;
    if (chosen_cap > 65536)
        chosen_cap = 65536;
    DynamicJsonDocument chosen_doc(chosen_cap);
    DeserializationError derr = deserializeJson(chosen_doc, chosen_obj_str);
    if (derr)
    {
        Serial.printf("[XMNOTE] chosen object parse error: %s (len=%u cap=%u)\n", derr.c_str(), (unsigned)chosen_len, (unsigned)chosen_cap);
        return;
    }
    JsonObject chosen = chosen_doc.as<JsonObject>();

    // flexible keys
    // Expected fields per user: id, bookId, content, idea, chapter
    // bookId may be numeric or string; normalize to string for matching
    std::string book_id_str;
    if (chosen.containsKey("bookId"))
    {
        JsonVariant v = chosen["bookId"];
        if (v.is<const char *>())
            book_id_str = std::string(v.as<const char *>());
        else if (v.is<long long>())
            book_id_str = std::to_string((long long)v.as<long long>());
        else if (v.is<int>())
            book_id_str = std::to_string((int)v.as<int>());
    }
    else if (chosen.containsKey("book_id"))
    {
        JsonVariant v = chosen["book_id"];
        if (v.is<const char *>())
            book_id_str = std::string(v.as<const char *>());
        else if (v.is<long long>())
            book_id_str = std::to_string((long long)v.as<long long>());
        else if (v.is<int>())
            book_id_str = std::to_string((int)v.as<int>());
    }
    else if (chosen.containsKey("id"))
    {
        JsonVariant v = chosen["id"];
        if (v.is<const char *>())
            book_id_str = std::string(v.as<const char *>());
        else if (v.is<long long>())
            book_id_str = std::to_string((long long)v.as<long long>());
        else if (v.is<int>())
            book_id_str = std::to_string((int)v.as<int>());
    }

    const char *book_id = book_id_str.empty() ? nullptr : book_id_str.c_str();
    Serial.printf("[XMNOTE] excerpt bookId=%s\n", book_id ? book_id : "(null)");

    const char *excerpt = nullptr;
    if (chosen.containsKey("content"))
        excerpt = chosen["content"];
    else if (chosen.containsKey("excerpt"))
        excerpt = chosen["excerpt"];
    else if (chosen.containsKey("text"))
        excerpt = chosen["text"];

    Serial.printf("[XMNOTE] excerpt content len=%d\n", excerpt ? (int)strlen(excerpt) : 0);

    const char *idea = nullptr;
    if (chosen.containsKey("idea"))
        idea = chosen["idea"];
    else if (chosen.containsKey("thought"))
        idea = chosen["thought"];
    else if (chosen.containsKey("comment"))
        idea = chosen["comment"];

    Serial.printf("[XMNOTE] idea len=%d\n", idea ? (int)strlen(idea) : 0);

    // Load books.json from /xmnote
    String books_content;
    read_file_to_string("/xmnote/books.json", books_content, 32768) || read_file_to_string("/xmnote/book.json", books_content, 32768);
    size_t books_len = books_content.length();
    size_t books_capacity = books_len + 4096;
    if (books_capacity > 65536)
        books_capacity = 65536;
    DynamicJsonDocument books_doc(books_capacity);
    bool have_book = false;
    std::string title_str, author_str;
    if (books_content.length() > 0)
    {
        Serial.printf("[XMNOTE] loaded books file, len=%d\n", (int)books_content.length());
        if (deserializeJson(books_doc, books_content) == DeserializationError::Ok)
        {
            JsonArray b_arr;
            if (books_doc.is<JsonArray>())
                b_arr = books_doc.as<JsonArray>();
            else if (books_doc.containsKey("books") && books_doc["books"].is<JsonArray>())
                b_arr = books_doc["books"].as<JsonArray>();

            if (!b_arr.isNull())
            {
                for (JsonObject b : b_arr)
                {
                    // Normalize book id from books.json
                    std::string bid_str;
                    if (b.containsKey("id"))
                    {
                        JsonVariant vb = b["id"];
                        if (vb.is<const char *>())
                            bid_str = std::string(vb.as<const char *>());
                        else if (vb.is<long long>())
                            bid_str = std::to_string((long long)vb.as<long long>());
                        else if (vb.is<int>())
                            bid_str = std::to_string((int)vb.as<int>());
                    }
                    else if (b.containsKey("bookId"))
                    {
                        JsonVariant vb = b["bookId"];
                        if (vb.is<const char *>())
                            bid_str = std::string(vb.as<const char *>());
                        else if (vb.is<long long>())
                            bid_str = std::to_string((long long)vb.as<long long>());
                        else if (vb.is<int>())
                            bid_str = std::to_string((int)vb.as<int>());
                    }
                    else if (b.containsKey("book_id"))
                    {
                        JsonVariant vb = b["book_id"];
                        if (vb.is<const char *>())
                            bid_str = std::string(vb.as<const char *>());
                        else if (vb.is<long long>())
                            bid_str = std::to_string((long long)vb.as<long long>());
                        else if (vb.is<int>())
                            bid_str = std::to_string((int)vb.as<int>());
                    }
                    if (!book_id)
                        continue;
                    if (bid_str.empty())
                        continue;
                    if (book_id_str == bid_str)
                    {
                        if (b.containsKey("name"))
                            title_str = std::string(b["name"].as<const char *>());
                        else if (b.containsKey("title"))
                            title_str = std::string(b["title"].as<const char *>());
                        if (b.containsKey("author"))
                            author_str = std::string(b["author"].as<const char *>());
                        Serial.printf("[XMNOTE] matched book id=%s title=%s author=%s\n", bid_str.c_str(), title_str.c_str(), author_str.c_str());
                        have_book = true;
                        break;
                    }
                }
            }
        }
    }

    // fallback to fields in chosen
    if (!have_book)
    {
        if (chosen.containsKey("title"))
            title_str = std::string(chosen["title"].as<const char *>());
        if (chosen.containsKey("author"))
            author_str = std::string(chosen["author"].as<const char *>());
    }

    // Prepare strings
    String idea_s = idea ? String(idea) : String("");
    String excerpt_s = excerpt ? String(excerpt) : String("");
    String title_s = title_str.empty() ? String("") : String(title_str.c_str());
    String author_s = author_str.empty() ? String("") : String(author_str.c_str());
    // Prepare fonts
    // int f_idea = (int)(baseFont * 1.2f + 0.5f);
    int f_idea = (int)(baseFont * 0.95f);
    int f_excerpt = baseFont;
    int f_title = (int)(baseFont * 0.8f);
    // int f_author = (int)(baseFont * 0.8f + 0.5f);
    int f_author = (int)(baseFont);

    // Compute per-line heights using same formula as display_print_wrapped
    uint8_t base_font_file = get_font_size_from_file();
    if (base_font_file == 0)
        base_font_file = 24;
    auto line_height_for = [&](int font_sz) -> int
    {
        float scale = (font_sz > 0 && base_font_file > 0) ? ((float)font_sz / (float)base_font_file) : 1.0f;
        return (int)((base_font_file + LINE_MARGIN) * scale + 0.5f);
    };

    int author_line_h = line_height_for(f_author);
    int title_line_h = line_height_for(f_title);
    int idea_line_h = line_height_for(f_idea);
    int content_line_h = line_height_for(f_excerpt);

    // Fixed allocations: author 1 line, title 2 lines
    int author_h = author_line_h * 1;
    int title_h = title_line_h * 2;

    int remaining_h = a_h - (author_h + title_h + 2 * LINE_MARGIN);
    if (remaining_h < 0)
        remaining_h = 0;

    // Decide distribution between idea and content
    int content_h = 0;
    int idea_h = 0;
    bool has_content = excerpt_s.length() > 0;
    bool has_idea = idea_s.length() > 0;

    // Helper: count wrapped lines for given text and font size
    auto count_wrapped_lines = [&](const String &s, int16_t area_w, int font_sz) -> int
    {
        if (s.length() == 0)
            return 0;
        std::string st = std::string(s.c_str());
        size_t pos = 0;
        int lines = 0;
        while (pos < st.length())
        {
            size_t next = find_break_position_scaled(st, pos, area_w, false, (float)font_sz);
            if (next == pos)
                break;
            lines++;
            pos = next;
            if (pos < st.length() && st[pos] == '\n')
                pos++;
        }
        return lines > 0 ? lines : 0;
    };

    // Compute how many lines content/idea will occupy (subject to limits)
    int content_need_lines = has_content ? count_wrapped_lines(excerpt_s, a_w, f_excerpt) : 0;
    int idea_need_lines = has_idea ? count_wrapped_lines(idea_s, a_w, f_idea) : 0;

    if (has_content)
    {
        if (has_idea)
        {
            int content_max_lines = 4;
            int content_lines = std::min(content_need_lines, content_max_lines);
            // ensure content_lines do not exceed what remaining_h can hold
            int max_content_by_px = remaining_h / content_line_h;
            if (content_lines > max_content_by_px)
                content_lines = max_content_by_px;
            content_h = content_lines * content_line_h;

            int remain_after_content = remaining_h - content_h;
            int idea_lines = std::min(idea_need_lines, std::max(0, remain_after_content / idea_line_h));
            idea_h = idea_lines * idea_line_h;
        }
        else
        {
            // idea missing: content may occupy entire remaining area (but limited by its need)
            int content_lines = std::min(content_need_lines, remaining_h / content_line_h);
            // if content_need_lines is 0 but text non-empty, still allow at least one line
            if (content_lines == 0 && content_need_lines > 0)
                content_lines = 1;
            content_h = content_lines * content_line_h;
            idea_h = 0;
        }
    }
    else
    {
        // no content, idea takes all remaining (but limited by need)
        int idea_lines = std::min(idea_need_lines, remaining_h / idea_line_h);
        if (idea_lines == 0 && idea_need_lines > 0)
            idea_lines = 1;
        idea_h = idea_lines * idea_line_h;
        content_h = 0;
    }

    Serial.printf("[XMNOTE] layout a_h=%d author_h=%d title_h=%d remaining=%d idea_h=%d content_h=%d\n", a_h, author_h, title_h, remaining_h, idea_h, content_h);

    // Render in order: idea (top), content, title, author (bottom)
    // Center idea+content vertically within the remaining area
    int total_idea_content_h = idea_h + content_h;
    int top_offset = 0;
    if (remaining_h > total_idea_content_h)
        top_offset = (remaining_h - total_idea_content_h) / 2;
    int cur_y = y + top_offset;

    if (has_idea && idea_h > 0)
    {
        Serial.printf("[XMNOTE] render idea at y=%d h=%d font=%d len=%d\n", cur_y, idea_h, f_idea, (int)idea_s.length());
        g_canvas->fillCircle(PAPER_S3_WIDTH - 20, cur_y , 10, GREY_LEVEL_LIGHT);
        g_canvas->fillCircle(PAPER_S3_WIDTH - 20, cur_y , 6, GREY_LEVEL_DARK);
        g_canvas->drawCircle(PAPER_S3_WIDTH - 20, cur_y , 10);
        g_canvas->fillCircle(PAPER_S3_WIDTH - 35, cur_y - 10, 4, GREY_LEVEL_DARK);
        g_canvas->fillCircle(PAPER_S3_WIDTH - 25, cur_y +20 , 2, GREY_LEVEL_MID);
        display_print_wrapped(idea_s.c_str(), x, cur_y, a_w, idea_h, f_idea, 0, 15, 0, false, false);
        cur_y += idea_h;
        //        g_canvas->drawLine(20,cur_y,PAPER_S3_WIDTH-20, cur_y, GREY_LEVEL_LIGHT);
    }
    else if (has_idea && idea_h == 0)
    {
        // if idea exists but no height assigned, still render one minimal line
        int min_h = idea_line_h;
        g_canvas->fillCircle(PAPER_S3_WIDTH - 20, cur_y , 10, GREY_LEVEL_LIGHT);
        g_canvas->fillCircle(PAPER_S3_WIDTH - 20, cur_y , 6, GREY_LEVEL_DARK);
        g_canvas->drawCircle(PAPER_S3_WIDTH - 20, cur_y , 10);
        g_canvas->fillCircle(PAPER_S3_WIDTH - 35, cur_y - 10, 4, GREY_LEVEL_DARK);
        g_canvas->fillCircle(PAPER_S3_WIDTH - 25, cur_y +20 , 2, GREY_LEVEL_MID);
 
        Serial.printf("[XMNOTE] render idea minimal at y=%d h=%d\n", cur_y, min_h);
        display_print_wrapped(idea_s.c_str(), x, cur_y, a_w, min_h, f_idea, 0, 15, 0, false, false);
        cur_y += min_h;
    }

    if (has_content && content_h > 0)
    {
        g_canvas->fillRect(5, cur_y - 4, 8, 8, GREY_LEVEL_MID);
        g_canvas->fillTriangle(5, cur_y - 4, 15, cur_y - 12, 10, cur_y - 4, GREY_LEVEL_MID);
        g_canvas->fillRect(15, cur_y - 4, 8, 8, GREY_LEVEL_MID);
        g_canvas->fillTriangle(15, cur_y - 4, 25, cur_y - 12, 20, cur_y - 4, GREY_LEVEL_MID);

        Serial.printf("[XMNOTE] render content at y=%d h=%d font=%d len=%d\n", cur_y, content_h, f_excerpt, (int)excerpt_s.length());
        display_print_wrapped(excerpt_s.c_str(), x, cur_y + 2, a_w, content_h, f_excerpt, 0, 15, 0, false, false);
        cur_y += content_h;
    }
    else if (has_content && content_h == 0)
    {
        g_canvas->fillRect(8, cur_y - 4, 10, 10, GREY_LEVEL_MID);
        g_canvas->fillTriangle(8, cur_y - 4, 18, cur_y - 12, 13, cur_y - 4, GREY_LEVEL_MID);
        g_canvas->fillRect(20, cur_y - 4, 10, 10, GREY_LEVEL_MID);
        g_canvas->fillTriangle(20, cur_y - 4, 30, cur_y - 12, 25, cur_y - 4, GREY_LEVEL_MID);

        int min_h = content_line_h;
        Serial.printf("[XMNOTE] render content minimal at y=%d h=%d\n", cur_y, min_h);
        display_print_wrapped(excerpt_s.c_str(), x, cur_y + 2, a_w, min_h, f_excerpt, 0, 15, 0, false, false);
        cur_y += min_h;
    }

    // title and author at bottom: ensure sufficient space remains; render with their fixed heights
    // If there's not enough space, allow display_print_wrapped to trim lines.
    // Title should start at the bottom of the remaining area (not immediately after idea/content)
    int title_y = y + remaining_h + LINE_MARGIN;
    Serial.printf("[XMNOTE] render title at y=%d (expected start y=%d) h=%d font=%d len=%d\n", title_y, y + remaining_h, title_h, f_title, (int)title_s.length());
    // Clamp title_y so title+author won't overflow the component area
    int max_title_y = y + a_h - (title_h + author_h);
    if (title_y > max_title_y)
        title_y = max_title_y;

    // Print one rect
    if (title_s.length() > 0)
    {
        display_print_wrapped(title_s.c_str(), x, title_y, a_w, title_h, f_title, 0, 15, 0, false, false, true);
    }

    int author_y = title_y + title_h + LINE_MARGIN;
    if (author_y + author_h > y + a_h)
        author_y = y + a_h - author_h;
    Serial.printf("[XMNOTE] render author at y=%d h=%d font=%d len=%d\n", author_y, author_h, f_author, (int)author_s.length());
    // if (author_s.length() > 0) display_print_wrapped(author_s.c_str(), x, author_y, a_w, author_h, f_author, 0, 15, 0, false, false);

    ui_push_image_to_canvas("/spiffs/xmnote.png", 0, author_y - (28 - f_author / 2));
    if (author_s.length() > 0)
        bin_font_print(author_s.c_str(), f_author, 0, a_w - 56, 56, author_y, false, nullptr, TEXT_ALIGN_LEFT, a_w - 56);
}
