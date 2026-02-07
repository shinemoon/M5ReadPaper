#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#include "../SD/SDWrapper.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include <cstdint>
#include <stdint.h>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <map>
#include <ctime>
#include "text_handle.h"
#include "book_handle.h"
#include "ui/ui_canvas_image.h"
#include "ui/toc_display.h"
#include "test/per_file_debug.h"
#include "device/ui_display.h"
#include "text/bin_font_print.h"
#include "config/config_manager.h"
#include "device/safe_fs.h"
// tag handling (auto/manual tags)
#include "text/tags_handle.h"
// font buffer for page caching
#include "text/font_buffer.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

extern bool autoread;

// Helpers to classify whitespace and count readable UTF-8 codepoints.
static bool is_unicode_whitespace(uint32_t cp)
{
    if (cp <= 0x000D)
    {
        return (cp >= 0x0009 && cp <= 0x000D) || cp == 0x0000 || cp == 0x0001;
    }
    if (cp == 0x0020 || cp == 0x00A0)
        return true;
    if (cp >= 0x2000 && cp <= 0x200A)
        return true;
    if (cp == 0x2028 || cp == 0x2029)
        return true;
    if (cp == 0x202F || cp == 0x205F)
        return true;
    if (cp == 0x3000)
        return true;
    return false;
}

static size_t count_readable_codepoints(const std::string &text)
{
    size_t count = 0;
    size_t len = text.size();
    size_t pos = 0;
    while (pos < len)
    {
        unsigned char b0 = static_cast<unsigned char>(text[pos]);
        uint32_t cp = 0;
        size_t adv = 1;
        if ((b0 & 0x80) == 0)
        {
            cp = b0;
        }
        else if ((b0 & 0xE0) == 0xC0 && pos + 1 < len)
        {
            cp = ((b0 & 0x1F) << 6) | (static_cast<unsigned char>(text[pos + 1]) & 0x3F);
            adv = 2;
        }
        else if ((b0 & 0xF0) == 0xE0 && pos + 2 < len)
        {
            cp = ((b0 & 0x0F) << 12) |
                 (((unsigned char)text[pos + 1] & 0x3F) << 6) |
                 ((unsigned char)text[pos + 2] & 0x3F);
            adv = 3;
        }
        else if ((b0 & 0xF8) == 0xF0 && pos + 3 < len)
        {
            cp = ((b0 & 0x07) << 18) |
                 (((unsigned char)text[pos + 1] & 0x3F) << 12) |
                 (((unsigned char)text[pos + 2] & 0x3F) << 6) |
                 ((unsigned char)text[pos + 3] & 0x3F);
            adv = 4;
        }
        else
        {
            cp = b0;
        }

        if (!is_unicode_whitespace(cp))
        {
            ++count;
        }

        pos += adv;
    }

    return count;
}

// RAII wrapper for File to ensure proper close
class AutoCloseFile
{
private:
    File f;

public:
    AutoCloseFile(File file) : f(file) {}
    ~AutoCloseFile()
    {
        if (f)
            f.close();
    }
    File &get() { return f; }
    operator bool() const { return (bool)f; }
    // Prevent copying
    AutoCloseFile(const AutoCloseFile &) = delete;
    AutoCloseFile &operator=(const AutoCloseFile &) = delete;
};

// Public thin wrappers to allow background task to attempt brief file lock
// acquisition without accessing private methods directly.
bool BookHandle::tryAcquireFileLock(TickType_t timeout)
{
    return acquireFileLock(timeout);
}

void BookHandle::releaseFileLockPublic()
{
    releaseFileLock();
}

// Forward declaration: update history list when opening a SD book file
static bool updateHistoryList(const std::string &book_file_path);
// (index-file removal implemented in removeIndexFilesForBookForPath defined below)

// Small static helpers to build sanitized filenames
static std::string make_sanitized_base(const std::string &book_file_path)
{
    std::string safe;
    safe.assign(book_file_path);
    for (char &c : safe)
    {
        if (c == '/' || c == '\\')
            c = '_';
        else if (c == ':' || c == '?' || c == '*' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    size_t dot = safe.find_last_of('.');
    if (dot != std::string::npos)
        safe = safe.substr(0, dot);
    return safe;
}

static std::string page_filename_for(const std::string &book_file_path)
{
    return std::string("/bookmarks/") + make_sanitized_base(book_file_path) + ".page";
}

static std::string progress_filename_for(const std::string &book_file_path)
{
    return std::string("/bookmarks/") + make_sanitized_base(book_file_path) + ".progress";
}

static std::string complete_filename_for(const std::string &book_file_path)
{
    return std::string("/bookmarks/") + make_sanitized_base(book_file_path) + ".complete";
}

// Forward declaration for helper defined later in this file
static bool page_file_valid_for(const std::string &book_file_path);

// Implementation of public helper to remove index files for a given book path.
// Declared in header as removeIndexFilesForBookForPath.
void removeIndexFilesForBookForPath(const std::string &book_file_path)
{
    // Build sanitized filenames
    std::string safe = book_file_path;
    for (char &c : safe)
    {
        if (c == '/' || c == '\\')
            c = '_';
        else if (c == ':' || c == '?' || c == '*' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    size_t dot = safe.find_last_of('.');
    if (dot != std::string::npos)
        safe = safe.substr(0, dot);

    std::string page_file = std::string("/bookmarks/") + safe + ".page";
    std::string progress_file = std::string("/bookmarks/") + safe + ".progress";
    std::string complete_file = std::string("/bookmarks/") + safe + ".complete";
    std::string rec_file = std::string("/bookmarks/") + safe + ".rec";

    // Only remove the explicit index-related artifacts. Avoid sweeping /bookmarks
    // to prevent accidental deletion of unrelated user files (e.g. .bm or .tags).
    auto try_remove_if_exists = [&](const std::string &fpath)
    {
        if (SDW::SD.exists(fpath.c_str()))
        {
            (void)SDW::SD.remove(fpath.c_str());
        }
    };

    // remove primary index artifacts and their tmp variants if present
    try_remove_if_exists(page_file);
    try_remove_if_exists(progress_file);
    try_remove_if_exists(complete_file);
    try_remove_if_exists(rec_file);

    // also remove tmp variants created by SafeFS (if any)
    try_remove_if_exists(SafeFS::tmpPathFor(page_file));
    try_remove_if_exists(SafeFS::tmpPathFor(progress_file));
    try_remove_if_exists(SafeFS::tmpPathFor(complete_file));
    try_remove_if_exists(SafeFS::tmpPathFor(rec_file));

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] removeIndexFilesForBookForPath: 完成索引文件清理 (sanitized:%s)\n", safe.c_str());
#endif
}

// Constructor / Destructor
BookHandle::BookHandle(const std::string &path, std::int16_t area_w_, std::int16_t area_h_, float fsize,
                       TextEncoding enc)
    : file_path(path), file_handle(), cur_pos(0), area_w(area_w_), area_h(area_h_),
      font_size(get_font_size_from_file()), encoding(enc), // 使用字体文件中的实际大小
      font_cache_initialized(false),                       // 【初始化】字体缓存标志
      history_head(0), history_count(0), current_page_index(0), page_completed(false),
      indexing_in_progress(false), indexing_should_stop(false),
      indexing_current_pos(0), indexing_file_size(0), indexing_start_time(0),
      index_just_completed(false),                // 【初始化】索引刚完成标志
      file_access_mutex(nullptr), showlabel(true) // 显式初始化showlabel
{
#if DBG_BOOK_HANDLE
    Serial.printf("[BH] BookHandle 构造函数: '%s' (区域: %dx%d, 字体: %.1f)\n",
                  file_path.c_str(), area_w, area_h, font_size);
#endif

    // 创建文件访问互斥锁
    file_access_mutex = xSemaphoreCreateMutex();
    if (file_access_mutex == nullptr)
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] BookHandle 构造函数: 互斥锁创建失败\n");
#endif
        return;
    }

    if (!open())
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] BookHandle 构造函数: 文件打开失败\n");
#endif
        return;
    }

    // Try to restore bookmark if available
    bool bookmark_restored = loadBookmarkAndJump();

    // Try to initilaize the paging if not yet
    if (!loadPage())
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] BookHandle 构造函数: 分页加载失败\n");
#endif
        return;
    }

    // 【修复】只有在没有恢复书签时，才强制同步 cur_pos
    // 如果书签已恢复，应该保持书签设置的位置，不要覆盖
    if (!bookmark_restored && !page_positions.empty())
    {
        if (current_page_index >= page_positions.size())
            current_page_index = 0;
        cur_pos = page_positions[current_page_index];
        last_page.success = false; // force reload on first render
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] BookHandle ctor: 无书签，同步 cur_pos -> %zu (index=%zu, total=%zu)\n",
                      cur_pos, current_page_index, page_positions.size());
#endif
    }
    else if (bookmark_restored)
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] BookHandle ctor: 已恢复书签，保持书签位置 cur_pos=%zu (index=%zu, total=%zu)\n",
                      cur_pos, current_page_index, page_positions.size());
#endif
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] BookHandle 构造函数完成: 当前页=%zu, 总页数=%zu\n",
                  current_page_index, page_positions.size());
#endif

    // Prefetch TOC cache to avoid delay when opening TOC UI
    // This scans the .idx file once and caches page offsets
    toc_prefetch_for_book(file_path);
}

// Return an identifier for diagnostics; use the pointer address as unique id.
size_t BookHandle::getId() const
{
    return reinterpret_cast<size_t>(this);
}

BookHandle::~BookHandle()
{
    close();

    // 清理互斥锁
    if (file_access_mutex != nullptr)
    {
        // 在删除互斥锁前，确保索引已经停止，避免在其他任务中仍使用该对象
        if (indexing_in_progress)
        {
#if DBG_BOOK_HANDLE
            Serial.println("[BH] 析构前：检测到索引仍在进行，发送停止并等待...");
#endif
            stopIndexingAndWait(4000);
        }
        vSemaphoreDelete(file_access_mutex);
        file_access_mutex = nullptr;
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] BookHandle 析构函数: 互斥锁已删除\n");
#endif
    }
}

bool BookHandle::open()
{
    if (!file_path.size())
        return false;
    if ((bool)file_handle)
        return true;

    std::string p = file_path;
    bool use_spiffs = false;
    if (p.find("/spiffs/") == 0)
    {
        use_spiffs = true;
        p = p.substr(8);
    }
    if (p.empty() || p[0] != '/')
        p = std::string("/") + p;

    if (use_spiffs)
        file_handle = SPIFFS.open(p.c_str(), "r");
    else
    {
        // expected format: /sd/...
        if (p.rfind("/sd", 0) == 0)
            p = p.substr(3);
        file_handle = SDW::SD.open(p.c_str(), "r");
    }

    if (!file_handle)
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] open failed: %s\n", p.c_str());
#endif
        // Attempt fallback to default file /spiffs/ReadPaper.txt if this isn't already the target
        const std::string default_fp = std::string("/spiffs/ReadPaper.txt");
        if (file_path != default_fp)
        {
            if (SPIFFS.exists("/ReadPaper.txt"))
            {
                File df = SPIFFS.open("/ReadPaper.txt", "r");
                if (df)
                {
                    // Update internal file_path to default and persist to config
                    file_path = default_fp;
                    df.close();
                    config_set_current_file(file_path.c_str());
                    // Mark to skip bookmark restoration because we just fell back to default
                    this->skip_bookmark_on_open = true;
                    // Open default file for real
                    file_handle = SPIFFS.open("/ReadPaper.txt", "r");
                    if ((bool)file_handle)
                    {
#if DBG_BOOK_HANDLE
                        Serial.println("[BH] Fallback: opened default /spiffs/ReadPaper.txt");
#endif
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // 成功打开文件后，若是 SD 路径则更新 /history.list
    if (!use_spiffs)
    {
        // file_path 保持原始格式，如 "/sd/dir/file.ext"
        updateHistoryList(file_path);
    }

    // 【安全防护】如果 .complete 标记存在，确保删除残留的 .progress 文件
    // 这可以防止之前删除失败导致的状态不一致
    std::string complete_marker = getCompleteFileName();
    if (SDW::SD.exists(complete_marker.c_str()))
    {
        std::string progress_file = getProgressFileName();
        if (SDW::SD.exists(progress_file.c_str()))
        {
#if DBG_BOOK_HANDLE
            Serial.printf("[BH:open] .complete exists but found stale .progress, removing: %s\n",
                          progress_file.c_str());
#endif
            SDW::SD.remove(progress_file.c_str());
        }
    }

    // refresh tags cache for this book
    refreshTagsCache();
    // Detect whether a same-directory .idx file exists for this book and record it
    {
        // build expected idx path by replacing extension with .idx
        std::string idx_path = file_path;
        size_t dotpos = idx_path.find_last_of('.');
        if (dotpos != std::string::npos)
            idx_path = idx_path.substr(0, dotpos) + ".idx";
        else
            idx_path += ".idx";

        bool indexed = false;
        // SPIFFS paths use /spiffs/ prefix in file_path; SPIFFS.exists expects a path like "/foo"
        if (file_path.rfind("/spiffs/", 0) == 0)
        {
            std::string rel = std::string("/") + idx_path.substr(8); // remove "/spiffs"
            if (SPIFFS.exists(rel.c_str()))
                indexed = true;
        }
        else
        {
            // For SD and generic FS, try direct exists check. Also try removing leading "/sd" if present.
            if (SDW::SD.exists(idx_path.c_str()))
                indexed = true;
            else if (idx_path.rfind("/sd/", 0) == 0)
            {
                std::string rel = idx_path.substr(3); // remove leading "/sd"
                if (SDW::SD.exists(rel.c_str()))
                    indexed = true;
            }
        }
        is_indexed_ = indexed;
        // If the book has a sidecar .idx file, attempt to preload it into PSRAM-backed cache
        if (is_indexed_)
        {
            // best-effort: load idx positions into memory to speed up lookups
            loadIdxToPSRAM();
        }
    }

    // Warm up TOC cache early so UI entry feels instant
    toc_prefetch_for_book(file_path);

    // 构建TOC专用字体缓存（如果存在.idx文件）
    {
        std::string toc_path = file_path;
        size_t dotpos = toc_path.find_last_of('.');
        if (dotpos != std::string::npos)
            toc_path = toc_path.substr(0, dotpos) + ".idx";
        else
            toc_path += ".idx";

        bool toc_exists = false;
        if (file_path.rfind("/spiffs/", 0) == 0)
        {
            std::string rel = std::string("/") + toc_path.substr(8);
            toc_exists = SPIFFS.exists(rel.c_str());
            if (toc_exists)
                toc_path = std::string("/spiffs") + rel;
        }
        else
        {
            if (SDW::SD.exists(toc_path.c_str()))
            {
                toc_exists = true;
            }
            else if (toc_path.rfind("/sd/", 0) == 0)
            {
                std::string rel = toc_path.substr(3);
                if (SDW::SD.exists(rel.c_str()))
                {
                    toc_exists = true;
                    toc_path = std::string("/sd") + rel;
                }
            }
        }

        if (toc_exists)
        {
            buildTocCharCache(toc_path.c_str());
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] TOC font cache built from: %s\n", toc_path.c_str());
#endif
        }
    }

    // 初始化字体缓存系统（在成功打开书籍后）
    if (isOpen() && pages_loaded)
    {
        g_font_buffer_manager.initialize(this, getCurrentPageIndex());
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] Font buffer initialized for page %u\n", (unsigned)getCurrentPageIndex());
#endif
    }

    return true;
}

void BookHandle::close()
{
    // 清理字体缓存
    g_font_buffer_manager.clearAll();

    // 清理TOC字体缓存
    clearTocCache();

#if DBG_BOOK_HANDLE
    Serial.println("[BH] Font buffer and TOC cache cleared");
#endif

    if ((bool)file_handle)
        file_handle.close();

    // 清理可能加载到 PSRAM 的 idx 缓存
    clearIdxPSRAM();
}

// Load .idx positions into memory (best-effort into PSRAM). Returns true if any positions loaded.
bool BookHandle::loadIdxToPSRAM()
{
    if (!is_indexed_)
        return false;

    // Build idx path by replacing extension
    std::string idx_path = file_path;
    size_t dotpos = idx_path.find_last_of('.');
    if (dotpos != std::string::npos)
        idx_path = idx_path.substr(0, dotpos) + ".idx";
    else
        idx_path += ".idx";

    File idxf;
    if (file_path.rfind("/spiffs/", 0) == 0)
    {
        std::string rel = std::string("/") + idx_path.substr(8);
        if (SPIFFS.exists(rel.c_str()))
            idxf = SPIFFS.open(rel.c_str(), "r");
    }
    else
    {
        if (SDW::SD.exists(idx_path.c_str()))
            idxf = SDW::SD.open(idx_path.c_str(), "r");
        else if (idx_path.rfind("/sd/", 0) == 0)
        {
            std::string rel = idx_path.substr(3);
            if (SDW::SD.exists(rel.c_str()))
                idxf = SDW::SD.open(rel.c_str(), "r");
        }
    }

    if (!idxf)
        return false;

    std::vector<size_t> tmp;
    std::vector<std::string> tmp_titles;
    auto trim_fn = [](std::string &s)
    {
        size_t a = 0;
        while (a < s.size() && isspace((unsigned char)s[a]))
            ++a;
        size_t b = s.size();
        while (b > a && isspace((unsigned char)s[b - 1]))
            --b;
        if (b <= a)
        {
            s.clear();
            return;
        }
        s = s.substr(a, b - a);
    };
    while (idxf.available())
    {
        String s = idxf.readStringUntil('\n');
        std::string line = std::string(s.c_str());
        // trim CR
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        // Try parsing format like: #序号#, #标题#, #字节位置#, #百分比#,
        std::vector<size_t> hash_pos;
        for (size_t i = 0; i < line.size(); ++i)
        {
            if (line[i] == '#')
                hash_pos.push_back(i);
        }
        if (hash_pos.size() >= 8)
        {
            std::string title_str = line.substr(hash_pos[2] + 1, hash_pos[3] - hash_pos[2] - 1);
            std::string pos_str = line.substr(hash_pos[4] + 1, hash_pos[5] - hash_pos[4] - 1);
            // trim spaces
            trim_fn(title_str);
            trim_fn(pos_str);
            if (!pos_str.empty())
            {
                const char *cstr = pos_str.c_str();
                char *endptr = nullptr;
                unsigned long long val = strtoull(cstr, &endptr, 10);
                if (endptr != cstr)
                {
                    tmp.push_back((size_t)val);
                    tmp_titles.push_back(title_str);
                }
            }
            continue;
        }
        // Fallback: try comma-separated style, take second field as title and third as pos
        size_t p1 = line.find(',');
        if (p1 == std::string::npos)
            continue;
        size_t p2 = line.find(',', p1 + 1);
        if (p2 == std::string::npos)
            continue;
        size_t p3 = line.find(',', p2 + 1);
        if (p3 == std::string::npos)
            p3 = line.length();
        std::string title_field = line.substr(p1 + 1, p2 - p1 - 1);
        std::string pos_field = line.substr(p2 + 1, p3 - p2 - 1);
        trim_fn(title_field);
        trim_fn(pos_field);
        if (pos_field.empty())
            continue;
        const char *cstr = pos_field.c_str();
        char *endptr = nullptr;
        unsigned long long val = strtoull(cstr, &endptr, 10);
        if (endptr == cstr)
            continue;
        tmp.push_back((size_t)val);
        tmp_titles.push_back(title_field);
    }
    idxf.close();

    if (tmp.empty())
        return false;

    // store into vector cache
    idx_positions_psram_ = tmp;
    idx_titles_psram_ = tmp_titles;

#ifdef ESP_PLATFORM
    // Try to allocate PSRAM-backed raw buffer (best-effort). If fails, keep vector only.
    size_t bytes = tmp.size() * sizeof(size_t);
    void *ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr)
    {
        memcpy(ptr, tmp.data(), bytes);
        idx_positions_psram_ptr = (size_t *)ptr;
        idx_positions_psram_count = tmp.size();
        idx_psram_loaded_ = true;
    }
    else
    {
        // allocation failed; keep vector in normal heap
        idx_positions_psram_ptr = nullptr;
        idx_positions_psram_count = idx_positions_psram_.size();
        idx_psram_loaded_ = false;
    }
#else
    idx_positions_psram_ptr = nullptr;
    idx_positions_psram_count = idx_positions_psram_.size();
    idx_psram_loaded_ = false;
#endif

    return true;
}

