#include "toc_display.h"
#include "ui_canvas_utils.h"
#include "current_book.h"
#include "text/bin_font_print.h"
#include "text/font_buffer.h"
#include "device/ui_display.h"
#include "globals.h"
#include "../SD/SDWrapper.h"
#include <SPIFFS.h>
#include <vector>
#include <string>
#include <cstdlib>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern M5Canvas *g_canvas;

// Pagination state for TOC display
static int toc_current_page = 0;
static std::string toc_last_book = "";
// When true, force the TOC UI to refresh on next show_toc_ui() call.
// Declared `extern` in toc_display.h so other modules can set it.
bool toc_refresh = false;
// Last recorded TOC entry found by toc_jump_to_position().
size_t toc_last_entry_index = 0;
int toc_last_entry_page = -1;
int toc_last_entry_row = -1;
bool toc_last_entry_valid = false;
static const int TOC_ROWS = 10; // keep in sync with drawing/handlers

// Helper forward declarations
static std::string get_idx_filename(const std::string &book_file_path);
static bool parse_toc_line(const std::string &line, TocEntry &entry);

struct TocPageCache
{
    std::string book_path;
    size_t rows_per_page = TOC_ROWS;
    size_t total_entries = 0;
    size_t file_size = 0;
    bool ready = false;
    std::vector<size_t, PSRAMAllocator<size_t>> page_offsets;
    // Cache entry positions for quick lookup (存放在 PSRAM)
    std::vector<size_t, PSRAMAllocator<size_t>> entry_positions;
    int cached_page = -1;
    std::vector<TocEntry, PSRAMAllocator<TocEntry>> cached_entries;
};

static TocPageCache g_toc_cache;
// Async load state
static bool g_toc_page_loading = false;
static int g_toc_page_loading_index = -1;

static void invalidate_toc_cache()
{
    g_toc_cache.book_path.clear();
    g_toc_cache.rows_per_page = TOC_ROWS;
    g_toc_cache.total_entries = 0;
    g_toc_cache.file_size = 0;
    g_toc_cache.page_offsets.clear();
    g_toc_cache.entry_positions.clear();
    g_toc_cache.ready = false;
    g_toc_cache.cached_page = -1;
    g_toc_cache.cached_entries.clear();
    // Invalidate last recorded lookup as cache is gone
    toc_last_entry_valid = false;
}

static inline void invalidate_toc_page_cache_internal()
{
    g_toc_cache.cached_page = -1;
    g_toc_cache.cached_entries.clear();
}

static File open_idx_file(const std::string &book_file_path)
{
    std::string idx_path = get_idx_filename(book_file_path);
    File idx_file;
    if (book_file_path.rfind("/spiffs/", 0) == 0)
    {
        std::string spiffs_path = std::string("/") + idx_path;
        if (SPIFFS.exists(spiffs_path.c_str()))
            idx_file = SPIFFS.open(spiffs_path.c_str(), "r");
    }
    else
    {
        std::string sd_path = std::string("/") + idx_path;
        if (SDW::SD.exists(sd_path.c_str()))
            idx_file = SDW::SD.open(sd_path.c_str(), "r");
    }
    return idx_file;
}

static bool read_next_line(File &file, std::string &line, size_t &line_start_pos)
{
    line.clear();
    line_start_pos = file.position();

    // 读取单个字符直到遇到换行符或文件结束
    // 避免频繁调用 file.available() 触发 ftell/lseek 导致看门狗超时
    while (true)
    {
        int c = file.read();
        if (c == -1) // EOF
            break;
        if (c == '\r')
            continue;
        if (c == '\n')
            return true;
        line.push_back((char)c);
    }

    return !line.empty();
}

