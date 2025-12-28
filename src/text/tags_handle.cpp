#include "tags_handle.h"
#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include "../SD/SDWrapper.h"
#include "device/safe_fs.h"
#include "book_handle.h"
#include "text/text_handle.h"

// 最大条目数
static const size_t MAX_TAG_LINES = 10;

// 辅助：得到 tags 文件名，基于 getBookmarkFileName 的安全化规则
static std::string getTagsFileName(const std::string &book_file_path)
{
    // getBookmarkFileName 返回 /bookmarks/<safe>.bm
    std::string bm = getBookmarkFileName(book_file_path);
    // 将后缀替换为 .tags
    size_t dot = bm.find_last_of('.');
    std::string result;
    if (dot != std::string::npos)
    {
        result = bm.substr(0, dot) + ".tags";
    }
    else
    {
        result = bm + ".tags";
    }
    return result;
}

// 从指定书籍文件读取预览：从 position 开始，取前 10 个非空字符（按字符计数，不是字节）
// 支持 UTF-8 和 GBK 两种编码：返回 UTF-8 编码的 preview
static inline bool is_unicode_whitespace(uint32_t cp)
{
    // common whitespace code points
    if (cp <= 0x000D)
    {
        // 0x0009 - 0x000D (tab/newline/vertical tab/form feed/carriage return) + 0x0000..
        return (cp >= 0x0009 && cp <= 0x000D) || cp == 0x0000 || cp == 0x0001;
    }
    if (cp == 0x0020)
        return true; // space
    if (cp == 0x00A0)
        return true; // NO-BREAK SPACE
    if (cp >= 0x2000 && cp <= 0x200A)
        return true; // various en/em spaces
    if (cp == 0x2028 || cp == 0x2029)
        return true; // line separator, paragraph separator
    if (cp == 0x202F || cp == 0x205F)
        return true; // narrow no-break space, medium math space
    if (cp == 0x3000)
        return true; // ideographic space
    return false;
}

static inline bool is_linebreak(uint32_t cp)
{
    // common line break characters: LF, CR, Unicode line/paragraph separators
    if (cp == 0x000A || cp == 0x000D)
        return true;
    if (cp == 0x2028 || cp == 0x2029)
        return true;
    return false;
}

