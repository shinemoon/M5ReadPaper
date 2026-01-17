#pragma once

#include <string>
#include <vector>
#include <FS.h>
#include <M5Unified.h>
#include "text_handle.h"
#include "readpaper.h"
#include "text/tags_handle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>

// 简单的书籍翻页封装，使用现有的 read_text_page 接口
class BookHandle
{
public:
    BookHandle(const std::string &path, int16_t area_w, int16_t area_h, float fsize,
               TextEncoding enc = TextEncoding::AUTO_DETECT);
    ~BookHandle();

    // 打开/关闭文件句柄（BookHandle 在构造时会自动 open，清理时会 close）
    bool open();
    void close();
    bool isOpen() const;

    // 设置/读取当前位置
    void setPosition(size_t pos);
    size_t position() const;
    size_t getFileSize() const; // 获取文件大小
    const std::string &filePath() const { return file_path; }
    std::string getBookName() const; // 获取书名（从文件路径中提取文件名）

    // 翻页操作
    // nextPage: 从当前位置读取下一页并前进；返回的 result.page_end_pos 为下一次调用的起点
    TextPageResult nextPage();
    // prevPage: 尝试使用历史栈回退；若历史为空，则调用 read_text_page(..., backward=true) 作为后备
    TextPageResult prevPage();

    // 读取当前页（不改变历史）
    TextPageResult currentPage();

    // 清空历史（例如打开新书）
    void clearHistory();

    // 获取当前页面摘要（前DIGEST_NUM个字符）
    const std::string &getCurrentDigest() const { return current_digest; }

    // Paging
    bool loadPage();

    // 分页相关方法
    bool generatePageFile();             // 生成分页文件
    bool loadPageFile();                 // 从文件加载分页信息
    bool savePageFile();                 // 保存分页信息到文件
    std::string getPageFileName() const; // 获取分页文件名
    size_t getTotalPages() const;  // 获取总页数（可能触发重新加载）
    size_t getCurrentPageIndex() const { return current_page_index; } // 获取当前页索引
    bool jumpToPage(size_t page_index);                               // 跳转到指定页
    bool isPageCompleted() const { return page_completed; }           // 当前页是否完成
    void setPageCompleted(bool completed) { page_completed = completed; }

    // 增量索引建立机制
    bool generatePageFileIncremental();      // 增量生成分页文件
    bool loadIndexProgress();                // 加载索引建立进度
    bool saveIndexProgress();                // 保存索引建立进度
    bool continueIndexGeneration();          // 继续索引建立
    std::string getProgressFileName() const; // 获取进度文件名
    bool isIndexingComplete() const;         // implemented in .cpp: checks in-memory flag and on-disk markers
    bool isIndexingInProgress() const { return indexing_in_progress; }
    float getIndexingProgress() const;                   // 获取索引建立进度百分比
    void stopIndexing() { indexing_should_stop = true; } // 停止索引建立
    // 停止索引并等待（最多等待 timeout_ms 毫秒），返回是否在超时前停止
    bool stopIndexingAndWait(unsigned long timeout_ms);

    // ---- Minimal indexing API for Background Indexer ----
    size_t getIndexingCurrentPos() const { return indexing_current_pos; }
    void setIndexingCurrentPos(size_t pos) { indexing_current_pos = pos; }
    size_t getIndexingFileSize() const { return indexing_file_size; }
    void setIndexingFileSize(size_t s) { indexing_file_size = s; }
    void setIndexingInProgress(bool v) { indexing_in_progress = v; }
    bool getAndClearIndexingShouldStop();
    void requestStopIndexing();

    void clearPagePositions();
    void appendPagePosition(size_t pos);
    void setPagesLoaded(bool v) { pages_loaded = v; }
    bool isPagesLoaded() const { return pages_loaded; }
    


    // 记录"本次索引工作循环的起点"，用于在主循环中检测是否出现"连续两次起点相同（无前进）"的情形
    size_t getLastIndexCycleStart() const { return last_index_cycle_start_; }
    void setLastIndexCycleStart(size_t pos) { last_index_cycle_start_ = pos; }
    // 无前进次数累计（用于 N 次判定再触发强制完成）
    uint8_t getNoProgressStreak() const { return no_progress_streak_; }
    void setNoProgressStreak(uint8_t v) { no_progress_streak_ = v; }
    void resetNoProgressStreak() { no_progress_streak_ = 0; }
    void incNoProgressStreak() { if (no_progress_streak_ < 255) ++no_progress_streak_; }
    void resetIndexCycleHeuristics() { last_index_cycle_start_ = (size_t)-1; no_progress_streak_ = 0; }