void BookHandle::clearIdxPSRAM()
{
    // free raw PSRAM buffer if allocated
#ifdef ESP_PLATFORM
    if (idx_positions_psram_ptr)
    {
        heap_caps_free(idx_positions_psram_ptr);
        idx_positions_psram_ptr = nullptr;
    }
#else
    (void)idx_positions_psram_ptr;
#endif
    idx_positions_psram_count = 0;
    idx_psram_loaded_ = false;
    // clear in-heap vector
    idx_positions_psram_.clear();
    idx_titles_psram_.clear();
    idx_positions_psram_.shrink_to_fit();
}

// 标记对象正在被关闭，供后台索引器安全退出
void BookHandle::markForClose()
{
    // Before marking closed, save an automatic tag (slot 0) with current position
    // so the last-read position is preserved for this book.
    TextPageResult tp = currentPage();
    if (tp.success)
    {
        // use file_path (member) as the book identifier
        insertAutoTagForFile(file_path, tp.file_pos);
        // refresh in-memory cache so UI reflects new auto tag
        refreshTagsCache();
    }

    closing_ = true;
}

bool BookHandle::isClosing() const
{
    return closing_;
}

std::string BookHandle::getCompleteFileName() const
{
    if (!sanitized_base_.empty())
        return std::string("/bookmarks/") + sanitized_base_ + ".complete";
    return complete_filename_for(file_path);
}

std::string BookHandle::getBookName() const
{
    // 从文件路径中提取文件名
    size_t last_slash = file_path.find_last_of('/');
    std::string filename;
    if (last_slash != std::string::npos && last_slash + 1 < file_path.length())
    {
        filename = file_path.substr(last_slash + 1);
    }
    else
    {
        filename = file_path; // 如果没有斜杠，使用整个路径
    }

    // 去掉扩展名
    size_t last_dot = filename.find_last_of('.');
    if (last_dot != std::string::npos && last_dot > 0)
    {
        return filename.substr(0, last_dot);
    }
    return filename;
}

bool BookHandle::isOpen() const { return (bool)file_handle; }

void BookHandle::setPosition(size_t pos)
{
    cur_pos = pos;
    // history intentionally removed
}

size_t BookHandle::position() const { return cur_pos; }

size_t BookHandle::getFileSize() const
{
    if (indexing_file_size > 0)
    {
        return indexing_file_size;
    }
    // 如果索引文件大小未知，直接从文件系统获取
    std::string path = file_path;
    File temp_file;
    if (path.substr(0, 3) == "/sd")
    {
        path = path.substr(3); // 移除 /sd 前缀
        temp_file = SDW::SD.open(path.c_str(), "r");
    }
    if (path.substr(0, 7) == "/spiffs")
    {
        path = path.substr(7); // 移除 /sd 前缀
        temp_file = SPIFFS.open(path.c_str(), "r");
    }

    if (temp_file)
    {
        temp_file.seek(0, SeekEnd);
        size_t file_size = temp_file.position();
        temp_file.close();
        return file_size;
    }
    return 0;
}

// nextPage: 基于页面索引的下一页翻页
TextPageResult BookHandle::nextPage()
{
    TextPageResult res;
    if (!isOpen())
    {
        if (!open())
        {
            res.success = false;
            return res;
        }
    }

    // 确保分页信息已加载
    if (!pages_loaded)
    {
        Serial.printf("[BH] nextPage: pages_loaded=false, attempting to load/generate\n");
        if (!loadPageFile())
        {
            // 如果没有分页文件，生成一个
            if (!generatePageFile())
            {
                res.success = false;
                return res;
            }
        }

        // 首次加载分页后，初始化字体缓存
        Serial.printf("[BH] nextPage: After load/generate - pages_loaded=%d, total_pages=%zu\n",
                      pages_loaded ? 1 : 0, page_positions.size());
        if (pages_loaded && !g_font_buffer_manager.isInitialized())
        {
            Serial.printf("[BH] nextPage: Initializing font buffer for page %u\n",
                          (unsigned)getCurrentPageIndex());
            g_font_buffer_manager.initialize(this, getCurrentPageIndex());
        }
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] nextPage: current_page_index=%zu, total_pages=%zu, hasNext=%s\n",
                  current_page_index, page_positions.size(), hasNextPage() ? "true" : "false");
    Serial.printf("[BH] nextPage: 调用开始 - 时间戳: %lu\n", millis());
    Serial.printf("[BH] nextPage: indexing_complete=%s, indexing_in_progress=%s\n",
                  isIndexingComplete() ? "true" : "false", indexing_in_progress ? "true" : "false");
#endif

    // 检查是否有下一页
    if (!hasNextPage())
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] nextPage: 没有下一页 (current=%zu, total=%zu)\n",
                      current_page_index, page_positions.size());
#endif
        // 【简化逻辑】如果没有下一页，直接返回失败，不干扰索引过程
        // 索引会在后台自动进行，用户稍后可以再次尝试翻页
        res.success = false;
        return res;
    }

    // 移动到下一页 - 先验证下一个偏移是否有效
    size_t desired_index = current_page_index + 1;
    size_t next_pos = 0;
    if (desired_index < page_positions.size())
        next_pos = page_positions[desired_index];

    // If the next_pos is not greater than current cur_pos, it may be a stale/zero entry
    if (next_pos <= cur_pos)
    {
        // If indexing is in progress, wait briefly for the indexer to append offsets
        if (indexing_in_progress)
        {
            unsigned long wait_start = millis();
            const unsigned long WAIT_MS = 200; // short wait
            while ((millis() - wait_start) < WAIT_MS)
            {
                // give indexer a chance to progress
                taskYIELD();
                vTaskDelay(pdMS_TO_TICKS(5));
                if (desired_index < page_positions.size())
                {
                    next_pos = page_positions[desired_index];
                    if (next_pos > cur_pos)
                        break;
                }
            }
        }
    }

    // If still invalid, compute the next page synchronously from current cur_pos
    if (next_pos <= cur_pos)
    {
        // Attempt to compute next page by reading the page at cur_pos
        if (!acquireFileLock(pdMS_TO_TICKS(5000)))
        {
            res.success = false;
            return res;
        }
        size_t saved_pos = saveCurrentPosition();
        // 使用全局 font_size 确保字体切换后立即生效
        extern float font_size;
        // Calculate boundary for reading (next page start if exists)
        size_t tmp_max_pos = SIZE_MAX;
        if (pages_loaded && desired_index < page_positions.size() && desired_index + 1 < page_positions.size())
        {
            tmp_max_pos = page_positions[desired_index + 1];
        }
        TextPageResult tmp = read_text_page(file_handle, file_path, cur_pos, area_w, area_h, font_size, encoding, false, getVerticalText(), tmp_max_pos);
        restorePosition(saved_pos);
        releaseFileLock();
        if (tmp.success && tmp.page_end_pos > cur_pos)
        {
            next_pos = tmp.page_end_pos;
            // 如果检测到了编码，同步更新
            if (encoding == TextEncoding::AUTO_DETECT && g_text_state.encoding != TextEncoding::AUTO_DETECT)
            {
                encoding = g_text_state.encoding;
            }
            // insert into page_positions at desired_index
            if (desired_index <= page_positions.size())
            {
                page_positions.insert(page_positions.begin() + desired_index, next_pos);
                // persist minimal index progress (best-effort)
                savePageFile();
            }
        }
        else
        {
            // cannot obtain next page
            res.success = false;
            return res;
        }
    }

    // OK: advance to next page
    size_t old_page_index = current_page_index;
    current_page_index = desired_index;
    cur_pos = next_pos;
    page_completed = false; // 新页面未完成

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] nextPage: 跳转到页面 %zu, 位置 %zu\n", current_page_index, cur_pos);
#endif

    // 更新字体缓存（滚动更新）
    if (g_font_buffer_manager.isInitialized())
    {
        g_font_buffer_manager.scrollUpdate(this, current_page_index, true);
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] Font buffer updated: %u -> %u (forward)\n",
                      (unsigned)old_page_index, (unsigned)current_page_index);
#endif
    }

    // 读取页面内容 - 使用安全的文件访问
    if (!acquireFileLock(pdMS_TO_TICKS(5000)))
    { // 5秒超时
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] nextPage: 无法获取文件锁\n");
#endif
        res.success = false;
        return res;
    }

    // 保存当前文件位置
    size_t saved_pos = saveCurrentPosition();

    // 使用全局 font_size 确保字体切换后立即生效
    extern float font_size;

    // Determine next page boundary to limit reading
    size_t max_byte_pos = SIZE_MAX;
    if (pages_loaded && current_page_index < page_positions.size())
    {
        if (current_page_index + 1 < page_positions.size())
        {
            max_byte_pos = page_positions[current_page_index + 1];
        }
    }

    res = read_text_page(file_handle, file_path, cur_pos, area_w, area_h, font_size, encoding, false, getVerticalText(), max_byte_pos);

    // 恢复文件位置（如果需要）
    restorePosition(saved_pos);

    // 释放文件锁
    releaseFileLock();
    if (res.success)
    {
        last_page = res;
        updateCurrentDigest(res.page_text);

        // 如果之前是 AUTO_DETECT，现在已检测出具体编码，同步到 BookHandle
        if (encoding == TextEncoding::AUTO_DETECT && g_text_state.encoding != TextEncoding::AUTO_DETECT)
        {
            encoding = g_text_state.encoding;
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] nextPage: 编码检测完成，更新为 %d\n", (int)encoding);
#endif
        }

        // 检查是否读取了完整页面
        if (current_page_index + 1 < page_positions.size())
        {
            // 不是最后一页，检查是否读到了下一页的起始位置
            page_completed = (res.page_end_pos >= page_positions[current_page_index + 1]);
        }
        else
        {
            // 是最后一页，检查是否读到文件末尾
            file_handle.seek(0, SeekEnd);
            size_t file_size = file_handle.position();
            page_completed = (res.page_end_pos >= file_size);
        }

#if DBG_BOOK_HANDLE
        Serial.printf("[BH] nextPage: 读取成功，page_end_pos=%zu, page_completed=%s\n",
                      res.page_end_pos, page_completed ? "true" : "false");
#endif

        // saveBookmark() 移到render之后调用，降低翻页延迟
    }
    else
    {
        // 读取失败，回退页面索引
        current_page_index--;
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] nextPage: 读取失败，回退到页面 %zu\n", current_page_index);
#endif
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] nextPage: 调用结束 - 时间戳: %lu, success=%s\n", millis(), res.success ? "true" : "false");
#endif

    return res;
}

// prevPage: 基于页面索引的上一页翻页
TextPageResult BookHandle::prevPage()
{
    TextPageResult res;
    if (!isOpen())
    {
        if (!open())
        {
            res.success = false;
            return res;
        }
    }

    // 确保分页信息已加载
    if (!pages_loaded)
    {
        Serial.printf("[BH] prevPage: pages_loaded=false, attempting to load/generate\n");
        if (!loadPageFile())
        {
            // 如果没有分页文件，生成一个
            if (!generatePageFile())
            {
                res.success = false;
                return res;
            }
        }

        // 首次加载分页后，初始化字体缓存
        Serial.printf("[BH] prevPage: After load/generate - pages_loaded=%d, total_pages=%zu\n",
                      pages_loaded ? 1 : 0, page_positions.size());
        if (pages_loaded && !g_font_buffer_manager.isInitialized())
        {
            Serial.printf("[BH] prevPage: Initializing font buffer for page %u\n",
                          (unsigned)getCurrentPageIndex());
            g_font_buffer_manager.initialize(this, getCurrentPageIndex());
        }
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] prevPage: current_page_index=%zu, total_pages=%zu, hasPrev=%s\n",
                  current_page_index, page_positions.size(), hasPrevPage() ? "true" : "false");