// 从指定书籍文件读取预览：从 position 开始，去掉开头的空白（包括 Unicode 常见空白），取随后 10 个字符（按字符计数，不是字节）
// 支持 UTF-8 和 GBK：返回 UTF-8 编码的字符串
static std::string makePreviewFromBook(const std::string &book_file_path, size_t position)
{
    // open underlying file (SPIFFS or SD)
    std::string path = book_file_path;
    File f;
    bool use_spiffs = false;
    if (path.rfind("/spiffs", 0) == 0)
    {
        use_spiffs = true;
        path = path.substr(7);
    }
    if (!use_spiffs)
    {
        if (path.rfind("/sd", 0) == 0)
            path = path.substr(3);
    }

    if (use_spiffs)
        f = SPIFFS.open(path.c_str(), "r");
    else
        f = SDW::SD.open(path.c_str(), "r");

    if (!f)
        return std::string();

    size_t file_size = (size_t)f.size();
    if (position >= file_size)
    {
        f.close();
        return std::string();
    }

    const size_t READ_BUF = 2048;
    f.seek(position, SeekSet);
    std::vector<uint8_t> buf;
    buf.resize(READ_BUF);
    int nread = f.read(buf.data(), READ_BUF);
    if (nread <= 0)
    {
        f.close();
        return std::string();
    }
    buf.resize((size_t)nread);

    TextEncoding enc = detect_text_encoding(buf.data(), buf.size());

    // Convert buffer to a UTF-8 string we can iterate safely over codepoints
    std::string utf8s;
    if (enc == TextEncoding::GBK)
    {
        // raw bytes -> GBK -> UTF-8 using existing project helper
        std::string raw((const char *)buf.data(), buf.size());
        utf8s = convert_to_utf8(raw, TextEncoding::GBK);
    }
    else
    {
        // assume UTF-8
        utf8s.assign((const char *)buf.data(), buf.size());
    }

    // iterate over UTF-8 codepoints, skip leading whitespace codepoints, then take next up to 10 codepoints
    std::string preview;
    preview.reserve(64);
    size_t idx = 0;
    size_t len = utf8s.size();

    // helper to read next utf8 codepoint and return its codepoint and byte-range
    auto next_utf8 = [&](size_t &pos, uint32_t &out_cp, size_t &out_bytes) -> bool
    {
        if (pos >= len)
            return false;
        unsigned char b0 = (unsigned char)utf8s[pos];
        if ((b0 & 0x80) == 0)
        {
            out_cp = b0;
            out_bytes = 1;
        }
        else if ((b0 & 0xE0) == 0xC0)
        {
            if (pos + 1 >= len)
                return false;
            out_cp = ((b0 & 0x1F) << 6) | ((unsigned char)utf8s[pos + 1] & 0x3F);
            out_bytes = 2;
        }
        else if ((b0 & 0xF0) == 0xE0)
        {
            if (pos + 2 >= len)
                return false;
            out_cp = ((b0 & 0x0F) << 12) | (((unsigned char)utf8s[pos + 1] & 0x3F) << 6) | ((unsigned char)utf8s[pos + 2] & 0x3F);
            out_bytes = 3;
        }
        else if ((b0 & 0xF8) == 0xF0)
        {
            if (pos + 3 >= len)
                return false;
            out_cp = ((b0 & 0x07) << 18) | (((unsigned char)utf8s[pos + 1] & 0x3F) << 12) | (((unsigned char)utf8s[pos + 2] & 0x3F) << 6) | ((unsigned char)utf8s[pos + 3] & 0x3F);
            out_bytes = 4;
        }
        else
        {
            // invalid/unsupported start byte, skip
            out_cp = 0;
            out_bytes = 1;
        }
        return true;
    };

    // skip leading whitespace
    while (idx < len)
    {
        uint32_t cp = 0;
        size_t bytes = 0;
        if (!next_utf8(idx, cp, bytes))
            break;
        if (is_unicode_whitespace(cp))
        {
            idx += bytes;
            continue;
        }
        break;
    }

    // collect up to 10 characters (after trimming leading whitespace)
    size_t collected = 0;
    size_t pos = idx;
    while (pos < len && collected < 10)
    {
        uint32_t cp = 0;
        size_t bytes = 0;
        if (!next_utf8(pos, cp, bytes))
            break;
        // skip line break characters inside the collected characters
        if (is_linebreak(cp))
        {
            pos += bytes;
            continue;
        }
        size_t start = pos;
        preview.append(utf8s.data() + start, bytes);
        pos = start + bytes;
        ++collected;
    }

    // Trim any trailing control characters
    while (!preview.empty() && (preview.back() == '\0' || (unsigned char)preview.back() < 0x20))
        preview.pop_back();

    return preview;
}

// 将 vector 写回 .tags 文件（覆盖写入），格式：pos:"preview":percentage
static bool writeTagsFile(const std::string &tags_fn, const std::vector<TagEntry> &entries)
{
    // use SafeFS to write atomically
    bool result = SafeFS::safeWrite(tags_fn, [&](File &f)
                             {
                                 char linebuf[256];
                                 for (const auto &e : entries)
                                 {
                                     // sanitize preview: remove any '"' 字符
                                     std::string pv = e.preview;
                                     for (char &c : pv)
                                     {
                                         if (c == '"')
                                             c = ' ';
                                     }
                                     // percentage with two decimals
                                     float pct = e.percentage;
                                     int n = snprintf(linebuf, sizeof(linebuf), "%zu:\"%s\":%.2f\n", e.position, pv.c_str(), pct);
                                     if (n > 0)
                                     {
                                         // write marker prefix: 'A:' for auto, 'M:' for manual
                                         char mark[2] = { e.is_auto ? 'A' : 'M', ':' };
                                         f.write((const uint8_t *)mark, 2);
                                         f.write((const uint8_t *)linebuf, (size_t)n);
                                     }
                                 }
                                 return true;
                             });
    
    // 【调试日志】记录tags文件写入结果
    if (result)
    {
        Serial.printf("[Tags] writeTagsFile: 写入成功 (%zu tags)\n", entries.size());
    }
    else
    {
        Serial.println("[Tags] writeTagsFile: 写入失败！");
    }
    
    return result;
}