    File openIndexingReadHandle();
    bool loadIndexProgressFromDisk();
    // Note: persistent page/progress file writes are handled by the background index task.
    // ------------------------------------------------------

    // 非阻塞/短超时的文件锁接口（供后台索引在UI优先时快速让路）
    bool tryAcquireFileLock(TickType_t timeout = 0);
    void releaseFileLockPublic();

    // 后台索引建立支持
    bool canContinueIndexing() const { return !isIndexingComplete() && !indexing_in_progress; }
    void resumeIndexingIfNeeded(); // 如果需要且可以，恢复索引建立

    // 强制重新索引
    bool forceReindex(bool already_holding_lock = false); // 强制重新建立索引

    // 标记索引已完成（由后台索引器调用）
    void markIndexingComplete();

    // 基于页面索引的翻页方法
    bool hasNextPage() const;  // 检查是否有下一页（可能触发重新加载）
    bool hasPrevPage() const { return current_page_index > 0; }

    // 自动书签功能
    bool loadBookmarkAndJump(); // 加载书签并跳转到保存的位置
    bool saveBookmark();        // 保存当前位置到书签文件

    // Getter方法用于书签系统访问私有成员
    int16_t getAreaWidth() const { return area_w; }
    int16_t getAreaHeight() const { return area_h; }
    float getFontSize() const { return font_size; }
    TextEncoding getEncoding() const { return encoding; }
    bool getShowLabel() const { return showlabel; }
    bool getKeepOrg() const { return keep_org_; }
    bool getDrawBottom() const { return draw_bottom_; }
    bool getVerticalText() const { return vertical_text_; }
    // 阅读时间（小时/分钟），用于记录最后阅读时间
    int16_t getReadHour() const { return readhour; }
    int16_t getReadMin() const { return readmin; }
    size_t getCurrentPageCharCount() const;

    // Setter方法
    // 设置是否在锁屏时显示标签，并立即将该设置保存到书签文件以便重启后保留
    void setShowLabel(bool show)
    {
        showlabel = show;
        saveBookmark();
    }
    // keepOrg: 当为 true 时跳过繁简转换（保持原始书籍组织），并立即保存到书签
    void setKeepOrg(bool keep)
    {
        keep_org_ = keep;
        saveBookmark();
    }
    // drawBottom: 当为 true 时在文字下方画底线，并立即保存到书签
    void setDrawBottom(bool draw)
    {
        draw_bottom_ = draw;
        saveBookmark();
    }
    // verticalText: 当为 true 时竖排显示文字，并立即保存到书签
    void setVerticalText(bool vertical)
    {
        vertical_text_ = vertical;
        saveBookmark();
    }

    // 当外部字体加载/切换后调用此方法以更新 BookHandle 的字体大小并
    // 将该信息立即持久化到书签文件（避免必须重新打开书籍才能保存）
    void setFontSize(float f)
    {
        font_size = f;
        // persist change to bookmark so next session and index logic can see it
        saveBookmark();
    }

    // 标记对象正在被关闭（用于避免后台索引在删除时仍使用对象）
    void markForClose();
    bool isClosing() const;

    // 渲染当前页面内容到屏幕
    void renderCurrentPage(float font_size, M5Canvas *canvas = nullptr, bool showPage = true, bool showWait = false, bool pendingPush=false);
    // 查找给定文件偏移对应的页面索引（找到最大的 i 使 page_positions[i] <= file_pos）
    // 返回 true 且 out_index 有效时表示成功；若无法确定则返回 false
    bool findPageIndexForPosition(size_t file_pos, size_t &out_index);
    // 返回指定页索引的起始字节位置（若不存在则返回 (size_t)-1）
    size_t getPageStart(size_t page_index) const;
    // Jump to the page corresponding to a TOC entry index (0-based).
    // If the book has no TOC or the index is invalid, do nothing and return false.
    bool jumpToTocLine(size_t toc_index);
    // Select a random TOC entry (if .idx exists) and jump to it.
    // Returns true on successful jump, false otherwise.
    bool goToRandomToC();
    // Jump to a random page within the already-indexed portion of the book.
    // Returns true on success, false otherwise.
    bool goToRandomPage();
    // Tags cache utilities
    void refreshTagsCache();
    const std::vector<TagEntry> &getCachedTags() const { return cached_tags; }
    std::string getCompleteFileName() const;
    // Diagnostic id (stable for lifetime) used by bg-index/logging
    size_t getId() const;