static bool ensure_toc_cache(const std::string &book_file_path)
{
    if (book_file_path.empty())
    {
        invalidate_toc_cache();
        return false;
    }

    // If the currently opened BookHandle has an idx cache loaded into memory,
    // prefer using it to populate TOC positions to avoid scanning the .idx file.
    if (g_current_book && g_current_book->filePath() == book_file_path && g_current_book->isIdxCached())
    {
        const std::vector<size_t> &idx = g_current_book->getIdxPositions();
        if (!idx.empty())
        {
            g_toc_cache.book_path = book_file_path;
            g_toc_cache.rows_per_page = TOC_ROWS;
            g_toc_cache.file_size = g_current_book->getFileSize();
            g_toc_cache.page_offsets.clear();
            g_toc_cache.entry_positions.clear();

            g_toc_cache.entry_positions.assign(idx.begin(), idx.end());
            for (size_t i = 0; i < idx.size(); i += TOC_ROWS)
                g_toc_cache.page_offsets.push_back(idx[i]);

            g_toc_cache.total_entries = idx.size();
            g_toc_cache.ready = true;
            invalidate_toc_page_cache_internal();
            return true;
        }
    }

    File idx_file = open_idx_file(book_file_path);
    if (!idx_file)
    {
        invalidate_toc_cache();
        return false;
    }

    size_t file_size = idx_file.size();
    if (g_toc_cache.ready && g_toc_cache.book_path == book_file_path &&
        g_toc_cache.file_size == file_size && g_toc_cache.rows_per_page == TOC_ROWS)
    {
        idx_file.close();
        return true;
    }

    g_toc_cache.book_path = book_file_path;
    g_toc_cache.rows_per_page = TOC_ROWS;
    g_toc_cache.file_size = file_size;
    g_toc_cache.page_offsets.clear();
    g_toc_cache.entry_positions.clear();
    g_toc_cache.total_entries = 0;
    invalidate_toc_page_cache_internal();

    std::string line;
    line.reserve(256);
    size_t line_start = 0;
    size_t entry_index = 0;
    unsigned long scan_start_time = millis();
    const unsigned long MAX_SCAN_TIME = 5000; // 最多扫描5秒，避免卡死

    while (read_next_line(idx_file, line, line_start))
    {
        TocEntry entry;
        if (!parse_toc_line(line, entry))
            continue;

        if (entry_index % TOC_ROWS == 0)
            g_toc_cache.page_offsets.push_back(line_start);

        // Cache this entry's file position for quick lookup
        g_toc_cache.entry_positions.push_back(entry.position);

        entry_index++;

        // 每处理 50 个条目 yield 一次（减少频率，因为已经优化了 file.available）
        if (entry_index % 50 == 0)
        {
            yield();

            // 超时保护：如果扫描时间过长，停止并使用已扫描的数据
            if (millis() - scan_start_time > MAX_SCAN_TIME)
            {
#if DBG_TOC
                Serial.printf("[TOC] 扫描超时，已处理 %d 个条目，停止扫描\n", entry_index);
#endif
                break;
            }
        }
    }

    idx_file.close();
    g_toc_cache.total_entries = entry_index;
    g_toc_cache.ready = true;
    return true;
}