#endif

    // 检查是否有上一页
    if (!hasPrevPage())
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] prevPage: 已是第一页 (index=%zu)\n", current_page_index);
#endif
        res.success = false;
        return res;
    }

    // 移动到上一页
    size_t old_page_index = current_page_index;
    current_page_index--;
    cur_pos = page_positions[current_page_index];
    page_completed = false; // 重新加载页面

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] prevPage: 跳转到页面 %zu, 位置 %zu\n", current_page_index, cur_pos);
#endif

    // 更新字体缓存（滚动更新）
    if (g_font_buffer_manager.isInitialized())
    {
        g_font_buffer_manager.scrollUpdate(this, current_page_index, false);
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] Font buffer updated: %u -> %u (backward)\n",
                      (unsigned)old_page_index, (unsigned)current_page_index);
#endif
    }

    // 读取页面内容 - 使用安全的文件访问
    if (!acquireFileLock(pdMS_TO_TICKS(5000)))
    { // 5秒超时
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] prevPage: 无法获取文件锁\n");
#endif
        res.success = false;
        return res;
    }

    // 保存当前文件位置
    size_t saved_pos = saveCurrentPosition();

    // 使用全局 font_size 确保字体切换后立即生效
    extern float font_size;

    // Determine next page boundary to limit reading
    size_t max_byte_pos = SIZE_MAX;
    if (pages_loaded && current_page_index < page_positions.size())
    {
        if (current_page_index + 1 < page_positions.size())
        {
            max_byte_pos = page_positions[current_page_index + 1];
        }
    }

    res = read_text_page(file_handle, file_path, cur_pos, area_w, area_h, font_size, encoding, false, getVerticalText(), max_byte_pos);

    // 恢复文件位置（如果需要）
    restorePosition(saved_pos);

    // 释放文件锁
    releaseFileLock();
    if (res.success)
    {
        last_page = res;
        updateCurrentDigest(res.page_text);

        // 如果之前是 AUTO_DETECT，现在已检测出具体编码，同步到 BookHandle
        if (encoding == TextEncoding::AUTO_DETECT && g_text_state.encoding != TextEncoding::AUTO_DETECT)
        {
            encoding = g_text_state.encoding;
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] prevPage: 编码检测完成，更新为 %d\n", (int)encoding);
#endif
        }

        // 检查是否读取了完整页面
        if (current_page_index + 1 < page_positions.size())
        {
            // 不是最后一页，检查是否读到了下一页的起始位置
            page_completed = (res.page_end_pos >= page_positions[current_page_index + 1]);
        }
        else
        {
            // 是最后一页，检查是否读到文件末尾
            file_handle.seek(0, SeekEnd);
            size_t file_size = file_handle.position();
            page_completed = (res.page_end_pos >= file_size);
        }

#if DBG_BOOK_HANDLE
        Serial.printf("[BH] prevPage: 读取成功，page_end_pos=%zu, page_completed=%s\n",
                      res.page_end_pos, page_completed ? "true" : "false");
#endif

        // saveBookmark() 移到render之后调用，降低翻页延迟

#if DBG_BOOK_HANDLE
        Serial.printf("[BH] prevPage page_index=%zu page_text:\n%s\n", current_page_index, res.page_text.c_str());
#endif
    }
    else
    {
        // 读取失败，回退页面索引
        current_page_index++;
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] prevPage: 读取失败，回退到页面 %zu\n", current_page_index);
#endif
    }

    return res;
}

TextPageResult BookHandle::currentPage()
{
    if (last_page.success && last_page.file_pos == cur_pos)
        return last_page;

    TextPageResult res;
    if (!isOpen())
    {
        if (!open())
        {
            res.success = false;
            return res;
        }
    }

    // 使用安全的文件访问
    if (!acquireFileLock(pdMS_TO_TICKS(5000)))
    { // 5秒超时
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] currentPage: 无法获取文件锁\n");
#endif
        res.success = false;
        return res;
    }

    // 保存当前文件位置
    size_t saved_pos = saveCurrentPosition();

    // 使用全局 font_size 而非成员变量，确保切换字体后立即生效
    extern float font_size;
    // 同步更新成员变量，确保后续保存书签时使用正确的值
    if (fabs(this->font_size - font_size) > 0.01f)
    {
        this->font_size = font_size;
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] currentPage: 检测到字体大小变化，更新为 %.2f\n", font_size);
#endif
    }

    // Determine next page boundary to limit reading within current page
    size_t max_byte_pos = SIZE_MAX;
    if (pages_loaded && current_page_index < page_positions.size())
    {
        // If there's a next page, use its start position as the boundary
        if (current_page_index + 1 < page_positions.size())
        {
            max_byte_pos = page_positions[current_page_index + 1];
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] currentPage: limit reading to next page start %zu\n", max_byte_pos);
#endif
        }
    }

    res = read_text_page(file_handle, file_path, cur_pos, area_w, area_h, font_size, encoding, false, getVerticalText(), max_byte_pos);

    // 恢复文件位置（如果需要）
    restorePosition(saved_pos);

    // 释放文件锁
    releaseFileLock();
    if (res.success)
    {
        last_page = res;
        updateCurrentDigest(res.page_text);

        // 如果之前是 AUTO_DETECT，现在已检测出具体编码，同步到 BookHandle
        // 避免下次翻页时重复检测，节省时间
        if (encoding == TextEncoding::AUTO_DETECT && g_text_state.encoding != TextEncoding::AUTO_DETECT)
        {
            encoding = g_text_state.encoding;
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] currentPage: 编码检测完成，更新为 %d\n", (int)encoding);
#endif
        }
    }
    return res;
}

void BookHandle::clearHistory()
{
    // 清理历史时也删除与当前书籍相关的索引文件
    removeIndexFilesForBookForPath(file_path);
    // 关闭文件句柄
    close();
}

// jumpToPage: 跳转到指定页面索引
bool BookHandle::jumpToPage(size_t page_index)
{
    if (!isOpen())
    {
        if (!open())
        {
            return false;
        }
    }
    // 确保分页信息已加载
    if (!pages_loaded)
    {
        if (!loadPageFile())
        {
            if (!generatePageFile())
            {
                return false;
            }
        }
    }

    // 检查页面索引是否有效
    if (page_index >= page_positions.size())
    {
        return false;
    }

    // 跳转到指定页面
    current_page_index = page_index;
    cur_pos = page_positions[current_page_index];

    // 清除缓存的页面，强制重新读取
    last_page.success = false;

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] jumpToPage: 跳转到页面 %zu, 位置 %zu, page_positions.size=%zu\n", page_index, cur_pos, page_positions.size());
#endif

    // saveBookmark() 移到render之后调用，降低跳转延迟

    // 更新字体缓存（滚动更新）- 如果未被锁定
    if (g_font_buffer_manager.isInitialized() && !g_font_buffer_manager.isInitializationLocked())
    {
        size_t old_page = g_font_buffer_manager.getCurrentPageIndex();
        bool forward = (page_index > old_page);
        g_font_buffer_manager.scrollUpdate(this, page_index, forward);
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] Font buffer updated: %u -> %u (forward=%d)\n",
                      (unsigned)old_page, (unsigned)page_index, forward ? 1 : 0);
#endif
    }

    // 如果当前正在索引，但书签/字体/显示参数与当前不匹配，尝试在跳转后模拟强制重更新索引。
    // 场景：字体在外部被替换，但后台索引仍在用旧参数进行，需停止并触发重建。
    if (indexing_in_progress)
    {
        // 读取书签并比较关键参数
        BookmarkConfig cfg = loadBookmarkForFile(file_path);
        if (cfg.valid)
        {
            // 使用当前加载的字体大小（而非BookHandle构造时的font_size）来检测变化
            uint8_t current_font_file_size = get_font_size_from_file();
            bool font_size_changed = (cfg.font_base_size > 0 && current_font_file_size > 0 && cfg.font_base_size != current_font_file_size);
            // Note: font_name and font_version comparison removed - only font size matters for re-indexing
            // Same-size fonts assumed to have similar pagination impact
            bool area_changed = (cfg.area_width != area_w || cfg.area_height != area_h);
            bool encoding_changed = (cfg.encoding != encoding);

            if (font_size_changed || area_changed || encoding_changed)
            {
                // 请求停止当前索引并等待（最长5s），然后强制重建索引
                Serial.println("[BH] jumpToPage: 检测到书签参数与当前不匹配且正在索引，尝试停止并强制重建索引");
                bool stopped = stopIndexingAndWait(5000);
                if (!stopped)
                {
                    Serial.println("[BH] jumpToPage: 停止旧索引超时，仍将尝试强制重建索引");
                }
                // 更新 BookHandle 的 font_size 以反映当前加载的字体
                extern float font_size;
                this->font_size = font_size;
                // 请求后台任务执行强制重新建立索引（避免 UI 阻塞）
                extern void requestForceReindex();
                requestForceReindex();
            }
        }
    }

    return true;
}

void BookHandle::updateCurrentDigest(const std::string &page_text)
{
    current_digest.clear();
    if (page_text.empty())
        return;

    // skip leading whitespace (spaces, tabs, newlines, CR, etc.)
    size_t n = page_text.size();
    size_t pos = 0;
    while (pos < n && std::isspace((unsigned char)page_text[pos]))
        ++pos;
    if (pos >= n)
        return;

    // approximate byte limit for DIGEST_NUM codepoints
    size_t max_bytes = std::min((size_t)DIGEST_NUM * 3, n);
    size_t end_limit = std::min(n, pos + max_bytes);

    current_digest.reserve(std::min((size_t)256, end_limit - pos)); // small reserve

    int newline_count = 0;
    int codepoints = 0;

    while (pos < end_limit && codepoints < DIGEST_NUM && newline_count < 3)
    {
        // find end of current line (up to end_limit)
        size_t line_end = pos;
        while (line_end < end_limit && page_text[line_end] != '\n')
            ++line_end;

        // check if the line is entirely whitespace (a blank line)
        bool blank = true;
        for (size_t k = pos; k < line_end; ++k)
        {
            if (!std::isspace((unsigned char)page_text[k]))
            {
                blank = false;
                break;
            }
        }

        if (blank)
        {
            // skip blank line entirely (do not count its newline)
            pos = (line_end < end_limit) ? line_end + 1 : line_end;
            continue;
        }

        // append bytes of the line, counting UTF-8 codepoints approximately
        for (size_t b = pos; b < line_end && codepoints < DIGEST_NUM; ++b)
        {
            unsigned char c = page_text[b];
            if ((c & 0xC0) != 0x80)
                ++codepoints; // start of UTF-8 codepoint
            current_digest.push_back(page_text[b]);
        }

        // handle newline if it exists within limits
        if (line_end < end_limit)
        {
            // attempt to append the newline only if it doesn't exceed DIGEST_NUM
            if (codepoints < DIGEST_NUM)
            {
                ++newline_count;
                if (newline_count >= 3)
                    break; // stop after including the 3rd newline
                current_digest.push_back('\n');
                ++codepoints; // newline is a codepoint
                pos = line_end + 1;
            }
            else
            {
                // cannot append newline due to DIGEST_NUM limit -> stop
                break;
            }
        }
        else
        {
            // reached end_limit or EOF for this slice
            pos = line_end;
            break;
        }
    }

    // Ensure we didn't end with a truncated UTF-8 sequence.
    // It's possible end_limit cut inside a multi-byte character and
    // the loop above copied continuation bytes at the end. Remove any
    // trailing continuation bytes and, if the leading byte is incomplete,
    // remove it as well so the string always ends on a valid UTF-8 boundary.
    if (!current_digest.empty())
    {
        // remove trailing continuation bytes (0x80..0xBF)
        while (!current_digest.empty() && (((unsigned char)current_digest.back() & 0xC0) == 0x80))
        {
            current_digest.pop_back();
        }

        if (!current_digest.empty())
        {
            // find the start byte of the last UTF-8 sequence
            size_t len = current_digest.size();
            size_t i = len - 1;
            while (i > 0 && (((unsigned char)current_digest[i] & 0xC0) == 0x80))
                --i;

            unsigned char lead = (unsigned char)current_digest[i];
            size_t expected_len = 1;
            if ((lead & 0x80) == 0x00)
                expected_len = 1;
            else if ((lead & 0xE0) == 0xC0)
                expected_len = 2;
            else if ((lead & 0xF0) == 0xE0)
                expected_len = 3;
            else if ((lead & 0xF8) == 0xF0)
                expected_len = 4;

            size_t available = len - i;
            if (available < expected_len)
            {
                // incomplete leading byte -> truncate it as well
                current_digest.resize(i);
            }
        }
    }
}

size_t BookHandle::getCurrentPageCharCount() const
{
    if (last_render_char_count_ > 0)
        return last_render_char_count_;
    if (last_page.success)
        return count_readable_codepoints(last_page.page_text);
    return 0;
}

// Bookmark helpers
bool ensureBookmarksFolder()
{
    if (!SDW::SD.exists("/bookmarks"))
    {
        return SDW::SD.mkdir("/bookmarks");
    }
    return true;
}

// Screenshot folder helper
bool ensureScreenshotFolder()
{
    if (!SDW::SD.exists("/screenshot"))
    {
        return SDW::SD.mkdir("/screenshot");
    }
    return true;
}

// 更新 /history.list：把当前打开的书路径插到文件头，去重并移除不存在的条目
// 仅接受以 /sd/book/ 开头的路径
static bool updateHistoryList(const std::string &book_file_path)
{
    const char *HISTORY = "/history.list";
    const char *TMP = "/history.list.tmp";
    const size_t MAX_ENTRIES = 20; // 限制历史记录长度

    // 标准化路径：确保以 /sd 开头
    std::string normalized_path = book_file_path;
    if (normalized_path.rfind("/sd", 0) != 0)
    {
        // 如果路径不是以 /sd 开头，添加前缀
        if (normalized_path[0] != '/')
            normalized_path = "/" + normalized_path;
        if (normalized_path.rfind("/sd/", 0) != 0)
            normalized_path = "/sd" + normalized_path;
    }

    // 只接受 /sd/book/ 开头的路径
    if (normalized_path.rfind("/sd/book/", 0) != 0)
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] updateHistoryList: 路径不符合要求 (必须以 /sd/book/ 开头): %s\n", normalized_path.c_str());
#endif
        return false;
    }

    std::vector<std::string> old_lines;

    // 读取已有历史（如果存在）
    if (SDW::SD.exists(HISTORY))
    {
        AutoCloseFile f(SDW::SD.open(HISTORY, "r"));
        if (f)
        {
            while (f.get().available())
            {
                String line = f.get().readStringUntil('\n');
                line.trim();
                if (line.length() == 0)
                    continue;
                old_lines.push_back(std::string(line.c_str()));
            }
            // File will be automatically closed when f goes out of scope
        }
    }

    // 构建新的历史列表，首行为当前打开的路径
    std::vector<std::string> new_lines;
    new_lines.reserve(std::min(old_lines.size() + 1, (size_t)MAX_ENTRIES));
    new_lines.push_back(normalized_path);

    for (const auto &ln : old_lines)
    {
        if (ln == normalized_path)
            continue; // 去重

        // 只保留 /sd/book/ 开头的路径
        if (ln.rfind("/sd/book/", 0) != 0)
            continue;

        // 检查文件是否存在
        std::string p = ln.substr(3); // 移除 /sd 前缀
        bool exists = SDW::SD.exists(p.c_str());

        if (!exists)
            continue; // 移除不存在的条目

        if (new_lines.size() >= MAX_ENTRIES)
            break;
        new_lines.push_back(ln);
    }

    // 写入临时文件
    File tf = SDW::SD.open(TMP, "w");
    if (!tf)
        return false;
    for (const auto &s : new_lines)
    {
        tf.println(s.c_str());
    }
    tf.close();

    // 原子替换历史文件：尝试 rename，若失败则直接覆盖
    if (!SDW::SD.rename(TMP, HISTORY))
    {
        // 备用方式：覆盖目标文件
        File hf = SDW::SD.open(HISTORY, "w");
        if (!hf)
        {
            // 清理临时文件
            SDW::SD.remove(TMP);
            return false;
        }
        for (const auto &s : new_lines)
            hf.println(s.c_str());
        hf.close();
        SDW::SD.remove(TMP);
    }

    return true;
}