std::vector<TagEntry> loadTagsForFile(const std::string &book_file_path)
{
    std::vector<TagEntry> out;
    std::string tags_fn = getTagsFileName(book_file_path);
    SafeFS::restoreFromTmpIfNeeded(tags_fn);
    if (!SDW::SD.exists(tags_fn.c_str()))
    {
        // 【调试日志】文件不存在（正常情况，不需要警告）
        return out; // 文件不存在返回空
    }

    File f = SDW::SD.open(tags_fn.c_str(), "r");
    if (!f)
    {
        Serial.printf("[Tags] loadTagsForFile: 警告 - 无法打开tags文件 %s\n", tags_fn.c_str());
        return out;
    }

    std::vector<TagEntry> parsed;
    while (f.available())
    {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0)
            continue;
        std::string s = std::string(line.c_str());
        bool is_auto = false;
        if (s.size() >= 2 && (s[0] == 'A' || s[0] == 'M') && s[1] == ':')
        {
            is_auto = (s[0] == 'A');
            s = s.substr(2);
        }
        else
        {
            // 【警告】没有A:/M:前缀的行（旧格式或格式错误）
            Serial.printf("[Tags] loadTagsForFile: 警告 - 行没有A:/M:前缀，默认为manual: %s\n", s.c_str());
        }

        // parse remaining as pos:"preview":percentage
        size_t p1 = s.find(':');
        if (p1 == std::string::npos)
            continue;
        std::string posstr = s.substr(0, p1);
        size_t q1 = s.find('"', p1 + 1);
        size_t q2 = std::string::npos;
        if (q1 != std::string::npos)
            q2 = s.find('"', q1 + 1);

        std::string preview;
        size_t p2 = p1 + 1;
        if (q1 != std::string::npos && q2 != std::string::npos)
        {
            preview = s.substr(q1 + 1, q2 - (q1 + 1));
            p2 = q2 + 1;
        }

        size_t p3 = s.find(':', p2);
        std::string pctstr;
        if (p3 != std::string::npos)
            pctstr = s.substr(p3 + 1);
        else
        {
            if (p2 < s.size())
                pctstr = s.substr(p2);
        }

        auto trim = [](std::string &t)
        {
            while (!t.empty() && isspace((unsigned char)t.front()))
                t.erase(t.begin());
            while (!t.empty() && isspace((unsigned char)t.back()))
                t.pop_back();
        };
        trim(posstr);
        trim(preview);
        trim(pctstr);

        unsigned long long posv = strtoull(posstr.c_str(), nullptr, 10);
        float pctv = 0.0f;
        if (!pctstr.empty())
            pctv = (float)atof(pctstr.c_str());

        TagEntry te;
        te.position = (size_t)posv;
        te.preview = preview;
        te.percentage = pctv;
        te.is_auto = is_auto;
        parsed.push_back(te);
    }
    f.close();

    // separate auto and manual entries
    TagEntry auto_entry; bool have_auto = false;
    std::vector<TagEntry> manual;
    for (const auto &e : parsed)
    {
        if (e.is_auto)
        {
            auto_entry = e; have_auto = true;
        }
        else
            manual.push_back(e);
    }

    std::sort(manual.begin(), manual.end(), [](const TagEntry &a, const TagEntry &b)
              { return a.position < b.position; });
    if (manual.size() > 0 && manual.size() > (MAX_TAG_LINES > 0 ? (MAX_TAG_LINES - 1) : 0))
        manual.resize((MAX_TAG_LINES > 0) ? (MAX_TAG_LINES - 1) : 0);

    if (have_auto)
        out.push_back(auto_entry);
    for (const auto &m : manual)
        out.push_back(m);

    // ReCompute percentage based on actual book file size
    size_t total = 0;
    std::string path = book_file_path;
    File tempf;
    if (path.rfind("/sd", 0) == 0)
        path = path.substr(3);
    if (path.rfind("/spiffs", 0) == 0)
        path = path.substr(7);
    if (book_file_path.rfind("/spiffs", 0) == 0)
        tempf = SPIFFS.open(path.c_str(), "r");
    else
        tempf = SDW::SD.open(path.c_str(), "r");
    if (tempf)
    {
        tempf.seek(0, SeekEnd);
        total = (size_t)tempf.position();
        tempf.close();
    }

    if (total > 0)
    {
        for (auto &e : out)
        {
            e.percentage = (float)((double)e.position * 100.0 / (double)total);
        }
    }
    
    // 【调试日志】记录加载的tags数量
    if (!out.empty())
    {
        Serial.printf("[Tags] loadTagsForFile: 成功加载 %zu 个tags (文件: %s, 书籍大小: %zu bytes)\n", 
                     out.size(), tags_fn.c_str(), total);
    }
    
    return out;
}