static bool load_toc_page_entries(const std::string &book_file_path, int page, int rows, std::vector<TocEntry, PSRAMAllocator<TocEntry>> &out_entries)
{
    out_entries.clear();
    out_entries.reserve(rows);

    if (!ensure_toc_cache(book_file_path))
        return false;

    // Fast path: if the current BookHandle has idx positions/titles cached, use them
    if (g_current_book && g_current_book->filePath() == book_file_path && g_current_book->isIdxCached())
    {
        const auto &positions = g_current_book->getIdxPositions();
        const auto &titles = g_current_book->getIdxTitles();
        if (positions.empty())
            return false;

        size_t total_entries = positions.size();
        size_t page_count = (total_entries + rows - 1) / rows;
        if (page < 0 || (size_t)page >= page_count)
            return true;

        size_t start_idx = (size_t)page * rows;
        size_t end_idx = std::min(start_idx + rows, total_entries);
        for (size_t i = start_idx; i < end_idx; ++i)
        {
            TocEntry te;
            te.index = (int)i;
            te.position = positions[i];
            if (i < titles.size())
                te.title = titles[i];
            else
                te.title.clear();
            // percentage if file size known
            if (g_toc_cache.file_size > 0)
                te.percentage = ((double)te.position / (double)g_toc_cache.file_size) * 100.0f;
            else
                te.percentage = 0.0f;
            out_entries.push_back(te);
        }
        return true;
    }

    if (g_toc_cache.cached_page == page && g_toc_cache.cached_page != -1)
    {
        out_entries = g_toc_cache.cached_entries;
        return true;
    }

    File idx_file = open_idx_file(book_file_path);
    if (!idx_file)
        return false;

    if (page < 0 || (size_t)page >= g_toc_cache.page_offsets.size())
    {
        idx_file.close();
        return true;
    }

    idx_file.seek(g_toc_cache.page_offsets[page]);

    std::string line;
    line.reserve(256);
    size_t dummy = 0;
    int collected = 0;

    while (collected < rows && read_next_line(idx_file, line, dummy))
    {
        TocEntry entry;
        if (!parse_toc_line(line, entry))
            continue;
        out_entries.push_back(entry);
        collected++;
    }

    idx_file.close();
    g_toc_cache.cached_page = page;
    g_toc_cache.cached_entries = out_entries;
    return true;
}

bool fetch_toc_entry(const std::string &book_file_path, size_t toc_index, TocEntry &entry)
{
    // Prefer BookHandle in-memory cache if available
    if (g_current_book && g_current_book->filePath() == book_file_path && g_current_book->isIdxCached())
    {
        const auto &positions = g_current_book->getIdxPositions();
        const auto &titles = g_current_book->getIdxTitles();
        if (toc_index < positions.size())
        {
            entry.index = (int)toc_index;
            entry.position = positions[toc_index];
            if (toc_index < titles.size())
                entry.title = titles[toc_index];
            else
                entry.title.clear();

            // percentage if file size known
            if (g_toc_cache.file_size > 0)
                entry.percentage = ((double)entry.position / (double)g_toc_cache.file_size) * 100.0f;
            else
                entry.percentage = 0.0f;
            return true;
        }
        return false;
    }

    // Fallback to file-based fetch (reads only one page worth)
    if (!ensure_toc_cache(book_file_path))
        return false;

    if (toc_index >= g_toc_cache.total_entries || g_toc_cache.rows_per_page == 0)
        return false;

    size_t page = toc_index / g_toc_cache.rows_per_page;
    size_t offset_in_page = toc_index % g_toc_cache.rows_per_page;

    if (page >= g_toc_cache.page_offsets.size())
        return false;

    File idx_file = open_idx_file(book_file_path);
    if (!idx_file)
        return false;

    idx_file.seek(g_toc_cache.page_offsets[page]);

    std::string line;
    line.reserve(256);
    size_t dummy = 0;
    size_t local_idx = 0;

    while (read_next_line(idx_file, line, dummy))
    {
        TocEntry candidate;
        if (!parse_toc_line(line, candidate))
            continue;

        if (local_idx == offset_in_page)
        {
            entry = candidate;
            idx_file.close();
            return true;
        }

        local_idx++;

        if (local_idx > offset_in_page)
            break;
    }

    idx_file.close();
    return false;
}