// 从 /history.list 中删除指定书籍路径
static bool removeFromHistoryList(const std::string &book_file_path)
{
    const char *HISTORY = "/history.list";
    const char *TMP = "/history.list.tmp";

    // 标准化路径：确保以 /sd 开头
    std::string normalized_path = book_file_path;
    if (normalized_path.rfind("/sd", 0) != 0)
    {
        // 如果路径不是以 /sd 开头，添加前缀
        if (normalized_path[0] != '/')
            normalized_path = "/" + normalized_path;
        if (normalized_path.rfind("/sd/", 0) != 0)
            normalized_path = "/sd" + normalized_path;
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] removeFromHistoryList: 删除路径 '%s'\n", normalized_path.c_str());
#endif

    if (!SDW::SD.exists(HISTORY))
    {
#if DBG_BOOK_HANDLE
        Serial.println("[BH] removeFromHistoryList: history.list 不存在");
#endif
        return true; // 没有历史文件，视为成功
    }

    std::vector<std::string> old_lines;

    // 读取已有历史
    {
        AutoCloseFile f(SDW::SD.open(HISTORY, "r"));
        if (f)
        {
            while (f.get().available())
            {
                String line = f.get().readStringUntil('\n');
                line.trim();
                if (line.length() == 0)
                    continue;
                old_lines.push_back(std::string(line.c_str()));
            }
        }
    }

    // 过滤掉要删除的路径
    std::vector<std::string> new_lines;
    bool found = false;
    for (const auto &ln : old_lines)
    {
        if (ln == normalized_path)
        {
            found = true;
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] removeFromHistoryList: 找到并跳过 '%s'\n", ln.c_str());
#endif
            continue; // 跳过要删除的条目
        }
        new_lines.push_back(ln);
    }

    if (!found)
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] removeFromHistoryList: 未找到路径 '%s'，无需修改\n", normalized_path.c_str());
#endif
        return true; // 没找到也算成功
    }

    // 写入临时文件
    File tf = SDW::SD.open(TMP, "w");
    if (!tf)
    {
#if DBG_BOOK_HANDLE
        Serial.println("[BH] removeFromHistoryList: 无法创建临时文件");
#endif
        return false;
    }
    for (const auto &s : new_lines)
    {
        tf.println(s.c_str());
    }
    tf.close();

    // 原子替换历史文件
    if (!SDW::SD.rename(TMP, HISTORY))
    {
        // 备用方式：覆盖目标文件
        File hf = SDW::SD.open(HISTORY, "w");
        if (!hf)
        {
            SDW::SD.remove(TMP);
#if DBG_BOOK_HANDLE
            Serial.println("[BH] removeFromHistoryList: 替换失败");
#endif
            return false;
        }
        for (const auto &s : new_lines)
            hf.println(s.c_str());
        hf.close();
        SDW::SD.remove(TMP);
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] removeFromHistoryList: 成功删除 '%s'\n", normalized_path.c_str());
#endif
    return true;
}

// 公共接口：从 history.list 中删除指定书籍
bool removeBookFromHistory(const std::string &book_path)
{
    return removeFromHistoryList(book_path);
}

std::string getBookmarkFileName(const std::string &book_file_path)
{
    // 为避免不同路径下同名文件的书签冲突，使用完整路径生成书签文件名
    std::string safe_path = book_file_path;

    // 将路径中的特殊字符替换为安全字符
    for (char &c : safe_path)
    {
        if (c == '/' || c == '\\')
        {
            c = '_';
        }
        else if (c == ':' || c == '?' || c == '*' || c == '<' || c == '>' || c == '|')
        {
            c = '_';
        }
    }

    // 去掉文件扩展名
    size_t dot = safe_path.find_last_of('.');
    if (dot != std::string::npos)
    {
        safe_path = safe_path.substr(0, dot);
    }

    return std::string("/bookmarks/") + safe_path + ".bm";
}

std::string getRecordFileName(const std::string &book_file_path)
{
    // 复用 getBookmarkFileName 的路径转换逻辑，只是后缀改为 .rec
    std::string safe_path = book_file_path;

    for (char &c : safe_path)
    {
        if (c == '/' || c == '\\')
        {
            c = '_';
        }
        else if (c == ':' || c == '?' || c == '*' || c == '<' || c == '>' || c == '|')
        {
            c = '_';
        }
    }

    size_t dot = safe_path.find_last_of('.');
    if (dot != std::string::npos)
    {
        safe_path = safe_path.substr(0, dot);
    }

    return std::string("/bookmarks/") + safe_path + ".rec";
}

bool saveBookmarkForFile(BookHandle *book)
{
    if (!book)
        return false;
    if (!ensureBookmarksFolder())
        return false;
    std::string fn = getBookmarkFileName(book->filePath());

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] saveBookmarkForFile: 保存书签 - 路径='%s', 页码=%zu, 位置=%zu\n",
                  book->filePath().c_str(), book->getCurrentPageIndex(), book->position());
#endif

    // 在写入新的 bm 文件之前，先读取旧的阅读时长（用于计算增量）
    BookmarkConfig old_cfg = loadBookmarkForFile(book->filePath());
    int16_t old_hour = old_cfg.valid ? old_cfg.readhour : 0;
    int16_t old_min = old_cfg.valid ? old_cfg.readmin : 0;

    // .bm 文件记录当前阅读位置，应该实时更新，不受"最大进度保护"约束
    // "最大进度保护"仅适用于 .tags 文件的第一个（auto）标签

    bool ok = SafeFS::safeWrite(fn, [&](File &f)
                                {
        f.printf("file_path=%s\n", book->filePath().c_str());
        f.printf("current_position=%zu\n", book->position());
        f.printf("file_size=%zu\n", book->getFileSize()); // 添加文件大小记录
        f.printf("area_width=%d\n", book->getAreaWidth());
        f.printf("area_height=%d\n", book->getAreaHeight());
        f.printf("font_size=%.2f\n", book->getFontSize());
        f.printf("font_name=%s\n", get_current_font_name());        // 保存字体名称
        f.printf("font_version=%u\n", get_font_version());          // 保存字体版本
        f.printf("font_base_size=%u\n", get_font_size_from_file()); // 保存字体文件基础尺寸
        f.printf("encoding=%d\n", (int)book->getEncoding());

        // 添加页面索引相关信息
        f.printf("current_page_index=%zu\n", book->getCurrentPageIndex());
        f.printf("total_pages=%zu\n", book->getTotalPages());
        f.printf("page_completed=%s\n", book->isPageCompleted() ? "true" : "false");
        f.printf("showlabel=%s\n", book->getShowLabel() ? "true" : "false");
        // 保存 keepOrg 的当前值
        f.printf("keepOrg=%s\n", book->getKeepOrg() ? "true" : "false");
        // 保存 drawBottom 的当前值
        f.printf("drawBottom=%s\n", book->getDrawBottom() ? "true" : "false");
        // 保存 verticalText 的当前值
        f.printf("verticalText=%s\n", book->getVerticalText() ? "true" : "false");
        
        // 保存最后阅读时间（小时/分钟），默认0
        f.printf("readhour=%d\n", book->getReadHour());
        f.printf("readmin=%d\n", book->getReadMin());

        f.println("valid=true");
        return true; });

    // 同步更新 .rec 文件：记录阅读时长历史
    if (ok)
    {
        std::string rec_fn = getRecordFileName(book->filePath());

        int16_t new_hour = book->getReadHour();
        int16_t new_min = book->getReadMin();

        // 计算增量（分钟数）
        int32_t old_total_mins = old_hour * 60 + old_min;
        int32_t new_total_mins = new_hour * 60 + new_min;
        int32_t delta_mins = new_total_mins - old_total_mins;

        // 仅当有增量时才更新 rec 文件
        if (delta_mins > 0)
        {
            // 获取当前时间戳（YYYYMMDDHH 格式）
            struct tm timeinfo;
            time_t now = time(nullptr);
            localtime_r(&now, &timeinfo);
            char timestamp_hour[32];
            char timestamp_day[32];
            snprintf(timestamp_hour, sizeof(timestamp_hour), "%04d%02d%02d%02d",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour);
            snprintf(timestamp_day, sizeof(timestamp_day), "%04d%02d%02d",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

            // 读取现有的 rec 文件
            std::map<std::string, int32_t> records; // key: timestamp, value: mins
            int32_t old_rec_total_mins = 0;         // rec文件第一行的原有总时间

            SafeFS::restoreFromTmpIfNeeded(rec_fn);
            if (SDW::SD.exists(rec_fn.c_str()))
            {
                File rf = SDW::SD.open(rec_fn.c_str(), "r");
                if (rf)
                {
                    // 读取第一行（总时间）
                    if (rf.available())
                    {
                        String first_line = rf.readStringUntil('\n');
                        first_line.trim();
                        // 解析 xxhxxm 或 xxm
                        int h_pos = first_line.indexOf('h');
                        if (h_pos > 0)
                        {
                            int hours = first_line.substring(0, h_pos).toInt();
                            int m_pos = first_line.indexOf('m', h_pos);
                            int minutes = 0;
                            if (m_pos > h_pos + 1)
                                minutes = first_line.substring(h_pos + 1, m_pos).toInt();
                            old_rec_total_mins = hours * 60 + minutes;
                        }
                        else
                        {
                            int m_pos = first_line.indexOf('m');
                            if (m_pos > 0)
                                old_rec_total_mins = first_line.substring(0, m_pos).toInt();
                        }
                    }

                    // 读取后续记录
                    while (rf.available())
                    {
                        String line = rf.readStringUntil('\n');
                        line.trim();
                        if (line.length() == 0)
                            continue;
                        int colon = line.indexOf(':');
                        if (colon > 0)
                        {
                            String ts = line.substring(0, colon);
                            String val = line.substring(colon + 1);
                            // 解析 xxm 或 xxhxxm
                            int32_t mins = 0;
                            int h_pos = val.indexOf('h');
                            if (h_pos > 0)
                            {
                                int hours = val.substring(0, h_pos).toInt();
                                int m_pos = val.indexOf('m', h_pos);
                                int minutes = 0;
                                if (m_pos > h_pos + 1)
                                    minutes = val.substring(h_pos + 1, m_pos).toInt();
                                mins = hours * 60 + minutes;
                            }
                            else
                            {
                                int m_pos = val.indexOf('m');
                                if (m_pos > 0)
                                    mins = val.substring(0, m_pos).toInt();
                            }
                            records[ts.c_str()] = mins;
                        }
                    }
                    rf.close();
                }
            }

            // 更新当前小时的记录
            records[timestamp_hour] += delta_mins;

            // 计算新的总时间：原有总时间 + 本次增量
            int32_t new_rec_total_mins = old_rec_total_mins + delta_mins;
            int32_t new_rec_total_hours = new_rec_total_mins / 60;
            int32_t new_rec_total_mins_remainder = new_rec_total_mins % 60;

            // 写回 rec 文件（第一行使用：原有总时间 + 本次增量）
            SafeFS::safeWrite(rec_fn, [&](File &f)
                              {
                // 第一行：原有总时间 + 本次增量
                f.printf("%dh%dm\n", new_rec_total_hours, new_rec_total_mins_remainder);
                
                // 后续行：按时间戳排序输出
                for (const auto &entry : records)
                {
                    int32_t total_mins = entry.second;
                    int32_t hours = total_mins / 60;
                    int32_t mins = total_mins % 60;
                    
                    if (hours > 0)
                        f.printf("%s:%dh%dm\n", entry.first.c_str(), hours, mins);
                    else
                        f.printf("%s:%dm\n", entry.first.c_str(), mins);
                }
                return true; });

            // 同步更新BookHandle对象和bm文件中的总时间（与rec文件第一行保持一致）
            book->setReadTime(new_rec_total_hours, new_rec_total_mins_remainder);

            // 重新保存bm文件以同步总时间
            std::string bm_fn = getBookmarkFileName(book->filePath());
            SafeFS::safeWrite(bm_fn, [&](File &f)
                              {
                f.printf("file_path=%s\n", book->filePath().c_str());
                f.printf("current_position=%zu\n", book->position());
                f.printf("file_size=%zu\n", book->getFileSize());
                f.printf("area_width=%d\n", book->getAreaWidth());
                f.printf("area_height=%d\n", book->getAreaHeight());
                f.printf("font_size=%.2f\n", book->getFontSize());
                f.printf("font_name=%s\n", get_current_font_name());
                f.printf("font_version=%u\n", get_font_version());
                f.printf("font_base_size=%u\n", get_font_size_from_file());
                f.printf("encoding=%d\n", (int)book->getEncoding());
                f.printf("current_page_index=%zu\n", book->getCurrentPageIndex());
                f.printf("total_pages=%zu\n", book->getTotalPages());
                f.printf("page_completed=%s\n", book->isPageCompleted() ? "true" : "false");
                f.printf("showlabel=%s\n", book->getShowLabel() ? "true" : "false");
                f.printf("keepOrg=%s\n", book->getKeepOrg() ? "true" : "false");
                f.printf("drawBottom=%s\n", book->getDrawBottom() ? "true" : "false");
                f.printf("verticalText=%s\n", book->getVerticalText() ? "true" : "false");
                
                // 写入与rec文件第一行同步的总时间
                f.printf("readhour=%d\n", new_rec_total_hours);
                f.printf("readmin=%d\n", new_rec_total_mins_remainder);
                
                f.println("valid=true");
                return true; });
        }
    }

    return ok;
}

BookmarkConfig loadBookmarkForFile(const std::string &book_file_path)
{
    BookmarkConfig cfg;
    std::string fn = getBookmarkFileName(book_file_path);
    // Attempt to restore from tmp if main file missing
    SafeFS::restoreFromTmpIfNeeded(fn);
    if (!SDW::SD.exists(fn.c_str()))
        return cfg;
    File f = SDW::SD.open(fn.c_str(), "r");
    if (!f)
        return cfg;
    while (f.available())
    {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.startsWith("#") || line.length() == 0)
            continue;
        int eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);
        key.trim();
        val.trim();
        if (key == "file_path")
            cfg.file_path = val.c_str();
        else if (key == "current_position")
            cfg.current_position = (size_t)val.toInt();
        else if (key == "file_size")
            cfg.file_size = (size_t)val.toInt();
        else if (key == "area_width")
            cfg.area_width = (std::int16_t)val.toInt();
        else if (key == "area_height")
            cfg.area_height = (std::int16_t)val.toInt();
        else if (key == "font_size")
            cfg.font_size = val.toFloat();
        else if (key == "font_name")
            cfg.font_name = val.c_str();
        else if (key == "font_version")
            cfg.font_version = (uint8_t)val.toInt();
        else if (key == "font_base_size")
            cfg.font_base_size = (uint8_t)val.toInt();
        else if (key == "encoding")
            cfg.encoding = (TextEncoding)val.toInt();
        else if (key == "current_page_index")
            cfg.current_page_index = (size_t)val.toInt();
        else if (key == "total_pages")
            cfg.total_pages = (size_t)val.toInt();
        else if (key == "page_completed")
            cfg.page_completed = (val == "true");
        else if (key == "showlabel")
            cfg.showlabel = (val == "true");
        else if (key == "keepOrg")
            cfg.keepOrg = (val == "true");
        else if (key == "drawBottom")
            cfg.drawBottom = (val == "true");
        else if (key == "verticalText")
            cfg.verticalText = (val == "true");
        else if (key == "readhour")
            cfg.readhour = (std::int16_t)val.toInt();
        else if (key == "readmin")
            cfg.readmin = (std::int16_t)val.toInt();
        else if (key == "valid")
            cfg.valid = (val == "true");
    }
    f.close();