bool insertTagForFile(const std::string &book_file_path, size_t position)
{
    if (position == (size_t)-1)
        return false;

    std::string tags_fn = getTagsFileName(book_file_path);
    if (!ensureBookmarksFolder())
        return false;

    // load existing and split into auto/manual
    std::vector<TagEntry> entries = loadTagsForFile(book_file_path);
    TagEntry auto_entry; bool have_auto = false; std::vector<TagEntry> manual;
    for (const auto &e : entries)
    {
        if (e.is_auto) { auto_entry = e; have_auto = true; }
        else manual.push_back(e);
    }

    // compute preview and percentage
    size_t total = 0;
    std::string path = book_file_path;
    File tempf;
    if (path.rfind("/sd", 0) == 0)
        path = path.substr(3);
    if (path.rfind("/spiffs", 0) == 0)
        path = path.substr(7);
    if (book_file_path.rfind("/spiffs", 0) == 0)
        tempf = SPIFFS.open(path.c_str(), "r");
    else
        tempf = SDW::SD.open(path.c_str(), "r");
    if (tempf)
    {
        tempf.seek(0, SeekEnd);
        total = (size_t)tempf.position();
        tempf.close();
    }

    TagEntry newe;
    newe.position = position;
    newe.preview = makePreviewFromBook(book_file_path, position);
    if (total > 0)
        newe.percentage = (float)((double)position * 100.0 / (double)total);
    else
        newe.percentage = 0.0f;
    newe.is_auto = false;

    // remove any existing manual with same position
    manual.erase(std::remove_if(manual.begin(), manual.end(), [&](const TagEntry &t)
                                { return t.position == position; }),
                 manual.end());

    // If manual area is full (MAX_TAG_LINES-1), evict the earliest manual entry
    if (manual.size() >= (MAX_TAG_LINES > 0 ? (MAX_TAG_LINES - 1) : 0))
    {
        if (!manual.empty()) manual.erase(manual.begin());
    }

    manual.push_back(newe);
    std::sort(manual.begin(), manual.end(), [](const TagEntry &a, const TagEntry &b)
              { return a.position < b.position; });

    std::vector<TagEntry> combined;
    if (have_auto) combined.push_back(auto_entry);
    for (const auto &m : manual) combined.push_back(m);
    if (combined.size() > MAX_TAG_LINES) combined.resize(MAX_TAG_LINES);

    return writeTagsFile(tags_fn, combined);
}