// Load the TOC page that contains `toc_index` (if necessary) and return the title.
// This is a fast path that reads at most one page (TOC_ROWS entries) from the .idx
// file when titles are not already cached.
bool get_toc_title_for_index(const std::string &book_file_path, size_t toc_index, std::string &out_title)
{
    out_title.clear();
    if (!ensure_toc_cache(book_file_path))
        return false;

    if (toc_index >= g_toc_cache.total_entries || g_toc_cache.rows_per_page == 0)
        return false;

    size_t page = toc_index / g_toc_cache.rows_per_page;
    size_t offset_in_page = toc_index % g_toc_cache.rows_per_page;

    // If the current BookHandle has titles loaded in memory, use them directly.
    if (g_current_book && g_current_book->filePath() == book_file_path && g_current_book->isIdxCached())
    {
        const auto &titles = g_current_book->getIdxTitles();
        if (toc_index < titles.size())
        {
            out_title = titles[toc_index];
            return true;
        }
        return false;
    }

    // Fast non-blocking path: if the desired TOC page is already cached, return title.
    if (g_toc_cache.cached_page == (int)page && !g_toc_cache.cached_entries.empty())
    {
        if (offset_in_page < g_toc_cache.cached_entries.size())
        {
            out_title = g_toc_cache.cached_entries[offset_in_page].title;
            return true;
        }
        return false;
    }

    // Not cached — caller should decide whether to trigger an async load.
    return false;
}

// Background task param
struct TocLoaderParam
{
    char *book_path;
    int page;
};

static void toc_page_loader_task(void *pvParameters)
{
    TocLoaderParam *p = (TocLoaderParam *)pvParameters;
    if (!p)
        vTaskDelete(NULL);

    std::string book(p->book_path);
    free(p->book_path);
    int page = p->page;
    delete p;

    std::vector<TocEntry, PSRAMAllocator<TocEntry>> entries;
    // Load page entries (this may block on SD), but it's okay in background
    if (load_toc_page_entries(book, page, (int)g_toc_cache.rows_per_page, entries))
    {
        // Update cache atomically-ish
        g_toc_cache.cached_page = page;
        g_toc_cache.cached_entries = entries;
    }

    g_toc_page_loading = false;
    g_toc_page_loading_index = -1;

    vTaskDelete(NULL);
}

void start_async_load_toc_page(const std::string &book_file_path, int page_index)
{
    if (book_file_path.empty() || page_index < 0)
        return;

    if (!ensure_toc_cache(book_file_path))
        return;

    if (g_toc_cache.cached_page == page_index)
        return; // already loaded

    if (g_toc_page_loading && g_toc_page_loading_index == page_index)
        return; // already loading

    // Prepare param
    TocLoaderParam *p = new TocLoaderParam();
    p->book_path = strdup(book_file_path.c_str());
    p->page = page_index;

    g_toc_page_loading = true;
    g_toc_page_loading_index = page_index;

    // Create a background FreeRTOS task to load the page
    BaseType_t rc = xTaskCreatePinnedToCore(toc_page_loader_task, "TocLoader", 8192, p, 1, NULL, 1);
    if (rc != pdPASS)
    {
        // Failed to create task; clean up
        free(p->book_path);
        delete p;
        g_toc_page_loading = false;
        g_toc_page_loading_index = -1;
    }
}

void toc_prefetch_for_book(const std::string &book_file_path)
{
    if (book_file_path.empty())
        return;

    // 延迟加载：只在实际需要显示TOC时才扫描，避免打开书籍时阻塞
    // ensure_toc_cache will be called lazily when toc_show() is called
    // This prevents blocking on large files during book open
#if DBG_TOC
    Serial.printf("[TOC] 跳过预加载，延迟到实际显示时\n");
#endif
}

// Pagination control implementations
void toc_next_page()
{
    if (!g_current_book)
        return;

    if (!ensure_toc_cache(g_current_book->filePath()))
        return;

    int total_pages = 0;
    if (g_toc_cache.total_entries > 0)
        total_pages = (int)((g_toc_cache.total_entries + TOC_ROWS - 1) / TOC_ROWS);

    if (total_pages > 0 && toc_current_page + 1 < total_pages)
        toc_current_page++;
}

void toc_prev_page()
{
    if (toc_current_page > 0)
        toc_current_page--;
}

void toc_reset_page()
{
    toc_current_page = 0;
    toc_last_book.clear();
    invalidate_toc_cache();
}