    // 增加阅读时间 1 分钟（在读取到 readhour/readmin 的基础上 +1min），
    // 会处理分钟进位到小时以及上限 (readhour==9999 && readmin==59) 的情况。
    void incrementReadingMinute();

    // 尝试初始化字体缓存（供外部字体加载流程调用）
    void tryInitializeFontCache();
    
private:
    std::string file_path;
    File file_handle; // 常驻文件句柄，BookHandle 负责 open/close
    size_t cur_pos = 0;
    int16_t area_w = 0;
    int16_t area_h = 0;
    float font_size = 0.0f;
    TextEncoding encoding = TextEncoding::AUTO_DETECT;
    
    // 字体缓存是否已在本BookHandle实例中初始化过
    bool font_cache_initialized = false;

    // 历史：固定大小环形缓冲，保存最近的页面起点以支持回退
    static const size_t MAX_HISTORY = 50;
    size_t history_buf[MAX_HISTORY];
    size_t history_head = 0;  // index of oldest item
    size_t history_count = 0; // number of items currently in buffer

    // 当前页面索引（0-based）
    size_t current_page_index = 0;

    // 页面完成状态
    bool page_completed = false; // 当前页是否已完整读取

    // 增量索引建立状态（单线程，不需要 volatile）
    // Note: indexing_complete flag removed - we trust disk .complete file as single source of truth
    bool indexing_in_progress = false;     // 索引是否正在建立中（运行时状态）
    bool indexing_should_stop = false;     // 是否应该停止索引建立
    size_t indexing_current_pos = 0;       // 当前索引建立到的文件位置
    size_t indexing_file_size = 0;         // 文件总大小
    unsigned long indexing_start_time = 0; // 索引建立开始时间
    mutable bool index_just_completed = false;     // 【新增】标记索引刚刚完成，需要重新加载一次（mutable允许在const函数中修改）
    // 最近一次"索引工作循环"的起点位置（仅用于启发式收尾判断）
    size_t last_index_cycle_start_ = (size_t)-1;
    uint8_t no_progress_streak_ = 0;

    // 文件访问安全机制
    SemaphoreHandle_t file_access_mutex; // 文件访问互斥锁

    // 安全文件访问方法
    File openFileForReading();                                // 创建独立的只读文件句柄
    bool acquireFileLock(TickType_t timeout = portMAX_DELAY); // 获取文件访问锁
    void releaseFileLock();                                   // 释放文件访问锁
    size_t saveCurrentPosition();                             // 保存当前文件位置
    void restorePosition(size_t pos);                         // 恢复文件位置

    // 缓存最近读到的一页
    TextPageResult last_page;
    size_t last_render_char_count_ = 0;

    // 当前页面摘要（前DIGEST_NUM个字符）
    std::string current_digest;

    // 分页数组：存储每页的起始位置
    std::vector<size_t> page_positions;
    bool pages_loaded = false;

    // 锁屏时是否显示标签信息
    bool showlabel = true;
    // 如果在 open() 中因为原始文件缺失而回退到默认文件，跳过书签恢复（因为默认文件可能有不同的书签）
    bool skip_bookmark_on_open = false;

    void updateCurrentDigest(const std::string &page_text);
    // 是否保持原始组织（true: 跳过繁简转换）
    // 默认改为 true：书签配置中默认跳过繁简转换
    bool keep_org_ = true;
    // 是否在文字下方画底线（true: 画底线）
    bool draw_bottom_ = true;
    // 是否竖排显示文字（true: 竖排显示）
    bool vertical_text_ = false;
    // 渲染阶段已检查书签字体一致性，避免重复触发重索引
    bool bookmark_font_checked = false;
    // 缓存安全化后的基名，避免频繁分配
    std::string sanitized_base_;
    // 标记对象正在被关闭（供外部调用以便后台索引能安全退出）
    bool closing_ = false;
    // Cached tags for this book (kept in sync when opening or when tags are modified)
    std::vector<TagEntry> cached_tags;
    // Whether a same-directory .idx file exists for this book (set during open())
    bool is_indexed_ = false;
    // PSRAM-backed idx cache
    // Primary in-heap cache (vector). If PSRAM allocation is available we may allocate
    // a raw buffer and copy into it; idx_positions_psram_ptr will point to that buffer.
    std::vector<size_t> idx_positions_psram_;
    // Titles cache corresponding to idx_positions_psram_. Kept in heap (strings)
    std::vector<std::string> idx_titles_psram_;
    size_t *idx_positions_psram_ptr = nullptr; // raw PSRAM pointer (if allocated)
    size_t idx_positions_psram_count = 0;
    bool idx_psram_loaded_ = false;
    // 最后阅读时间（小时/分钟），默认初始值为0
    std::int16_t readhour = 0;
    std::int16_t readmin = 0;
public:
    // Expose read-only query for whether the book has a sidecar .idx file
    bool isIndexed() const { return is_indexed_; }
    // Load/clear idx positions into PSRAM-backed cache
    bool loadIdxToPSRAM();                // load .idx into PSRAM or heap-backed cache
    void clearIdxPSRAM();                 // release PSRAM cache
    const std::vector<size_t> &getIdxPositions() const { return idx_positions_psram_; }
    const std::vector<std::string> &getIdxTitles() const { return idx_titles_psram_; }
    // Consider idx cached when we have parsed positions (vector non-empty).
    bool isIdxCached() const { return !idx_positions_psram_.empty(); }
    // Query whether a same-directory sidecar .toc file exists for this book
    bool hasToc() const;
};