// Overload: insert tag using caller-provided preview (UTF-8), avoid extra file IO
bool insertTagForFile(const std::string &book_file_path, size_t position, const std::string &preview_override)
{
    if (position == (size_t)-1)
        return false;

    std::string tags_fn = getTagsFileName(book_file_path);
    if (!ensureBookmarksFolder())
        return false;

    // load existing and split auto/manual
    std::vector<TagEntry> entries = loadTagsForFile(book_file_path);
    TagEntry auto_entry; bool have_auto = false; std::vector<TagEntry> manual;
    for (const auto &e : entries)
    {
        if (e.is_auto) { auto_entry = e; have_auto = true; } else manual.push_back(e);
    }

    // compute percentage using file size
    size_t total = 0;
    std::string path = book_file_path;
    File tempf;
    if (path.rfind("/sd", 0) == 0)
        path = path.substr(3);
    if (path.rfind("/spiffs", 0) == 0)
        path = path.substr(7);
    if (book_file_path.rfind("/spiffs", 0) == 0)
        tempf = SPIFFS.open(path.c_str(), "r");
    else
        tempf = SDW::SD.open(path.c_str(), "r");
    if (tempf)
    {
        tempf.seek(0, SeekEnd);
        total = (size_t)tempf.position();
        tempf.close();
    }

    TagEntry newe;
    newe.position = position;
    newe.preview = preview_override; // assume UTF-8 and already trimmed/normalized by caller
    if (total > 0)
        newe.percentage = (float)((double)position * 100.0 / (double)total);
    else
        newe.percentage = 0.0f;
    newe.is_auto = false;

    // remove any existing manual with same position
    manual.erase(std::remove_if(manual.begin(), manual.end(), [&](const TagEntry &t)
                                { return t.position == position; }),
                 manual.end());

    // If manual area is full, evict earliest manual
    if (manual.size() >= (MAX_TAG_LINES > 0 ? (MAX_TAG_LINES - 1) : 0))
    {
        if (!manual.empty()) manual.erase(manual.begin());
    }

    manual.push_back(newe);
    std::sort(manual.begin(), manual.end(), [](const TagEntry &a, const TagEntry &b)
              { return a.position < b.position; });

    std::vector<TagEntry> combined;
    if (have_auto) combined.push_back(auto_entry);
    for (const auto &m : manual) combined.push_back(m);
    if (combined.size() > MAX_TAG_LINES) combined.resize(MAX_TAG_LINES);

    return writeTagsFile(tags_fn, combined);
}

// Insert or update automatic slot0 tag. Replaces existing auto tag if any.
bool insertAutoTagForFile(const std::string &book_file_path, size_t position)
{
    if (position == (size_t)-1)
        return false;
    std::string tags_fn = getTagsFileName(book_file_path);
    if (!ensureBookmarksFolder())
        return false;

    std::vector<TagEntry> entries = loadTagsForFile(book_file_path);
    TagEntry auto_entry; bool have_auto = false; std::vector<TagEntry> manual;
    for (const auto &e : entries)
    {
        if (e.is_auto) { auto_entry = e; have_auto = true; }
        else manual.push_back(e);
    }
    
    // 【最大进度保护】auto tag 仅记录用户阅读的最远进度
    // 如果新位置比现有 auto tag 位置小，不更新（保护最大进度）
    if (have_auto && position < auto_entry.position)
    {
        Serial.printf("[Tags] insertAutoTagForFile: 保护最大进度，不更新 auto tag (new=%zu < existing=%zu)\n",
                      position, auto_entry.position);
        return true; // 返回成功，但不更新
    }

    // compute percentage
    size_t total = 0; std::string path = book_file_path; File tempf;
    if (path.rfind("/sd", 0) == 0) path = path.substr(3);
    if (path.rfind("/spiffs", 0) == 0) path = path.substr(7);
    if (book_file_path.rfind("/spiffs", 0) == 0) tempf = SPIFFS.open(path.c_str(), "r"); else tempf = SDW::SD.open(path.c_str(), "r");
    if (tempf) { tempf.seek(0, SeekEnd); total = (size_t)tempf.position(); tempf.close(); }

    TagEntry newe; newe.position = position; newe.preview = makePreviewFromBook(book_file_path, position); if (total > 0) newe.percentage = (float)((double)position * 100.0 / (double)total); else newe.percentage = 0.0f; newe.is_auto = true;

    // replace auto
    have_auto = true; auto_entry = newe;
    
    Serial.printf("[Tags] insertAutoTagForFile: 更新auto tag, new_pos=%zu (old_pos=%s)\n", 
                  position, have_auto ? "exists" : "none");

    std::sort(manual.begin(), manual.end(), [](const TagEntry &a, const TagEntry &b){ return a.position < b.position; });
    if (manual.size() > 0 && manual.size() > (MAX_TAG_LINES > 0 ? (MAX_TAG_LINES - 1) : 0)) manual.resize((MAX_TAG_LINES > 0) ? (MAX_TAG_LINES - 1) : 0);

    std::vector<TagEntry> combined;
    if (have_auto) combined.push_back(auto_entry);
    for (const auto &m : manual) combined.push_back(m);
    if (combined.size() > MAX_TAG_LINES) combined.resize(MAX_TAG_LINES);
    
    // 【调试日志】记录即将写入的tags信息
    Serial.printf("[Tags] insertAutoTagForFile: 准备写入 %zu 个tags (auto=%s, manual=%zu, 调用者: insertAutoTagForFile)\n", 
                  combined.size(), have_auto ? "yes" : "no", manual.size());
    
    return writeTagsFile(tags_fn, combined);
}