void toc_invalidate_page_cache()
{
    invalidate_toc_page_cache_internal();
}

int toc_get_current_page()
{
    return toc_current_page;
}

void toc_jump_to_position(const std::string &book_file_path, size_t file_pos)
{
    if (book_file_path.empty())
        return;

    // Ensure cache is built
    if (!ensure_toc_cache(book_file_path))
        return;

    if (g_toc_cache.total_entries == 0 || g_toc_cache.entry_positions.empty())
        return;
    // Reset last-entry record until we compute it below
    toc_last_entry_valid = false;

    // Find the entry with the largest position <= file_pos.
    // Use binary search on the cached in-memory positions for speed.
    size_t best_entry_index = 0;
    auto &positions = g_toc_cache.entry_positions;
    auto it = std::upper_bound(positions.begin(), positions.end(), file_pos);
    if (it == positions.begin())
    {
        best_entry_index = 0;
    }
    else
    {
        --it;
        best_entry_index = static_cast<size_t>(it - positions.begin());
    }

    // Calculate which page this entry is on
    if (g_toc_cache.rows_per_page > 0)
    {
        int target_page = best_entry_index / g_toc_cache.rows_per_page;
        int total_pages = (int)((g_toc_cache.total_entries + TOC_ROWS - 1) / TOC_ROWS);

        // Clamp to valid range
        if (target_page < 0)
            target_page = 0;
        if (target_page >= total_pages)
            target_page = total_pages - 1;

        toc_current_page = target_page;

        // Record the lookup result for later queries
        toc_last_entry_index = best_entry_index;
        toc_last_entry_page = target_page;
        toc_last_entry_row = (int)(best_entry_index % g_toc_cache.rows_per_page);
        toc_last_entry_valid = true;
    }
}

bool find_toc_entry_for_position(const std::string &book_file_path, size_t file_pos,
                                 size_t &out_entry_index, int &out_page, int &out_row_in_page,
                                 bool &out_on_current_page)
{
    out_entry_index = 0;
    out_page = -1;
    out_row_in_page = -1;
    out_on_current_page = false;

    if (book_file_path.empty())
        return false;

    if (!ensure_toc_cache(book_file_path))
        return false;

    if (g_toc_cache.entry_positions.empty() || g_toc_cache.total_entries == 0)
        return false;

    // Use upper_bound to find first element > file_pos, then step back
    auto &positions = g_toc_cache.entry_positions;
    size_t best_entry = 0;
    auto it = std::upper_bound(positions.begin(), positions.end(), file_pos);
    if (it == positions.begin())
    {
        best_entry = 0;
    }
    else
    {
        --it;
        best_entry = static_cast<size_t>(it - positions.begin());
    }

    out_entry_index = best_entry;

    if (g_toc_cache.rows_per_page == 0)
        return false;

    out_page = (int)(best_entry / g_toc_cache.rows_per_page);
    out_row_in_page = (int)(best_entry % g_toc_cache.rows_per_page);
    out_on_current_page = (out_page == toc_current_page);

    return true;
}

// Helper: sanitize book file path to generate .idx filename
static std::string get_idx_filename(const std::string &book_file_path)
{
    std::string safe = book_file_path;
    // Remove leading /sd/ or /spiffs/ prefix
    if (safe.rfind("/sd/", 0) == 0)
        safe = safe.substr(4);
    else if (safe.rfind("/spiffs/", 0) == 0)
        safe = safe.substr(8);

    // Replace extension with .idx
    size_t dot = safe.find_last_of('.');
    if (dot != std::string::npos)
        safe = safe.substr(0, dot) + ".idx";
    else
        safe += ".idx";

    return safe;
}