#if DBG_BOOKMARK
    Serial.printf("[BOOKMARK] === 加载书签文件信息 ===\n");
    Serial.printf("[BOOKMARK] 书签文件: %s\n", fn.c_str());
    Serial.printf("[BOOKMARK] 文件路径: %s\n", cfg.file_path.c_str());
    Serial.printf("[BOOKMARK] 当前位置: %zu\n", cfg.current_position);
    Serial.printf("[BOOKMARK] 文件大小: %zu bytes\n", cfg.file_size);
    Serial.printf("[BOOKMARK] 显示区域: %dx%d\n", cfg.area_width, cfg.area_height);
    Serial.printf("[BOOKMARK] 字体大小: %.2f\n", cfg.font_size);
    Serial.printf("[BOOKMARK] 字体名称: %s\n", cfg.font_name.empty() ? "未知" : cfg.font_name.c_str());
    Serial.printf("[BOOKMARK] 字体版本: %u\n", cfg.font_version);
    Serial.printf("[BOOKMARK] 字体基础尺寸: %u\n", cfg.font_base_size);
    Serial.printf("[BOOKMARK] 编码方式: %d\n", (int)cfg.encoding);
    Serial.printf("[BOOKMARK] 当前页索引: %zu\n", cfg.current_page_index);
    Serial.printf("[BOOKMARK] 总页数: %zu\n", cfg.total_pages);
    Serial.printf("[BOOKMARK] 页面完成: %s\n", cfg.page_completed ? "是" : "否");
    Serial.printf("[BOOKMARK] 书签有效: %s\n", cfg.valid ? "是" : "否");
    Serial.printf("[BOOKMARK] === 书签信息结束 ===\n");
#endif

    return cfg;
}

bool BookHandle::loadBookmarkAndJump()
{
    BookmarkConfig cfg = loadBookmarkForFile(file_path);

    // If we just fell back to default during open, skip restoring bookmark to avoid jumping
    // to some unrelated position. Instead start from the beginning and save a fresh bookmark.
    if (skip_bookmark_on_open)
    {
#if DBG_BOOK_HANDLE
        Serial.println("[BH] loadBookmarkAndJump: skip_bookmark_on_open set, start from page 0");
#endif
        skip_bookmark_on_open = false;
        current_page_index = 0;
        setPosition(0);
        page_completed = false;
        currentPage();
        saveBookmark();
        return false;
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] loadBookmarkAndJump: === 书签加载验证 ===\n");
    Serial.printf("[BH] 当前文件: %s\n", file_path.c_str());
    Serial.printf("[BH] 当前文件大小: %zu bytes\n", getFileSize());
    Serial.printf("[BH] 当前字体大小: %.2f\n", font_size);
    Serial.printf("[BH] 当前字体名称: %s\n", get_current_font_name());
    Serial.printf("[BH] 当前字体版本: %u\n", get_font_version());
    Serial.printf("[BH] 当前字体基础尺寸: %u\n", get_font_size_from_file());
    Serial.printf("[BH] 当前显示区域: %dx%d\n", area_w, area_h);
    Serial.printf("[BH] 当前编码: %d\n", (int)encoding);
    if (cfg.valid)
    {
        Serial.printf("[BH] 书签文件大小: %zu bytes\n", cfg.file_size);
        Serial.printf("[BH] 书签字体大小: %.2f\n", cfg.font_size);
        Serial.printf("[BH] 书签显示区域: %dx%d\n", cfg.area_width, cfg.area_height);
        Serial.printf("[BH] 书签编码: %d\n", (int)cfg.encoding);
    }
    Serial.printf("[BH] === 验证信息结束 ===\n");
#endif

    if (!cfg.valid)
    {
        // 没有有效书签，从第一页开始
        current_page_index = 0;
        setPosition(0);
        page_completed = false;
        // clear cached page to force re-render
        last_page.success = false;
        g_text_state.last_page.clear();

#if DBG_BOOK_HANDLE
        Serial.printf("[BH] loadBookmarkAndJump: 无书签，从第一页开始 (index=0)\n");
#endif

        currentPage();
        saveBookmark();
        return false;
    }

    // 验证书签文件路径是否匹配当前文件
    if (!cfg.file_path.empty() && cfg.file_path != file_path)
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] loadBookmarkAndJump: 文件路径不匹配，书签='%s'，当前='%s'，重新开始\n",
                      cfg.file_path.c_str(), file_path.c_str());
#endif
        // 文件路径不匹配，从第一页开始
        current_page_index = 0;
        setPosition(0);
        page_completed = false;
        currentPage();
        saveBookmark();
        return false;
    }

    // 验证文件大小是否匹配（检测文件是否被修改过）
    size_t current_file_size = getFileSize();
    bool file_size_changed = (cfg.file_size > 0 && current_file_size > 0 && cfg.file_size != current_file_size);

    // 验证字体大小是否匹配（使用当前加载的字体文件大小，而非BookHandle构造时的font_size）
    uint8_t current_font_file_size = get_font_size_from_file();
    bool font_size_changed = (cfg.font_base_size > 0 && current_font_file_size > 0 && cfg.font_base_size != current_font_file_size);

    // Note: font_name and font_version comparison removed - only font size matters for re-indexing
    // Same-size fonts assumed to have similar pagination impact, user can manually trigger re-index if needed

    if (file_size_changed || font_size_changed)
    {
#if DBG_BOOK_HANDLE
        if (file_size_changed)
        {
            Serial.printf("[BH] loadBookmarkAndJump: 文件大小不匹配 (书签:%zu, 当前:%zu)，强制重建索引\n",
                          cfg.file_size, current_file_size);
        }
        if (font_size_changed)
        {
            Serial.printf("[BH] loadBookmarkAndJump: 字体大小不匹配 (书签:%u, 当前:%u)，强制重建索引\n",
                          cfg.font_base_size, current_font_file_size);
        }
#endif
        // 文件或字体被修改过，请求后台任务强制重建索引（避免阻塞）
        extern void requestForceReindex();
        requestForceReindex();

        // 更新 BookHandle 的 font_size 以反映当前加载的字体
        extern float font_size;
        this->font_size = font_size;

        // 从第一页重新开始 - 无论索引是否完成都要正确设置位置
        current_page_index = 0;

        // 确保使用正确的第一页位置
        if (!page_positions.empty())
        {
            cur_pos = page_positions[0];
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] loadBookmarkAndJump: 已请求后台重建索引，设置第一页位置 %zu\n", cur_pos);
#endif
        }
        else
        {
            cur_pos = 0;
#if DBG_BOOK_HANDLE
            Serial.println("[BH] loadBookmarkAndJump: 已请求后台重建索引，页面索引尚未建立，使用位置 0");
#endif
        }

        page_completed = false;
        // 清除缓存，强制重新读取
        last_page.success = false;
        currentPage();
        saveBookmark();
        return false;
    }

    // 恢复编码设置和showlabel设置
    this->encoding = cfg.encoding;
    this->showlabel = cfg.showlabel;
    // 恢复 keepOrg 设置（是否跳过繁简转换）
    this->keep_org_ = cfg.keepOrg;
    // 恢复 drawBottom 设置（是否在文字下方画底线）
    this->draw_bottom_ = cfg.drawBottom;
    // 恢复 verticalText 设置（是否竖排显示文字）
    this->vertical_text_ = cfg.verticalText;
    // 恢复阅读时长（小时/分钟）到 BookHandle 成员，以便继续累积
    this->readhour = cfg.readhour;
    this->readmin = cfg.readmin;

    // 确保分页信息已加载
    if (!pages_loaded)
    {
        if (!loadPageFile())
        {
            if (!generatePageFile())
            {
                // 无法生成分页信息，使用位置方式回退
                setPosition(cfg.current_position);
                current_page_index = 0;
                page_completed = cfg.page_completed;

#if DBG_BOOK_HANDLE
                Serial.printf("[BH] loadBookmarkAndJump: 无法生成分页，回退到位置模式 (pos=%zu, index=0)\n", cfg.current_position);
#endif

                currentPage();
                // clear cached last page when we've loaded page positions
                last_page.success = false;
                g_text_state.last_page.clear();
                return true;
            }
        }
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] loadBookmarkAndJump: 书签信息 - page_index=%zu, position=%zu, total_pages=%zu\n",
                  cfg.current_page_index, cfg.current_position, page_positions.size());
#endif

    // 【重要】检测书签索引异常：如果页面索引远超实际页数，说明可能是之前重复索引导致的
    // 这种情况下应该基于位置重新查找正确的页面索引，并保存修正后的书签
    bool bookmark_index_corrupted = false;
    if (cfg.current_page_index >= page_positions.size())
    {
        bookmark_index_corrupted = true;
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] loadBookmarkAndJump: ⚠️ 书签页面索引异常 (%zu >= %zu)，可能是重复索引导致，将基于位置重新定位\n",
                      cfg.current_page_index, page_positions.size());
#endif
    }

    // 验证书签中的页面索引是否有效
    if (!bookmark_index_corrupted && cfg.current_page_index < page_positions.size())
    {
        // 使用页面索引恢复位置
        current_page_index = cfg.current_page_index;
        cur_pos = page_positions[current_page_index];
        page_completed = cfg.page_completed;

#if DBG_BOOK_HANDLE
        Serial.printf("[BH] loadBookmarkAndJump: 使用页面索引 %zu, 位置 %zu\n", current_page_index, cur_pos);
#endif

        // 如果书签中的位置与页面起始位置不一致，使用书签位置
        if (cfg.current_position != cur_pos &&
            cfg.current_position >= cur_pos &&
            (current_page_index + 1 >= page_positions.size() ||
             cfg.current_position < page_positions[current_page_index + 1]))
        {
            cur_pos = cfg.current_position;
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] loadBookmarkAndJump: 使用精确书签位置 %zu\n", cur_pos);
#endif
        }

        // (removeIndexFilesForBookForPath is implemented once near the top of this file)
    }
    else
    {
        // 书签中的页面索引无效（或被检测为损坏），尝试根据位置查找最接近的页面
        current_page_index = 0;
        for (size_t i = 0; i < page_positions.size(); i++)
        {
            if (page_positions[i] <= cfg.current_position)
            {
                current_page_index = i;
            }
            else
            {
                break;
            }
        }
        cur_pos = cfg.current_position;
        page_completed = cfg.page_completed;

#if DBG_BOOK_HANDLE
        if (bookmark_index_corrupted)
        {
            Serial.printf("[BH] loadBookmarkAndJump: 📝 书签已修复 - 从索引 %zu 修正为 %zu (位置 %zu)\n",
                          cfg.current_page_index, current_page_index, cur_pos);
        }
        else
        {
            Serial.printf("[BH] loadBookmarkAndJump: 索引无效，根据位置查找到页面 %zu, 位置 %zu\n",
                          current_page_index, cur_pos);
        }
#endif

        // 【关键】如果检测到书签损坏，保存修正后的书签，避免每次都重新计算
        if (bookmark_index_corrupted)
        {
            // 延迟保存，避免阻塞构造函数
            // saveBookmark() 会在 currentPage() 之后调用
        }
    }

    currentPage();

    // 如果书签被修复，立即保存正确的书签
    if (bookmark_index_corrupted)
    {
        saveBookmark();
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] loadBookmarkAndJump: ✅ 已保存修正后的书签 (index=%zu, pos=%zu)\n",
                      current_page_index, cur_pos);
#endif
    }

    return true;
}

bool BookHandle::saveBookmark()
{
    return saveBookmarkForFile(this);
}

// 增加阅读时间 1 分钟：在 readhour/readmin 基础上加 1 分钟，处理进位与上限
void BookHandle::incrementReadingMinute()
{
    // 如果已经达到上限 9999:59，则不再增加
    if (readhour >= 9999 && readmin >= 59)
        return;

    // 增加一分钟
    ++readmin;
    if (readmin >= 60)
    {
        readmin -= 60;
        if (readhour < 9999)
            ++readhour;
        else
        {
            // 达到小时上限，确保分钟为59并停止增长
            readmin = 59;
            readhour = 9999;
        }
    }
}

bool BookHandle::loadPage()
{
    if (pages_loaded)
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] loadPage: 分页已加载，共 %zu 页\n", page_positions.size());
#endif
        tryInitializeFontCache();
        return true; // 已经加载过了
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] loadPage: 开始加载分页信息...\n");
#endif

    // 尝试从文件加载分页信息
    if (loadPageFile())
    {
        tryInitializeFontCache();

        // 检查是否有进度文件来判断索引是否完成
        std::string progress_file = getProgressFileName();
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] loadPage: 发现进度文件 %s\n", progress_file.c_str());
#endif
        if (SDW::SD.exists(progress_file.c_str()))
        {
            // 有进度文件，说明索引未完成

            if (!loadIndexProgress())
            {
                // 如果加载进度失败（例如进度文件损坏或参数校验未通过），
                // 尝试从已加载的 page_positions 中推断已写入的最后偏移，以便恢复索引位置。
                if (!page_positions.empty())
                {
                    indexing_current_pos = page_positions.back();
                    indexing_file_size = file_handle.size();
                    indexing_in_progress = false;
#if DBG_BOOK_HANDLE
                    Serial.printf("[BH] loadPage: loadIndexProgress 失败，已从 page 文件推断当前位置: %zu\n", indexing_current_pos);
#endif
                }
            }
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] 检测到进度文件，索引未完成，当前进度: %.1f%%\n", getIndexingProgress());
#endif
        }
        else
        {
            // 没有进度文件，说明索引已完成

#if DBG_BOOK_HANDLE
            Serial.printf("[BH] 无进度文件，索引已完成\n");
#endif
        }

#if DBG_BOOK_HANDLE
        Serial.printf("[BH] 从文件加载分页信息成功，共 %zu 页，索引完成: %s\n",
                      page_positions.size(), isIndexingComplete() ? "YES" : "NO");
        if (page_positions.size() > 0)
        {
            Serial.printf("[BH] 首页位置: %zu, 末页位置: %zu\n",
                          page_positions[0], page_positions[page_positions.size() - 1]);
        }
#endif
        return true;
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] 分页文件不存在，检查是否有进度文件...\n");
#endif

    // 检查是否有未完成的索引建立进程
    if (loadIndexProgress())
    {
        // 有进度文件，尝试加载部分分页信息
        if (loadPageFile())
        {
            tryInitializeFontCache(); // 部分加载成功，可以开始阅读

#if DBG_BOOK_HANDLE
            Serial.printf("[BH] 从部分分页文件加载成功，当前 %zu 页，进度: %.1f%%\n",
                          page_positions.size(), getIndexingProgress());
#endif
            return true;
        }
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] 开始增量生成分页文件...\n");
#endif

    // 开始增量生成分页文件
    if (generatePageFileIncremental())
    {
        tryInitializeFontCache();
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] 增量生成分页文件完成，共 %zu 页\n", page_positions.size());
#endif
        return true;
    }
    else if (page_positions.size() > 0)
    {
        // 部分生成成功，可以开始阅读
        tryInitializeFontCache();

#if DBG_BOOK_HANDLE
        Serial.printf("[BH] 增量生成分页文件部分完成，当前 %zu 页，进度: %.1f%%\n",
                      page_positions.size(), getIndexingProgress());
        Serial.printf("[BH] 用户可以开始阅读，索引将在后台继续\n");
#endif
        return true;
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] 分页加载/生成失败！\n");
#endif
    return false;
}

std::string BookHandle::getPageFileName() const
{
    return page_filename_for(file_path);
}