bool insertAutoTagForFile(const std::string &book_file_path, size_t position, const std::string &preview_override)
{
    if (position == (size_t)-1)
        return false;
    std::string tags_fn = getTagsFileName(book_file_path);
    if (!ensureBookmarksFolder())
        return false;

    std::vector<TagEntry> entries = loadTagsForFile(book_file_path);
    TagEntry auto_entry; bool have_auto = false; std::vector<TagEntry> manual;
    for (const auto &e : entries)
    {
        if (e.is_auto) { auto_entry = e; have_auto = true; }
        else manual.push_back(e);
    }
    
    // 【最大进度保护】auto tag 仅记录用户阅读的最远进度
    // 如果新位置比现有 auto tag 位置小，不更新（保护最大进度）
    if (have_auto && position < auto_entry.position)
    {
        return true; // 返回成功，但不更新
    }

    // compute percentage
    size_t total = 0; std::string path = book_file_path; File tempf;
    if (path.rfind("/sd", 0) == 0) path = path.substr(3);
    if (path.rfind("/spiffs", 0) == 0) path = path.substr(7);
    if (book_file_path.rfind("/spiffs", 0) == 0) tempf = SPIFFS.open(path.c_str(), "r"); else tempf = SDW::SD.open(path.c_str(), "r");
    if (tempf) { tempf.seek(0, SeekEnd); total = (size_t)tempf.position(); tempf.close(); }

    TagEntry newe; newe.position = position; newe.preview = preview_override; if (total > 0) newe.percentage = (float)((double)position * 100.0 / (double)total); else newe.percentage = 0.0f; newe.is_auto = true;

    have_auto = true; auto_entry = newe;

    std::sort(manual.begin(), manual.end(), [](const TagEntry &a, const TagEntry &b){ return a.position < b.position; });
    if (manual.size() > 0 && manual.size() > (MAX_TAG_LINES > 0 ? (MAX_TAG_LINES - 1) : 0)) manual.resize((MAX_TAG_LINES > 0) ? (MAX_TAG_LINES - 1) : 0);

    std::vector<TagEntry> combined; if (have_auto) combined.push_back(auto_entry); for (const auto &m : manual) combined.push_back(m); if (combined.size() > MAX_TAG_LINES) combined.resize(MAX_TAG_LINES);
    return writeTagsFile(tags_fn, combined);
}

bool deleteTagForFileByPosition(const std::string &book_file_path, size_t position)
{
    std::string tags_fn = getTagsFileName(book_file_path);
    if (!SDW::SD.exists(tags_fn.c_str()))
        return false;
    std::vector<TagEntry> entries = loadTagsForFile(book_file_path);
    size_t before = entries.size();
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const TagEntry &t)
                                 { return t.position == position; }),
                  entries.end());
    if (entries.size() == before)
        return false; // nothing removed
    // write updated file (if now empty remove file)
    if (entries.empty())
    {
        return SDW::SD.remove(tags_fn.c_str());
    }
    return writeTagsFile(tags_fn, entries);
}

bool deleteTagForFileByIndex(const std::string &book_file_path, size_t index)
{
    std::string tags_fn = getTagsFileName(book_file_path);
    if (!SDW::SD.exists(tags_fn.c_str()))
        return false;
    std::vector<TagEntry> entries = loadTagsForFile(book_file_path);
    if (index >= entries.size())
        return false;
    entries.erase(entries.begin() + index);
    if (entries.empty())
    {
        return SDW::SD.remove(tags_fn.c_str());
    }
    return writeTagsFile(tags_fn, entries);
}

bool clearTagsForFile(const std::string &book_file_path)
{
    std::string tags_fn = getTagsFileName(book_file_path);
    if (!SDW::SD.exists(tags_fn.c_str()))
        return true; // already clear
    return SDW::SD.remove(tags_fn.c_str());
}