// Parse single line from .idx file
// Format: #序号#, #标题#, #字节位置#, #百分比#,
static bool parse_toc_line(const std::string &line, TocEntry &entry)
{
    if (line.empty() || line[0] != '#')
        return false;

    // Find all # delimiters
    std::vector<size_t> hash_pos;
    for (size_t i = 0; i < line.size(); ++i)
    {
        if (line[i] == '#')
            hash_pos.push_back(i);
    }

    if (hash_pos.size() < 8) // Need at least 8 # for format: #序号#, #标题#, #字节位置#, #百分比#,
        return false;

    // Extract fields between # delimiters
    // #序号# is between hash_pos[0] and hash_pos[1]
    std::string index_str = line.substr(hash_pos[0] + 1, hash_pos[1] - hash_pos[0] - 1);
    // #标题# is between hash_pos[2] and hash_pos[3]
    std::string title_str = line.substr(hash_pos[2] + 1, hash_pos[3] - hash_pos[2] - 1);
    // #字节位置# is between hash_pos[4] and hash_pos[5]
    std::string pos_str = line.substr(hash_pos[4] + 1, hash_pos[5] - hash_pos[4] - 1);
    // #百分比# is between hash_pos[6] and hash_pos[7]
    std::string pct_str = line.substr(hash_pos[6] + 1, hash_pos[7] - hash_pos[6] - 1);

    // Parse with basic validation
    if (index_str.empty() || pos_str.empty() || pct_str.empty())
        return false;

    // Simple atoi/atof style parsing (no exceptions)
    entry.index = atoi(index_str.c_str());
    entry.title = title_str;
    entry.position = strtoull(pos_str.c_str(), nullptr, 10);
    entry.percentage = atof(pct_str.c_str());

    return true;
}