// 获取总页数 - 智能检测索引完成后是否需要重新加载
size_t BookHandle::getTotalPages() const
{
    // 【废弃自动重新加载机制】
    // 原本想在索引完成后自动重新加载，但会导致时序问题：
    // 1. 设置 index_just_completed = true
    // 2. 立刻被其他线程/调用者检测到
    // 3. 重新加载时磁盘文件可能还没完全刷新
    // 4. 结果页数从4页变成1页
    //
    // 新方案：不自动重新加载，依赖内存中的page_positions
    // 磁盘文件主要用于下次打开书籍时恢复状态
    return page_positions.size();
}

// 检查是否有下一页 - 使用getTotalPages()以触发必要的重新加载
bool BookHandle::hasNextPage() const
{
    return current_page_index + 1 < getTotalPages();
}

bool BookHandle::loadPageFile()
{
    std::string page_file = getPageFileName();

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] loadPageFile: 尝试加载 %s\n", page_file.c_str());
#endif
    // 如果已经加载过分页，直接复用（避免重复从 SD 读取）
    if (pages_loaded && !page_positions.empty())
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] loadPageFile: 已缓存，跳过重复加载\n");
#endif
        return true;
    }

    if (!SDW::SD.exists(page_file.c_str()))
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] loadPageFile: 文件不存在\n");
#endif
        return false;
    }

    File file = SDW::SD.open(page_file.c_str(), "r");
    if (!file)
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] loadPageFile: 无法打开文件\n");
#endif
        return false;
    }

    page_positions.clear();

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] loadPageFile: 尝试以二进制方式解析分页文件...\n");
#endif

    // 检查是否为我们自定义的二进制格式：magic 'BPG1' (4 bytes), version (1 byte), reserved(3), uint32_t count
    char magic[4] = {0};
    if (file.readBytes(magic, 4) == 4)
    {
        if (magic[0] == 'B' && magic[1] == 'P' && magic[2] == 'G' && magic[3] == '1')
        {
            // binary format
            uint8_t ver = 0;
            if (file.readBytes((char *)&ver, 1) != 1)
            {
#if DBG_BOOK_HANDLE
                Serial.println("[BH] loadPageFile: 无法读取版本字节，退回文本解析");
#endif
                file.close();
                return false;
            }
            // skip 3 reserved bytes
            file.seek(file.position() + 3);

            uint32_t count = 0;
            if (file.readBytes((char *)&count, sizeof(count)) != sizeof(count))
            {
#if DBG_BOOK_HANDLE
                Serial.println("[BH] loadPageFile: 无法读取count，退回文本解析");
#endif
                file.close();
                return false;
            }

            // 计算文件中实际的偏移量数量（从文件大小推断）
            size_t cur_pos = file.position();
            size_t total_size = file.size();
            size_t remaining = (total_size > cur_pos) ? (total_size - cur_pos) : 0;
            uint32_t actualCount = (uint32_t)(remaining / sizeof(uint32_t));

            // 鲁棒性检查：如果 count 字段明显错误，使用推断值
            // 情况1: count 为 0（增量写入时的占位符，或 patchCount 被复位中断）
            // 情况2: count 明显过大（超过100万页，或超过文件实际容量）
            // 情况3: count 与文件大小不匹配（count 被部分写入）
            bool countIsStale = (count == 0) ||
                                (count > 1000000) ||
                                (count != actualCount);

            if (countIsStale)
            {
#if DBG_BOOK_HANDLE
                Serial.printf("[BH] loadPageFile: count字段异常 (filed=%u, actual=%u)，从文件大小推断\n",
                              count, actualCount);
#endif
                count = actualCount;

                // 检查是否有非对齐的字节（可能表示写入被中断）
                if (remaining % sizeof(uint32_t) != 0)
                {
#if DBG_BOOK_HANDLE
                    Serial.printf("[BH] loadPageFile: 警告: 偏移量字节数不对齐 (remaining=%zu)，可能丢失最后一个偏移量\n",
                                  remaining);
#endif
                }
            }

            // Read offsets (uint32_t) one by one
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t off = 0;
                if (file.readBytes((char *)&off, sizeof(off)) != sizeof(off))
                {
#if DBG_BOOK_HANDLE
                    Serial.printf("[BH] loadPageFile: 提前结束，已读取 %u/%u 项\n", i, count);
#endif
                    break;
                }
                page_positions.push_back((size_t)off);
            }
            file.close();
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] loadPageFile: 二进制解析完成，共 %zu 页\n", page_positions.size());
#endif
            if (page_positions.empty())
            {
                page_positions.push_back(0);
                tryInitializeFontCache();
                return false;
            }
            pages_loaded = true;
            tryInitializeFontCache();
            return true;
        }
    }

    // 退回到文本行解析（兼容老格式）
#if DBG_BOOK_HANDLE
    Serial.printf("[BH] loadPageFile: 非二进制或解析失败，使用文本解析回退\n");
#endif
    // rewind to start for text parsing
    file.seek(0);

    // 使用固定缓冲区按块读取，减少对 SD 的小读次数
    const size_t BUF_SIZE = 1024;
    char buffer[BUF_SIZE];
    std::string leftover;
    int line_count = 0;

    auto trim_inplace = [](std::string &s)
    {
        while (!s.empty() && isspace((unsigned char)s.front()))
            s.erase(s.begin());
        while (!s.empty() && isspace((unsigned char)s.back()))
            s.pop_back();
    };

    while (file.available())
    {
        size_t n = file.readBytes(buffer, BUF_SIZE);
        if (n == 0)
            break;

        size_t start = 0;
        for (size_t i = 0; i < n; ++i)
        {
            if (buffer[i] == '\n')
            {
                std::string line;
                if (!leftover.empty())
                {
                    line.swap(leftover);
                }
                line.append(buffer + start, i - start);
                start = i + 1;

                trim_inplace(line);
                if (!line.empty())
                {
                    unsigned long long v = strtoull(line.c_str(), nullptr, 10);
                    size_t pos = (size_t)v;
                    page_positions.push_back(pos);
                    ++line_count;
#if DBG_BOOK_HANDLE
                    if (line_count <= 3 || line_count % 1000 == 0)
                    {
                        Serial.printf("[BH] loadPageFile: 第%d行，位置 %zu\n", line_count, pos);
                    }
#endif
                }
            }
        }

        if (start < n)
        {
            leftover.append(buffer + start, n - start);
        }
    }

    if (!leftover.empty())
    {
        trim_inplace(leftover);
        if (!leftover.empty())
        {
            unsigned long long v = strtoull(leftover.c_str(), nullptr, 10);
            size_t pos = (size_t)v;
            page_positions.push_back(pos);
            ++line_count;
#if DBG_BOOK_HANDLE
            if (line_count <= 3 || line_count % 1000 == 0)
            {
                Serial.printf("[BH] loadPageFile: 第%d行，位置 %zu\n", line_count, pos);
            }
#endif
        }
    }

    file.close();

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] loadPageFile: 文本解析完成，共 %d 行，%zu 页\n", line_count, page_positions.size());
#endif

    if (page_positions.empty())
    {
        page_positions.push_back(0);
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] loadPageFile: 空文件\n");
#endif
        tryInitializeFontCache(); // 仍标记为已加载，避免重复尝试
        return false;
    }

    tryInitializeFontCache();
    return true;
}

bool BookHandle::savePageFile()
{
    // Writing page file is handled by the background indexer to avoid
    // concurrent filesystem writes from multiple contexts.
    return true;
}

// 获取进度文件名
std::string BookHandle::getProgressFileName() const
{
    return progress_filename_for(file_path);
}

// 加载索引建立进度
bool BookHandle::loadIndexProgress()
{
    std::string progress_file = getProgressFileName();

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] loadIndexProgress: 尝试加载进度文件:%s\n", progress_file.c_str());
#endif

    // Try to restore from tmp if needed
    SafeFS::restoreFromTmpIfNeeded(progress_file);
    // If no progress file found, check for .complete marker
    if (!SDW::SD.exists(progress_file.c_str()))
    {
        std::string complete_marker = complete_filename_for(file_path);

        if (SDW::SD.exists(complete_marker.c_str()))
        {
            // Validate that the .page file exists and looks sane before accepting .complete.
            std::string page_file = getPageFileName();
            bool page_valid = false;
            if (SDW::SD.exists(page_file.c_str()))
            {
                File pf = SDW::SD.open(page_file.c_str(), "r");
                if (pf)
                {
                    uint8_t magic[4] = {0};
                    if (pf.read(magic, 4) == 4)
                    {
                        if (magic[0] == 'B' && magic[1] == 'P' && magic[2] == 'G' && magic[3] == '1')
                            page_valid = true;
                    }
                    pf.close();
                }
            }

            if (page_valid)
            {
                // Indexing previously completed and page file appears valid

                tryInitializeFontCache();
#if DBG_BOOK_HANDLE
                Serial.printf("[BH] loadIndexProgress: 找到 .complete 标记 (%s)，且 .page 文件验证通过，标记索引已完成\n", complete_marker.c_str());
#endif
                return true;
            }
            else
            {
#if DBG_BOOK_HANDLE
                Serial.printf("[BH] loadIndexProgress: 找到 .complete 标记 (%s) 但 .page 文件缺失或无效 (%s)。忽略 .complete，需重新索引\n", complete_marker.c_str(), page_file.c_str());
#endif
                // Treat as not complete to avoid false positive from stale/corrupt marker
                return false;
            }
        }

#if DBG_BOOK_HANDLE
        Serial.printf("[BH] loadIndexProgress: 未找到进度文件或完成标记\n");
#endif
        return false;
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] loadIndexProgress: 尝试加载 %s\n", progress_file.c_str());
#endif

    File file = SDW::SD.open(progress_file.c_str(), "r");
    if (!file)
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] loadIndexProgress: 无法打开进度文件: %s\n", progress_file.c_str());
#endif
        return false;
    }

    IndexProgress progress;

    // 读取进度信息
    while (file.available())
    {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0)
            continue;

        int eq_pos = line.indexOf('=');
        if (eq_pos == -1)
            continue;

        String key = line.substring(0, eq_pos);
        String value = line.substring(eq_pos + 1);

        if (key == "file_path")
        {
            progress.file_path = value.c_str();
        }
        else if (key == "file_size")
        {
            progress.file_size = (size_t)value.toInt();
        }
        else if (key == "current_pos")
        {
            progress.current_pos = (size_t)value.toInt();
        }
        else if (key == "pages_generated")
        {
            progress.pages_generated = (size_t)value.toInt();
        }
        else if (key == "area_width")
        {
            progress.area_width = (int16_t)value.toInt();
        }
        else if (key == "area_height")
        {
            progress.area_height = (int16_t)value.toInt();
        }
        else if (key == "font_size")
        {
            progress.font_size = value.toFloat();
        }
        else if (key == "encoding")
        {
            progress.encoding = (TextEncoding)value.toInt();
        }
        else if (key == "start_time")
        {
            progress.start_time = (unsigned long)value.toInt();
        }
        else if (key == "last_update")
        {
            progress.last_update = (unsigned long)value.toInt();
        }
        else if (key == "valid")
        {
            progress.valid = (value == "true");
        }
    }

    file.close();

    // 验证进度信息是否有效
    size_t current_file_size = file_handle.size();
    bool validation_ok = true;
    if (!progress.valid ||
        progress.file_path != file_path ||
        progress.file_size != current_file_size || // 添加文件大小检查
        progress.area_width != area_w ||
        progress.area_height != area_h ||
        abs(progress.font_size - font_size) > 0.01f ||
        progress.encoding != encoding)
    {
        validation_ok = false;
#if DBG_BOOK_HANDLE
        if (progress.file_size != current_file_size)
        {
            Serial.printf("[BH] loadIndexProgress: 文件大小不匹配 (进度:%zu, 当前:%zu)\n",
                          progress.file_size, current_file_size);
        }
        if (abs(progress.font_size - font_size) > 0.01f)
        {
            Serial.printf("[BH] loadIndexProgress: 字体大小不匹配 (进度:%.2f, 当前:%.2f)\n",
                          progress.font_size, font_size);
        }
        Serial.printf("[BH] loadIndexProgress: 进度信息无效或参数不匹配\n");
#endif
    }

    // 如果校验失败但进度文件本身标注为 valid，则宽松回退，使用进度文件的关键字段来恢复索引
    if (!validation_ok)
    {
        if (progress.valid)
        {
            // 接受进度文件中的位置与文件大小（若 progress.file_size 为0则使用当前文件大小），以便恢复索引
            indexing_current_pos = progress.current_pos;
            indexing_file_size = (progress.file_size != 0) ? progress.file_size : file_handle.size();
            indexing_start_time = progress.start_time;

            indexing_in_progress = false;
            pages_loaded = true;
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] loadIndexProgress: 使用宽松回退恢复进度 - 位置: %zu, 文件大小: %zu\n",
                          indexing_current_pos, indexing_file_size);
#endif
            return true;
        }
        // 否则严格失败
        return false;
    }

    // 恢复状态
    indexing_current_pos = progress.current_pos;
    indexing_file_size = progress.file_size;
    indexing_start_time = progress.start_time;
    // 如果存在 .complete 标记文件，则认为索引已完成
    std::string complete_marker = complete_filename_for(file_path);
    // legacy basename.complete fallback
    std::string legacy_complete;
    {
        size_t last_slash = file_path.find_last_of('/');
        std::string base_name = (last_slash != std::string::npos) ? file_path.substr(last_slash + 1) : file_path;
        size_t last_dot = base_name.find_last_of('.');
        if (last_dot != std::string::npos)
            base_name = base_name.substr(0, last_dot);
        legacy_complete = std::string("/bookmarks/") + base_name + ".complete";
    }

    if (SDW::SD.exists(complete_marker.c_str()) || SDW::SD.exists(legacy_complete.c_str()))
    {

        pages_loaded = true;
    }
    else
    {

        indexing_in_progress = false; // 当前未在进行中
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] loadIndexProgress: 成功加载进度 - 位置: %zu/%zu, 页数: %zu, 索引完成: %s\n",
                  progress.current_pos, progress.file_size, progress.pages_generated,
                  isIndexingComplete() ? "YES" : "NO");
#endif

    return true;
}

// 保存索引建立进度
bool BookHandle::saveIndexProgress()
{
    if (!ensureBookmarksFolder())
    {
        return false;
    }

    std::string progress_file = getProgressFileName();
    bool ok = SafeFS::safeWrite(progress_file, [&](File &file)
                                {
        // 写入进度信息
        file.printf("file_path=%s\n", file_path.c_str());
        file.printf("file_size=%zu\n", indexing_file_size);
        file.printf("current_pos=%zu\n", indexing_current_pos);
        file.printf("pages_generated=%zu\n", page_positions.size());
        file.printf("area_width=%d\n", area_w);
        file.printf("area_height=%d\n", area_h);
        file.printf("font_size=%.2f\n", font_size);
        file.printf("encoding=%d\n", (int)encoding);
        file.printf("start_time=%lu\n", indexing_start_time);
        file.printf("last_update=%lu\n", millis());
        file.printf("valid=true\n");
        return true; });

#if DBG_BOOK_HANDLE
    if (ok)
    {
        Serial.printf("[BH] saveIndexProgress: 保存进度成功 - 位置: %zu/%zu\n",
                      indexing_current_pos, indexing_file_size);
    }
    else
    {
        Serial.printf("[BH] saveIndexProgress: 保存进度失败 - %s\n", progress_file.c_str());
    }
#endif
    return ok;
}

// 获取索引建立进度百分比
float BookHandle::getIndexingProgress() const
{
    if (indexing_file_size == 0)
        return 0.0f;
    return (float)indexing_current_pos * 100.0f / indexing_file_size;
}