// 返回 BookHandle 管理的文件路径（返回副本）。
// 如果传入 nullptr，则返回空字符串，方便外部安全读取。
static inline std::string getBookFilePath(const BookHandle *bh)
{
    if (!bh)
        return std::string();
    return bh->filePath();
}

// 索引建立进度结构
struct IndexProgress
{
    std::string file_path;
    size_t file_size;
    size_t current_pos;     // 当前处理到的文件位置
    size_t pages_generated; // 已生成的页面数
    int16_t area_width;
    int16_t area_height;
    float font_size;
    TextEncoding encoding;
    unsigned long start_time;  // 开始时间
    unsigned long last_update; // 最后更新时间
    bool valid;

    IndexProgress() : file_size(0), current_pos(0), pages_generated(0),
                      area_width(0), area_height(0), font_size(0.0f),
                      encoding(TextEncoding::AUTO_DETECT), start_time(0),
                      last_update(0), valid(false) {}
};

// 书签配置结构
struct BookmarkConfig
{
    std::string file_path;
    size_t current_position;
    size_t file_size; // 文件大小，用于检测文件是否被修改
    int16_t area_width;
    int16_t area_height;
    float font_size;
    std::string font_name;  // 字体名称
    uint8_t font_version;   // 字体版本
    uint8_t font_base_size; // 字体文件基础尺寸
    TextEncoding encoding;
    bool valid;

    // 页面索引相关信息
    size_t current_page_index;
    size_t total_pages;
    bool page_completed;
    bool showlabel;    // 是否在锁屏时显示标签信息，默认为true
    bool keepOrg;      // 当为 false 时（默认）渲染不应用全局繁简转换（保持原始书籍组织）
    bool drawBottom;   // 是否在文字下方画底线，默认为true
    bool verticalText; // 是否竖排显示文字，默认为false
    int16_t readhour; // 最后阅读小时（0-23），默认0
    int16_t readmin;  // 最后阅读分钟（0-59），默认0

    BookmarkConfig() : current_position(0), file_size(0), area_width(0), area_height(0),
                       font_size(0.0f), font_version(0), font_base_size(0), encoding(TextEncoding::AUTO_DETECT), valid(false),
                       current_page_index(0), total_pages(0), page_completed(false), showlabel(true), keepOrg(true), drawBottom(true), verticalText(false),
                       readhour(0), readmin(0) {}
};

// 全局配置保存和加载函数
bool saveBookmarkConfig(const BookHandle *book, const char *config_file = "/spiffs/bookmark.cfg");
BookmarkConfig loadBookmarkConfig(const char *config_file = "/spiffs/bookmark.cfg");
bool restoreBookFromConfig(BookHandle *&book, const BookmarkConfig &config);

// 新的自动书签系统函数
bool saveBookmarkForFile(const BookHandle *book);                      // 根据文件名自动保存书签到SD卡
BookmarkConfig loadBookmarkForFile(const std::string &book_file_path); // 根据文件名自动加载书签
bool isFileModified(const std::string &book_file_path);                // 检查文件是否被修改过（基于文件大小）
bool ensureBookmarksFolder();                                          // 确保SD卡上存在bookmarks文件夹
std::string getBookmarkFileName(const std::string &book_file_path);    // 获取书签文件名
std::string getRecordFileName(const std::string &book_file_path);      // 获取阅读记录文件名（.rec）
// Remove index files (page/progress/complete) for a given book path. Public so UI can call it too.
void removeIndexFilesForBookForPath(const std::string &book_file_path);