void show_toc_ui(M5Canvas *canvas)
{
    M5Canvas *target = canvas ? canvas : g_canvas;
    if (!target)
        return;

    // 清理TOC和书名缓存，避免索引期间缓存与字体文件状态不一致导致乱码
    // 这些缓存在下次需要时会自动重建
    clearTocCache();
    clearBookNameCache();

    // Left area (same dimensions as tag UI)
    const int16_t x = 0;
    const int16_t y = 0;
    const int16_t deltay = 32; // leave small top margin
    const int16_t w = 450;
    const int16_t h = 960;
    const int rows = TOC_ROWS;
    const int row_h = h * 0.9 / rows; // ~86

    // background
    target->fillRect(x, y, w, h, TFT_WHITE);
    target->fillRect(x + w, y, 540 - w, h, TFT_BLACK);

    // Load TOC for current book
    std::vector<TocEntry, PSRAMAllocator<TocEntry>> toc_entries;
    std::string current_book_path = "";
    if (g_current_book)
        current_book_path = g_current_book->filePath();

    // Reset page when book changes, and jump to current reading position
    if (current_book_path != toc_last_book || toc_refresh)
    {
        toc_refresh = false;
        toc_last_book = current_book_path;
        // Note: Do NOT call invalidate_toc_cache() here!
        // The cache was already built by toc_prefetch_for_book() when the book was opened.
        // The ensure_toc_cache() function has its own logic to detect if cache needs rebuilding.

        // Jump to the TOC page that contains the entry for current reading position
        if (g_current_book)
        {
            size_t current_pos = g_current_book->position();
            toc_jump_to_position(current_book_path, current_pos);
        }
        else
        {
            toc_current_page = 0;
        }
    }

    if (!current_book_path.empty())
        load_toc_page_entries(current_book_path, toc_current_page, rows, toc_entries);

    // Draw rows for current page
    for (int i = 0; i < rows; ++i)
    {
        int16_t ry = y + i * row_h + deltay; // inner padding
        if (i < (int)toc_entries.size())
        {
            // If this row corresponds to the last recorded TOC entry, draw a highlight background
            bool is_highlight = (toc_last_entry_valid && toc_last_entry_page == toc_current_page && i == toc_last_entry_row);
            if (is_highlight)
            {
                // Slightly inset the highlight so borders remain visible
                // target->fillTriangle(x + 420, ry + 50, x + 420, ry + 70, x + 430, ry + 60,TFT_BLACK);
            }

            const TocEntry &te = toc_entries[i];

            // determine if this TOC position is already indexed (available for jump)
            bool available = true;
            if (g_current_book)
            {
                available = (g_current_book->isIndexingComplete() ||
                             te.position <= g_current_book->getIndexingCurrentPos());
            }

            // title left, percentage right
            const char *title = te.title.c_str();
            char pctbuf[32];
            int pct = (int)(te.percentage + 0.5f);
            snprintf(pctbuf, sizeof(pctbuf), " %d%%", pct);

            ry = ry + 50;
            // choose color index: 0 for normal, 3 for not-yet-indexed
            // int text_color = available ? 0 : 3;
            // not use grey color as it will make the V3 scaler rendering complicated...
            int text_color = 0;

            // title area: allow up to ~300px width => 420
            bin_font_print(title, 24, text_color, 300 + 100, x + 48, ry, true, target, TEXT_ALIGN_LEFT, 300 + 100);
            // percentage aligned to right column
            //            bin_font_print(pctbuf, 24, text_color, 120, x + 350, ry, true, target, TEXT_ALIGN_LEFT, 120);
            // If highlighted, prefer a stronger color for text (keep using 0 if unsure)
            if (is_highlight)
            {
                // leave text_color as-is; if needed change to a specific index
                // Circle marker for each entry
                canvas->fillCircle(x + 20, ry + 12, 4, TFT_BLACK);
                canvas->drawCircle(x + 20, ry + 12, 6, TFT_BLACK);
                canvas->drawCircle(x + 20, ry + 12, 8, TFT_BLACK);
            }
            else
            {
                // Circle marker for each entry
                if (available)
                    canvas->drawCircle(x + 20, ry + 12, 3, TFT_BLACK);
            }
        }
        // else empty row — leave blank
    }

    // Screw decorations
    drawScrew(canvas, 20, 20);
    drawScrew(canvas, 520, 20);
    drawScrew(canvas, 20, 940);
    drawScrew(canvas, 520, 940);

    // Lines
    canvas->drawLine(450, 40, 540, 40, TFT_WHITE);
    canvas->drawLine(450, 920, 540, 920, TFT_WHITE);
    canvas->drawLine(0, 40, 450, 40, TFT_BLACK);
    canvas->drawLine(0, 920, 450, 920, TFT_BLACK);

    // Switcher (tab indicator)
    canvas->drawLine(225, 0, 235, 40, TFT_BLACK);
    canvas->drawLine(230, 0, 240, 40, TFT_BLACK);
    canvas->floodFill(220, 10, TFT_LIGHTGRAY);

    // Tab labels: "目录" active (darker), "书签" lighter
    bin_font_print("目录", 24, 0, 200, 140, 8, false, canvas);
    bin_font_print("书签", 24, 0, 200, 270, 8, false, canvas);

    // Icon (same as tag UI)
    canvas->fillRect(450 + 35, 40, 20, 35, TFT_LIGHTGRAY);
    canvas->fillTriangle(450 + 35, 75, 460 + 35, 70, 470 + 35, 75, TFT_BLACK);
    canvas->fillCircle(460 + 35, 50, 3, TFT_BLACK);

    // Pagination
    canvas->drawLine(235, 920, 225, 960, TFT_BLACK);
    canvas->drawLine(240, 920, 230, 960, TFT_BLACK);

    // Previous Page
    canvas->fillTriangle(120, 950, 160, 950, 140, 930, TFT_BLACK);
    // Next Page
    canvas->fillTriangle(304, 930, 344, 930, 324, 950, TFT_BLACK);

    // push to display if using global canvas
    if (!canvas && g_canvas)
    {
        M5.Display.powerSaveOff();
        g_canvas->pushSprite(0, 0);
        M5.Display.waitDisplay();
        M5.Display.powerSaveOn();
    }
    else
    {
        bin_font_flush_canvas();
    }
}