// 增量生成分页文件
bool BookHandle::generatePageFileIncremental()
{
    // 若已经完成（基于综合判断），直接返回成功，避免错误地再次触发索引
    if (isIndexingComplete())
    {
        indexing_in_progress = false;
        return true;
    }

    // 【关键修复】如果索引正在进行中，直接返回true，不要停止它
    // 这避免了用户翻页时意外停止后台索引
    if (indexing_in_progress)
    {
#if DBG_BOOK_HANDLE
        Serial.println("[BH] generatePageFileIncremental: indexing already in progress, returning true");
#endif
        return true; // 索引正在进行，无需重复启动
    }

    return true;
}

// 继续索引建立
bool BookHandle::continueIndexGeneration()
{
    // 强化：使用稳健判定，避免索引已完成后仍被误触发
    if (isIndexingComplete())
    {
        indexing_in_progress = false;
        return true; // 已经完成
    }

    // 【简化逻辑】如果索引正在进行中，直接返回true，不做任何干扰
    if (indexing_in_progress)
    {
        return true; // 索引正在进行，无需重复启动
    }

    // 请求后台索引任务执行增量索引，避免在 UI/前台路径同步执行
    return true;
}

// 如果需要且可以，恢复索引建立
void BookHandle::resumeIndexingIfNeeded()
{
    if (canContinueIndexing())
    {
        continueIndexGeneration();
    }
}

// 强制重新建立索引
bool BookHandle::forceReindex(bool already_holding_lock)
{
    // Delegate to background task: reset in-memory state and request background force reindex.
    Serial.println("[BH] forceReindex: delegating to background requestForceReindex()");

    // Reset in-memory indexing state so UI uses a clean state while background rebuilds

    indexing_in_progress = false;
    indexing_should_stop = false;
    indexing_current_pos = 0;
    indexing_file_size = 0;

    page_positions.clear();
    pages_loaded = false;
    current_page_index = 0;
    page_completed = false;
    last_page.success = false;
    g_text_state.last_page.clear();

    // Ensure bookmark font recheck occurs after rebuild
    bookmark_font_checked = false;

    // 立即同步书签以反映“索引将重建”的状态：
    // - current_page_index 重置为 0（已在上方设置）
    // - total_pages 由 page_positions.size() 写入，此时为 0
    // - page_completed 根据当前状态写入（通常为 false）
    // 其他非索引字段保持不变，且不删除 bm 文件。
    (void)saveBookmarkForFile(this);

    extern void requestForceReindex();
    requestForceReindex();
    // Caller expects a bool — return true to indicate request accepted
    return true;
}

bool BookHandle::generatePageFile()
{
    // Indexing will be handled automatically by the main loop
    return true;
}

// 渲染当前页面内容到屏幕
void BookHandle::renderCurrentPage(float font_size_param, M5Canvas *canvas, bool showPage, bool showWait, bool pendingPush, int8_t renderType, display_type effect)
{
    extern M5Canvas *g_canvas;
    extern GlobalConfig g_config;
    extern FontBufferManager g_font_buffer_manager;
    extern bool g_using_progmem_font;
    bool dark = g_config.dark;
    // Timing: record start time for renderCurrentPage -> bin_font_flush_canvas interval
    uint32_t bh_render_start_ms = millis();
    // If effect is RANDOM, pick a random non-NOEFFECT/non-RANDOM effect
    if (effect == RANDOM)
    {
        // List of available effects excluding NOEFFECT and RANDOM
        display_type available_effects[] = {
            VSHUTTER,  
            HSHUTTER,  
            VSHUTTER_NORMAL, 
            HSHUTTER_NORMAL,  
            VSHUTTER_REV,  
            HSHUTTER_REV,  
            VSHUTTER_NORMAL_REV, 
            HSHUTTER_NORMAL_REV,  
            RECT,  
        };
        size_t count = sizeof(available_effects) / sizeof(available_effects[0]);
        if (count > 0)
        {
            randomSeed(millis());
            size_t idx = random((int)count);
            effect = available_effects[idx];
        }
        else
        {
            effect = NOEFFECT;
        }
    }

    // ===== 字体页面缓存检查与准备 =====
    // 1. 检查缓存管理器是否已初始化
    if (!g_using_progmem_font && pages_loaded && !g_font_buffer_manager.isInitialized())
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] renderCurrentPage: 初始化字体缓存 (page=%u)\n", (unsigned)getCurrentPageIndex());
#endif
        g_font_buffer_manager.initialize(this, getCurrentPageIndex());
    }

    // 2. 确保当前页缓存已准备好（如果缓存管理器已初始化但当前页缺失，构建之）
    if (!g_using_progmem_font && g_font_buffer_manager.isInitialized() && pages_loaded)
    {
        if (!g_font_buffer_manager.isCacheValid(0))
        {
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] renderCurrentPage: 当前页缓存缺失，构建中...\n");
#endif
            // 简单重建当前页窗口
            g_font_buffer_manager.initialize(this, getCurrentPageIndex());
        }
    }

    // 在首次渲染时，检查书签中记录的字体/显示参数是否与当前不匹配。
    // 如果不匹配，则重置到第一页并强制重建索引。
    if (!bookmark_font_checked)
    {
        BookmarkConfig cfg = loadBookmarkForFile(file_path);
        if (cfg.valid)
        {
            // 使用当前加载的字体大小（而非BookHandle构造时的font_size）来检测变化
            uint8_t current_font_file_size = get_font_size_from_file();
            bool font_size_changed = (cfg.font_base_size > 0 && current_font_file_size > 0 && cfg.font_base_size != current_font_file_size);
            // Note: font_name and font_version comparison removed - only font size matters for re-indexing
            // Same-size fonts assumed to have similar pagination impact
            bool area_changed = (cfg.area_width != area_w || cfg.area_height != area_h);
            bool encoding_changed = (cfg.encoding != encoding);

            if (font_size_changed || area_changed || encoding_changed)
            {
                // 如果检测到差异，强制重建索引并跳回第一页
#if DBG_BOOK_HANDLE
                Serial.println("[BH] renderCurrentPage: 书签字体/参数不匹配，触发 forceReindex 并重置到第一页");
#endif
                // mark checked to avoid infinite loop
                bookmark_font_checked = true;
                // 更新 BookHandle 的 font_size 以反映当前加载的字体
                extern float font_size;
                this->font_size = font_size;
                extern void requestForceReindex();
                requestForceReindex();
                current_page_index = 0;
                if (!page_positions.empty())
                    cur_pos = page_positions[0];
                else
                    cur_pos = 0;
                last_page.success = false;
                currentPage();
                saveBookmark();
            }
            else
            {
                bookmark_font_checked = true;
            }
        }
        else
        {
            bookmark_font_checked = true;
        }
    }

    // 获取当前页内容进行渲染（避免复制大字符串）
    TextPageResult current = currentPage();
    last_render_char_count_ = count_readable_codepoints(current.page_text);
    bin_font_clear_canvas(dark);
    display_print(current.page_text.c_str(), font_size_param, TFT_BLACK, TL_DATUM,
                  MARGIN_TOP, MARGIN_BOTTOM, MARGIN_LEFT, MARGIN_RIGHT, TFT_WHITE, true, dark);

    // If this page contains any tag start positions, draw a small black dot at top-right
    // 【保护条件】只有在索引完全加载且有效时才检查和显示书签图标
    // page_positions.size() > 1 确保至少有两个页面边界（首页和至少一个后续页面）
    if (g_canvas && pages_loaded && !cached_tags.empty() &&
        current_page_index < page_positions.size() && page_positions.size() > 1)
    {
        size_t page_start = page_positions[current_page_index];
        size_t page_end = (current_page_index + 1 < page_positions.size()) ? page_positions[current_page_index + 1] : getFileSize();
        bool has_tag_here = false;

        // 检查当前页面范围内是否有manual tag（不显示auto tag图标）
        for (const auto &t : cached_tags)
        {
            if (t.position >= page_start && t.position < page_end)
            {
                // 只有非自动书签才显示图标
                if (!t.is_auto)
                {
                    has_tag_here = true;
                    break;
                }
            }
        }

        if (has_tag_here)
        {
            // coordinates: center at (520,20), radius 3, black fill
            g_canvas->fillRect(513, 0, 18, 35, g_config.dark ? TFT_WHITE : TFT_BLACK);
            g_canvas->fillTriangle(513, 35, 522, 30, 531, 35, g_config.dark ? TFT_BLACK : TFT_WHITE);
        }
    }

    // 显示进度条（如果启用）
    if (showPage && pages_loaded && page_positions.size() > 0)
    {
        // 右下角打印页码
        {
            // 将页面索引转换为字符串再打印，避免将 size_t 传递为字符串参数
            std::string page_num = std::to_string(current_page_index + 1);
            // bin_font_print(page_num.c_str(), 20, 0, PAPER_S3_WIDTH - 5, 0, 920 + 15, true, nullptr, TEXT_ALIGN_RIGHT, 0, true, false, false, dark);
            bin_font_print(page_num.c_str(), 20, 0, PAPER_S3_WIDTH - 5, 0, 920 + 15, false, nullptr, TEXT_ALIGN_RIGHT, 0, true, false, false, dark);
        }
        // 计算阅读进度
        float progress = (float)(current_page_index + 1) / (float)page_positions.size();

        // 绘制底部进度条
        if (g_canvas)
        {
            int16_t bar_width = (int16_t)(PAPER_S3_WIDTH * progress);
            int16_t bar_height = 2;
            int16_t bar_y = PAPER_S3_HEIGHT - bar_height;

            // 绘制黑色进度条
            g_canvas->fillRect(0, bar_y, PAPER_S3_WIDTH, bar_height, TFT_LIGHTGRAY);
            g_canvas->fillRect(0, bar_y, bar_width, bar_height, TFT_BLACK);
        }

        // 绘制自动阅读标志
        if (autoread)
        {
            // g_canvas -> drawArc(270, 960, 0, 20, 180,360,TFT_BLACK);
            g_canvas->fillTriangle(0, 470, 0, 490, 15, 480, dark ? TFT_WHITE : TFT_BLACK);
            g_canvas->fillTriangle(540, 470, 540, 490, 525, 480, dark ? TFT_WHITE : TFT_BLACK);
        }
    }

    // For lock screen
    if (showWait)
        ui_push_image_to_canvas("/spiffs/wait.png", 240, 450);

    // 如果 pendingPush 为 false，则在刷新前绘制索引进度半圆（当索引尚未完成时）
    if (!pendingPush)
    {
        // 在页面渲染完成后，如果当前书籍仍在建立索引，绘制上半圆进度指示器
        if (g_canvas && !isIndexingComplete())
        {
            float prog = getIndexingProgress();
            if (prog < 0.0f)
                prog = 0.0f;
            if (prog > 100.0f)
                prog = 100.0f;

            // 当索引完成（100%）时不显示该半圆
            if (prog < 100.0f)
            {
                const int16_t cx = 0;
                const int16_t cy = 960;
                const int16_t r = 30;
                /**/
                // 半圆方案
                g_canvas->fillArc(cx, cy, r / 2, r + 4, 270, 360, TFT_BLACK);
                g_canvas->fillArc(cx, cy, r / 2 + 2, r + 2, 270, 360, TFT_WHITE);
                g_canvas->fillArc(cx, cy, r / 2 + 4, r, 270 + 90 * (prog / 100), 360, TFT_BLACK);
                g_canvas->fillArc(cx, cy, 0, r / 2 - 2, 270 + 90 * (prog / 100), 360, TFT_LIGHTGREY);

                /* 左边条方案*/
                // g_canvas->fillRect(0,0,12, 960,TFT_LIGHTGRAY);
                // g_canvas->fillRect(1,0,10,960*(100-prog)/100,TFT_WHITE);

                /*顶栏*/
                /*
                int32_t tx = 540 * (prog / 100);
                g_canvas->fillRect(0, 0, 540, 8, TFT_BLACK);
                g_canvas->fillRect(tx, 1, 5, 6, TFT_WHITE);

                g_canvas->fillRect(0, 1, tx, 6, TFT_WHITE);
                g_canvas->fillTriangle(tx+5, 1, tx+5, 7, tx + 5+4 , 3, TFT_WHITE);
                */
            }
        }

        // 调试色带
        /*
        g_canvas -> fillRect(0,0, 20, 20, TFT_BLACK);
        g_canvas -> fillRect(0,20, 20, 20, TFT_LIGHTGREY);
        g_canvas -> fillRect(0,40, 20, 20, GREY_MAP_COLOR);
        g_canvas -> fillRect(0,60, 20, 20, TFT_LIGHTGREY);
        */
        // renderType
        // 1- only addBM
        // 2- back from reading menu
        // 3- dark switch
        // 4- back from manual refresh in quick menu
        // other - full render
        if (renderType == 1) // then only refresh top right conner
            bin_font_flush_canvas(false, false, false, NOEFFECT, 500, 0, 30, 40);
        else if (renderType == 2)
        {
            /*
            bin_font_flush_canvas(false, false, false, NOEFFECT, 0,310, 540, 290);
            bin_font_flush_canvas(false, false, false, NOEFFECT, 0, 0, 540, 310);
            bin_font_flush_canvas(false, false, false, NOEFFECT, 0, 600, 540, 360);
            */
            bin_font_flush_canvas(false, false, false, effect);
        }
        else if (renderType == 3)
        {
        }
        else if (renderType == 4)
        {
            bin_font_flush_canvas(false, false, true, effect);
        }

        else
            bin_font_flush_canvas(false, false, false, effect);

        // 打印从 renderCurrentPage 开始到 flushcanvas 执行结束的耗时（毫秒）
        uint32_t bh_render_elapsed_ms = millis() - bh_render_start_ms;
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] renderCurrentPage elapsed to flushcanvas: %u ms\n", (unsigned)bh_render_elapsed_ms);
#endif

        // ===== 渲染完成后预取周边页面缓存 =====
        if (!g_using_progmem_font && g_font_buffer_manager.isInitialized())
        {
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] renderCurrentPage: 开始预取周边页面缓存\n");
#endif
            g_font_buffer_manager.prefetchAround(this);
        }
    }
}

// 检查文件是否被修改过（基于文件大小和时间戳）
bool isFileModified(const std::string &book_file_path)
{
    BookmarkConfig cfg = loadBookmarkForFile(book_file_path);
    if (!cfg.valid || cfg.file_size == 0)
    {
        return false; // 没有书签或文件大小未记录，无法判断
    }

    // 获取当前文件大小
    std::string path = book_file_path;
    if (path.substr(0, 3) == "/sd")
    {
        path = path.substr(3); // 移除 /sd 前缀
    }
    File file = SDW::SD.open(path.c_str(), "r");
    if (!file)
    {
        return true; // 文件无法打开，可能被删除或修改
    }

    file.seek(0, SeekEnd);
    size_t current_size = file.position();
    file.close();

    return (current_size != cfg.file_size);
}

// ==================== 安全文件访问实现 ====================

File BookHandle::openFileForReading()
{
    std::string path = file_path;
    if (path.substr(0, 7) == "/spiffs")
    {
        path = path.substr(7); // 移除 /spiffs 前缀
        return SPIFFS.open(path.c_str(), "r");
    }
    else if (path.substr(0, 3) == "/sd")
    {
        path = path.substr(3); // 移除 /sd 前缀
        return SDW::SD.open(path.c_str(), "r");
    }
    else
    {
        return SDW::SD.open(path.c_str(), "r");
    }
}

void BookHandle::refreshTagsCache()
{
    // Load tags for this file into cached_tags. loadTagsForFile does not throw;
    // avoid exceptions because build is compiled without -fexceptions.
    cached_tags = loadTagsForFile(file_path);
}

bool BookHandle::acquireFileLock(TickType_t timeout)
{
    if (file_access_mutex == nullptr)
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] acquireFileLock: 互斥锁未初始化\n");
#endif
        return false;
    }

    bool acquired = xSemaphoreTake(file_access_mutex, timeout) == pdTRUE;
#if DBG_BOOK_HANDLE
    if (!acquired)
    {
        Serial.printf("[BH] acquireFileLock: 获取锁超时\n");
    }
#endif
    return acquired;
}

void BookHandle::releaseFileLock()
{
    if (file_access_mutex != nullptr)
    {
        xSemaphoreGive(file_access_mutex);
    }
}

// 停止索引并等待（最多等待 timeout_ms 毫秒），返回是否在超时前停止
bool BookHandle::stopIndexingAndWait(unsigned long timeout_ms)
{
    if (!indexing_in_progress)
        return true; // 已经停止

    indexing_should_stop = true;

    unsigned long start = millis();
    while (indexing_in_progress)
    {
        if ((millis() - start) > timeout_ms)
            break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return !indexing_in_progress;
}

size_t BookHandle::saveCurrentPosition()
{
    if (!file_handle)
    {
        return 0;
    }
    return file_handle.position();
}

void BookHandle::restorePosition(size_t pos)
{
    if (file_handle)
    {
        file_handle.seek(pos);
    }
}

// Simplified indexing completion check: only rely on .complete file existence.
// Single source of truth: check if .complete marker file exists on disk.
// The .complete file is created by the indexing implementation when it determines
// the index is complete (e.g., reached EOF or no forward progress).
bool BookHandle::isIndexingComplete() const
{
    std::string complete_marker = complete_filename_for(file_path);
    bool complete_exists = SDW::SD.exists(complete_marker.c_str());

    /*
#if DBG_BOOK_HANDLE
    Serial.printf("[BH:isIndexingComplete] .complete file check: %s -> %s\n",
                  complete_marker.c_str(),
                  complete_exists ? "EXISTS (complete)" : "NOT FOUND (incomplete)");
#endif
*/

    return complete_exists;
}

// Check whether a same-directory .toc file exists for this book
bool BookHandle::hasToc() const
{
    // build expected toc path by replacing extension with .toc
    std::string toc_path = file_path;
    size_t dotpos = toc_path.find_last_of('.');
    if (dotpos != std::string::npos)
        toc_path = toc_path.substr(0, dotpos) + ".idx";
    else
        toc_path += ".idx";

    // SPIFFS paths use /spiffs/ prefix in file_path; SPIFFS.exists expects a path like "/foo"
    if (file_path.rfind("/spiffs/", 0) == 0)
    {
        // remove leading "/spiffs" to get SPIFFS-relative path
        std::string rel = std::string("/") + toc_path.substr(8);
        return SPIFFS.exists(rel.c_str());
    }
    else
    {
        // For SD and generic FS, try direct exists check. Also try removing leading "/sd" if present.
        if (SDW::SD.exists(toc_path.c_str()))
            return true;
        else if (toc_path.rfind("/sd/", 0) == 0)
        {
            std::string rel = toc_path.substr(3); // remove leading "/sd"
            return SDW::SD.exists(rel.c_str());
        }
    }
    return false;
}

bool BookHandle::jumpToTocLine(size_t toc_index)
{
    // If there's no TOC file present, do nothing
    if (!hasToc())
        return false;

    // Fetch toc entry for given index
    TocEntry entry;
    if (!fetch_toc_entry(file_path, toc_index, entry))
        return false;

    // Ensure paging info is available (try to load or generate if needed)
    if (!pages_loaded)
    {
        // best-effort: try to load page file or generate incremental pages
        loadPage();
    }

    // Find the page index corresponding to the toc entry position
    size_t page_idx = 0;
    if (findPageIndexForPosition(entry.position, page_idx))
    {
        return jumpToPage(page_idx);
    }

    // If unable to map to a page index, do nothing
    return false;
}

bool BookHandle::goToRandomToC()
{
    // Ensure TOC exists
    if (!hasToc())
        return false;

    // Quick check: ensure there's at least one entry
    TocEntry entry;
    if (!fetch_toc_entry(file_path, 0, entry))
        return false;

    // Exponential probe to find an upper bound where fetch_toc_entry fails
    size_t low = 0;
    size_t high = 1;
    const size_t MAX_HIGH = (1u << 30);
    while (high < MAX_HIGH)
    {
        if (fetch_toc_entry(file_path, high, entry))
        {
            low = high;
            // prevent overflow
            if (high > MAX_HIGH / 2)
            {
                high = MAX_HIGH;
                break;
            }
            high = high * 2;
        }
        else
        {
            break;
        }
    }

    // Binary search between low and high to find last valid index
    size_t lo = low;
    size_t hi = (high > 0) ? (high - 1) : 0;
    // If high probed succeeded up to MAX_HIGH, hi might equal MAX_HIGH
    if (fetch_toc_entry(file_path, high, entry))
        hi = high;

    while (lo < hi)
    {
        size_t mid = lo + (hi - lo + 1) / 2;
        if (fetch_toc_entry(file_path, mid, entry))
            lo = mid;
        else
            hi = mid - 1;
    }

    size_t total_entries = lo + 1;
    if (total_entries == 0)
        return false;

    // Seed RNG to avoid fixed patterns (use millis() similar to other UI code)
    randomSeed(millis());
    // Generate random index in [0, total_entries-1]
    int rnd = random((int)total_entries);
    if (rnd < 0)
        rnd = 0;

    return jumpToTocLine((size_t)rnd);
}

bool BookHandle::goToRandomPage()
{
    // Determine available indexed size: if indexing complete, use full file size;
    // otherwise use indexing_current_pos which indicates how much has been indexed.
    size_t available_size = 0;
    if (isIndexingComplete())
    {
        available_size = getFileSize();
    }
    else
    {
        available_size = indexing_current_pos;
    }

    if (available_size == 0)
    {
        // No indexed data available
        return false;
    }

    // Seed RNG to avoid fixed pattern
    randomSeed(millis());

    // Generate a random fraction in [0,1)
    int r = random(0, 10000); // 0..9999
    float frac = (float)r / 10000.0f;

    size_t target_pos = (size_t)((double)available_size * (double)frac);

    // Clamp to valid range
    if (target_pos >= available_size)
        target_pos = (available_size > 0) ? (available_size - 1) : 0;

    // Map to page index
    size_t page_idx = 0;
    if (findPageIndexForPosition(target_pos, page_idx))
    {
        return jumpToPage(page_idx);
    }

    // Fallback: if mapping failed but we have page positions, pick a random page index
    if (!page_positions.empty())
    {
        size_t cnt = page_positions.size();
        size_t pick = (size_t)random((int)cnt);
        return jumpToPage(pick);
    }

    return false;
}

// Check whether the .page file for a given book path exists and has the expected header.
static bool page_file_valid_for(const std::string &book_file_path)
{
    std::string page_file = std::string("/bookmarks/") + make_sanitized_base(book_file_path) + ".page";
    if (SDW::SD.exists(page_file.c_str()))
    {
        File pf = SDW::SD.open(page_file.c_str(), "r");
        if (!pf)
            return false;

        // Basic header check
        uint8_t magic[4] = {0};
        if (pf.read(magic, 4) != 4)
        {
            pf.close();
            return false;
        }
        if (!(magic[0] == 'B' && magic[1] == 'P' && magic[2] == 'G' && magic[3] == '1'))
        {
            pf.close();
            return false;
        }

        // Seek to count (offset 8) and read it
        pf.seek(8);
        uint32_t count = 0;
        if (pf.read((uint8_t *)&count, sizeof(count)) != sizeof(count))
        {
            pf.close();
            return false;
        }

        // Validate count (must be at least 1 for an initial page entry)
        if (count == 0)
        {
            pf.close();
            return false;
        }

        // Validate file size: header (12 bytes) + count * 4 (offsets)
        pf.seek(0, SeekEnd);
        size_t actual_size = pf.position();
        size_t expected_size = 12 + ((size_t)count) * 4;
        if (actual_size < expected_size)
        {
            pf.close();
            return false;
        }

        // Optionally, read last offset and verify it is <= file size (basic sanity)
        size_t last_offset_pos = 12 + ((size_t)count - 1) * 4;
        if (last_offset_pos + 4 <= actual_size)
        {
            pf.seek((size_t)last_offset_pos);
            uint32_t last_off = 0;
            if (pf.read((uint8_t *)&last_off, sizeof(last_off)) == sizeof(last_off))
            {
                if ((size_t)last_off > actual_size)
                {
                    pf.close();
                    return false;
                }
            }
        }

        pf.close();
        return true;
    }

    return false;
}

// ---- Minimal indexing API implementations ----

bool BookHandle::getAndClearIndexingShouldStop()
{
    bool v = indexing_should_stop;
    indexing_should_stop = false;
    return v;
}

void BookHandle::requestStopIndexing()
{
    indexing_should_stop = true;
}

void BookHandle::clearPagePositions()
{
    page_positions.clear();
    pages_loaded = false;
    current_page_index = 0;
    cur_pos = 0;
    index_just_completed = false; // 清除索引完成标志
    last_render_char_count_ = 0;
}

void BookHandle::appendPagePosition(size_t pos)
{
    page_positions.push_back(pos);
    // 【关键修复】标记页面数据已加载，避免nextPage()等函数重新加载磁盘文件
    tryInitializeFontCache();
}

File BookHandle::openIndexingReadHandle()
{
    return openFileForReading();
}

bool BookHandle::loadIndexProgressFromDisk()
{
    return loadIndexProgress();
}

// -------------------------------------------------

// 标记索引已完成：移除进度文件（由后台索引器在写入 .complete 后调用）
// Single source of truth: we don't set memory flags, only trust disk .complete file
void BookHandle::markIndexingComplete()
{
    Serial.printf("[BH] markIndexingComplete: called, current pages=%zu\n", page_positions.size());

    indexing_in_progress = false; // stop in-progress flag (runtime state)

    // 【关键】设置标志，表示索引刚完成，下次访问getTotalPages时需要重新加载
    index_just_completed = true;
    Serial.printf("[BH] markIndexingComplete: set index_just_completed=true\n");

    // 【强化】删除 .progress 文件，并验证删除结果
    std::string progress_file = getProgressFileName();
    if (SDW::SD.exists(progress_file.c_str()))
    {
        bool removed = SDW::SD.remove(progress_file.c_str());
#if DBG_BOOK_HANDLE
        if (!removed)
        {
            Serial.printf("[BH:markIndexingComplete] WARNING: failed to remove progress file: %s\n",
                          progress_file.c_str());
        }
        else
        {
            Serial.printf("[BH:markIndexingComplete] Progress file removed: %s\n", progress_file.c_str());
        }
#endif
        (void)removed; // 避免未使用变量警告
    }
}

// 查找给定文件偏移对应的页面索引（找到最大的 i 使 page_positions[i] <= file_pos）
// 返回 true 且 out_index 有效时表示成功；若无法确定则返回 false
bool BookHandle::findPageIndexForPosition(size_t file_pos, size_t &out_index)
{
    // Ensure file/book is open
    if (!isOpen())
    {
        if (!open())
            return false;
    }

    // Ensure page positions are available (will attempt to load/generate as needed)
    if (!pages_loaded)
    {
        // try to load page file; loadPage() will attempt incremental generation if needed
        if (!loadPage())
        {
            // if still no page positions but vector has some entries, continue
            if (page_positions.empty())
                return false;
        }
    }

    if (page_positions.empty())
    {
        // fallback: single page at index 0
        out_index = 0;
        return true;
    }

#if DBG_BOOK_HANDLE
    // Debug: report page_positions summary to help diagnose mapping issues
    size_t first_pos = page_positions.front();
    size_t last_pos = page_positions.back();
    Serial.printf("[BH] findPageIndexForPosition: pages_loaded=%d, page_count=%zu, first=%zu, last=%zu, file_pos=%zu, indexing_pos=%zu\n",
                  pages_loaded ? 1 : 0, page_positions.size(), first_pos, last_pos, file_pos, indexing_current_pos);
#endif

    // Binary search for largest i where page_positions[i] <= file_pos
    size_t lo = 0;
    size_t hi = page_positions.size(); // exclusive

    // If before first page start, return 0
    if (file_pos < page_positions[0])
    {
        out_index = 0;
        return true;
    }

    // If file_pos is beyond the last indexed page position, check if indexing is complete
    if (file_pos > page_positions.back())
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] findPageIndexForPosition: file_pos=%zu > last_page_pos=%zu, indexing_complete=%d\n",
                      file_pos, page_positions.back(), isIndexingComplete() ? 1 : 0);
#endif
        // If indexing is not complete and file_pos is beyond indexed range, return false
        // This prevents jumping to wrong page when user clicks unindexed TOC entries
        if (!isIndexingComplete())
        {
#if DBG_BOOK_HANDLE
            Serial.println("[BH] findPageIndexForPosition: returning false - position beyond indexed range");
#endif
            return false;
        }
        // If indexing is complete, use last page as fallback
        out_index = page_positions.size() - 1;
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] findPageIndexForPosition: using last page index=%zu (page_pos=%zu) as fallback\n",
                      out_index, page_positions[out_index]);
#endif
        return true;
    }

    while (lo + 1 < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (page_positions[mid] <= file_pos)
            lo = mid;
        else
            hi = mid;
    }

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] findPageIndexForPosition: result index=%zu (page_pos=%zu)\n", lo, page_positions[lo]);
#endif

    out_index = lo;
    return true;
}

size_t BookHandle::getPageStart(size_t page_index) const
{
    if (page_positions.empty())
        return (size_t)-1;
    if (page_index >= page_positions.size())
        return (size_t)-1;
    return page_positions[page_index];
}

// 尝试初始化字体缓存（当pages_loaded变成true时调用）
void BookHandle::tryInitializeFontCache()
{
    extern FontBufferManager g_font_buffer_manager;
    extern bool g_using_progmem_font;

    size_t cur_page = getCurrentPageIndex();

#if DBG_BOOK_HANDLE
    Serial.printf("[BH] tryInitializeFontCache: isOpen=%d pages=%zu cur_page=%zu cacheInit=%d progmem=%d\n",
                  (int)isOpen(), page_positions.size(), cur_page,
                  (int)g_font_buffer_manager.isInitialized(), (int)g_using_progmem_font);
#endif

    if (!isOpen() || page_positions.empty() || cur_page >= page_positions.size())
    {
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] Font cache init skipped (invalid state): isOpen=%d pages=%zu cur_page=%zu\n",
                      (int)isOpen(), page_positions.size(), cur_page);
#endif
        return;
    }

    if (g_using_progmem_font)
    {
#if DBG_BOOK_HANDLE
        Serial.println("[BH] Font cache init skipped: PROGMEM font in use");
#endif
        return;
    }

    // 初始化或与当前页面对齐缓存窗口
    if (!g_font_buffer_manager.isInitialized())
    {
        g_font_buffer_manager.initialize(this, cur_page);
#if DBG_BOOK_HANDLE
        Serial.printf("[BH] Font cache initialized for page %u\n", (unsigned)cur_page);
#endif
        return;
    }

    // 已初始化但页面不一致时，执行滚动更新或重建以对齐当前页
    size_t cached_page = g_font_buffer_manager.getCurrentPageIndex();
    if (cached_page != cur_page)
    {
        // 避免在构建缓存时递归触发
        if (g_font_buffer_manager.isInitializationLocked())
        {
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] Font cache mismatch (cached=%u, current=%u) but locked, skipping\n",
                          (unsigned)cached_page, (unsigned)cur_page);
#endif
            return;
        }

        int diff = static_cast<int>(cur_page) - static_cast<int>(cached_page);
        bool forward = diff > 0;

        if (abs(diff) > 2)
        {
            g_font_buffer_manager.initialize(this, cur_page);
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] Font cache reinitialized for page %u (old=%u, diff=%d)\n",
                          (unsigned)cur_page, (unsigned)cached_page, diff);
#endif
        }
        else
        {
            g_font_buffer_manager.scrollUpdate(this, cur_page, forward);
#if DBG_BOOK_HANDLE
            Serial.printf("[BH] Font cache scrolled to page %u (old=%u, diff=%d, forward=%d)\n",
                          (unsigned)cur_page, (unsigned)cached_page, diff, forward ? 1 : 0);
#endif
        }
    }
}
