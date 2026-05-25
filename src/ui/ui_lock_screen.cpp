#include <string>
#include "ui_lock_screen.h"
#include <M5Unified.h>
#include "../include/readpaper.h"
#include "../text/bin_font_print.h"
#include "../device/ui_display.h"
#include "../text/text_handle.h"
#include "../text/book_handle.h"
#include "ui_canvas_image.h"
#include "../test/per_file_debug.h"
#include <SPIFFS.h>
#include <SD.h>
#include "../SD/SDWrapper.h"
#include "tasks/state_machine_task.h"
#include "config/config_manager.h"
#include "globals.h"
#include <vector>
#include <unordered_set>
#include "device/efficient_file_scanner.h"
#include "ui/toc_display.h"

#include "current_book.h"
#include "tasks/device_interrupt_task.h"
extern M5Canvas *g_canvas;
extern GlobalConfig g_config;

// 全局版本字符串（从/version文件读取）
std::string ver;

namespace
{
    struct LockImageCache
    {
        bool valid = false;
        bool sd_ready = false;
        std::vector<String> candidates;
        std::vector<String> dedicated_images;                 // Images that are dedicated to specific books
        std::unordered_set<std::string> dedicated_image_set;  // Fast lookup for random exclusion
        std::vector<String> book_basenames;                   // Basenames of all books in /book directory
        std::unordered_set<std::string> book_basenames_exact; // Exact stripped basename match
        bool books_scanned = false;
    };

    LockImageCache g_lock_image_cache;

    // Series queue cache: when multiple images belong to a series (same stripped base name),
    // we shuffle them once and cycle through to avoid repeating the same pick.
    static String g_last_series_key;
    static std::vector<String> g_series_queue;
    static int g_series_index = 0;

    inline void reset_lock_image_cache()
    {
        g_lock_image_cache.candidates.clear();
        g_lock_image_cache.dedicated_images.clear();
        g_lock_image_cache.dedicated_image_set.clear();
        g_lock_image_cache.book_basenames.clear();
        g_lock_image_cache.book_basenames_exact.clear();
        g_lock_image_cache.valid = false;
        g_lock_image_cache.books_scanned = false;
    }

    bool ensure_lock_image_candidates(const char *dirPath)
    {
#if DBG_LOCKSCREEN
        unsigned long lck_t0 = millis();
        Serial.printf("[LCK] ensure_lock_image_candidates enter: dir=%s, valid=%d, freeHeap=%u\n",
                      dirPath, (int)g_lock_image_cache.valid, (unsigned)ESP.getFreeHeap());
#endif
        if (g_lock_image_cache.valid)
            return true;

        if (!g_lock_image_cache.sd_ready)
        {
            if (!SDW::SD.begin())
            {
#if DBG_LOCKSCREEN
                Serial.println("[LCK] SD begin FAILED");
#endif
                return false;
            }
            g_lock_image_cache.sd_ready = true;
#if DBG_LOCKSCREEN
            Serial.println("[LCK] SD begin OK");
#endif
        }

        if (!SDW::SD.exists(dirPath))
        {
            g_lock_image_cache.valid = true;
            return true;
        }

        reset_lock_image_cache();

        std::vector<FileInfo> files = EfficientFileScanner::scanDirectory(std::string(dirPath));
        auto make_sd_prefixed = [](const std::string &p) -> String
        {
            if (p.rfind("/sd", 0) == 0)
                return String(p.c_str());
            if (p.rfind("/", 0) == 0)
                return String((std::string("/sd") + p).c_str());
            return String((std::string("/sd/") + p).c_str());
        };

        for (const auto &fi : files)
        {
            if (fi.isDirectory)
                continue;
            std::string lname = fi.name;
            for (auto &c : lname)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');

            const bool looks_like_image =
                lname.size() >= 4 &&
                (lname.find(".png") != std::string::npos || lname.find(".jpg") != std::string::npos ||
                 lname.find(".jpeg") != std::string::npos || lname.find(".bmp") != std::string::npos);

            if (!looks_like_image)
                continue;

            String fullPath = make_sd_prefixed(fi.path);
            g_lock_image_cache.candidates.push_back(fullPath);
        }

        g_lock_image_cache.valid = true;
#if DBG_LOCKSCREEN
        Serial.printf("[LCK] ensure_lock_image_candidates exit: %d candidates, took %lu ms, freeHeap=%u\n",
                      (int)g_lock_image_cache.candidates.size(), millis() - lck_t0, (unsigned)ESP.getFreeHeap());
#endif
        return true;
    }

    inline String extract_basename_no_ext(const String &path)
    {
        String fn = path;
        int ls = fn.lastIndexOf('/');
        if (ls >= 0)
            fn = fn.substring(ls + 1);
        int dot = fn.lastIndexOf('.');
        return (dot >= 0) ? fn.substring(0, dot) : fn;
    }

    inline String extract_filename(const String &path)
    {
        String fn = path;
        int ls = fn.lastIndexOf('/');
        return (ls >= 0) ? fn.substring(ls + 1) : fn;
    }

    // Remove trailing digits and trailing separators (space/_/-) from a filename base
    inline String strip_trailing_digits_and_separators(const String &s)
    {
        String out = s;
        // remove trailing digits
        while (out.length() > 0)
        {
            char c = out.charAt(out.length() - 1);
            if (c >= '0' && c <= '9')
                out.remove(out.length() - 1);
            else
                break;
        }
        // remove trailing separators
        while (out.length() > 0)
        {
            char c = out.charAt(out.length() - 1);
            if (c == ' ' || c == '_' || c == '-')
                out.remove(out.length() - 1);
            else
                break;
        }
        return out;
    }

    // Scan /book directory (recursively) and collect all book basenames (without extension, stripped)
    void scan_book_directory()
    {
#if DBG_LOCKSCREEN
        unsigned long lck_t0 = millis();
        Serial.println("[LCK] scan_book_directory enter");
#endif
        if (g_lock_image_cache.books_scanned)
            return;

        if (!g_lock_image_cache.sd_ready)
        {
            if (!SDW::SD.begin())
            {
                g_lock_image_cache.books_scanned = true;
                return;
            }
            g_lock_image_cache.sd_ready = true;
        }

        const char *bookDir = "/book";
        if (!SDW::SD.exists(bookDir))
        {
            g_lock_image_cache.books_scanned = true;
#if DBG_LOCKSCREEN
            Serial.println("[LCK] scan_book_directory: /book does not exist");
#endif
            return;
        }

        // Iterative DFS avoids recursion overhead and handles deep subdirectories robustly.
        auto is_book_file = [](const std::string &lower_name) -> bool
        {
            auto ends_with = [&](const char *ext) -> bool
            {
                const size_t n = strlen(ext);
                if (lower_name.size() < n)
                    return false;
                return lower_name.compare(lower_name.size() - n, n, ext) == 0;
            };
            return ends_with(".txt") || ends_with(".epub") || ends_with(".pdf");
        };

        std::vector<std::string> dir_stack;
        dir_stack.push_back(std::string(bookDir));

        int walked_entries = 0;
        while (!dir_stack.empty())
        {
            std::string dir_path = dir_stack.back();
            dir_stack.pop_back();

            File dir = SDW::SD.open(dir_path.c_str());
            if (!dir || !dir.isDirectory())
            {
                if (dir)
                    dir.close();
                continue;
            }

            dir.rewindDirectory();
            while (true)
            {
                File entry = dir.openNextFile();
                if (!entry)
                    break;

                const char *name_ptr = entry.name();
                if (!name_ptr || name_ptr[0] == '\0')
                {
                    entry.close();
                    continue;
                }

                std::string entry_path(name_ptr);
                if (entry_path.size() > 0 && entry_path[0] != '/')
                {
                    if (dir_path == "/")
                        entry_path = std::string("/") + entry_path;
                    else
                        entry_path = dir_path + std::string("/") + entry_path;
                }

                const bool is_dir = entry.isDirectory();
                entry.close();

                if (is_dir)
                {
                    dir_stack.push_back(entry_path);
                }
                else
                {
                    std::string lower_name = entry_path;
                    for (char &c : lower_name)
                    {
                        if (c >= 'A' && c <= 'Z')
                            c = static_cast<char>(c - 'A' + 'a');
                    }

                    if (!is_book_file(lower_name))
                        continue;

                    String basename = extract_basename_no_ext(String(entry_path.c_str()));
                    basename.toLowerCase();
                    String stripped = strip_trailing_digits_and_separators(basename);
                    if (stripped.length() > 0)
                    {
                        std::string key = std::string(stripped.c_str());
                        if (g_lock_image_cache.book_basenames_exact.insert(key).second)
                        {
                            g_lock_image_cache.book_basenames.push_back(stripped);
                        }
                    }
                }

                walked_entries++;
                if ((walked_entries % 16) == 0)
                    yield();
            }

            dir.close();
        }

        g_lock_image_cache.books_scanned = true;
#if DBG_LOCKSCREEN
        Serial.printf("[LCK] scan_book_directory exit: %d books found, walked %d entries, took %lu ms\n",
                      (int)g_lock_image_cache.book_basenames.size(), walked_entries, millis() - lck_t0);
#endif
    }

    // Check if an image is dedicated to any book (using same matching logic)
    bool is_image_dedicated_to_any_book(const String &imagePath)
    {
        String img_base = extract_basename_no_ext(imagePath);
        String img_base_lower = img_base;
        img_base_lower.toLowerCase();
        String img_base_stripped = strip_trailing_digits_and_separators(img_base_lower);

        if (img_base_stripped.length() == 0)
            return false;

        if (g_lock_image_cache.book_basenames_exact.find(std::string(img_base_stripped.c_str())) !=
            g_lock_image_cache.book_basenames_exact.end())
        {
            return true;
        }

        // Check against all book basenames
        for (const String &book_base_stripped : g_lock_image_cache.book_basenames)
        {
            // Fuzzy: stripped image base contained in book base or vice versa
            if (book_base_stripped.indexOf(img_base_stripped) >= 0 ||
                img_base_stripped.indexOf(book_base_stripped) >= 0)
            {
                return true;
            }
        }

        return false;
    }

    // Build the list of dedicated images
    void identify_dedicated_images()
    {
#if DBG_LOCKSCREEN
        unsigned long lck_t0 = millis();
        Serial.println("[LCK] identify_dedicated_images enter");
#endif
        if (g_lock_image_cache.dedicated_images.size() > 0)
            return; // Already identified

        scan_book_directory();

        for (const String &imgPath : g_lock_image_cache.candidates)
        {
            if (is_image_dedicated_to_any_book(imgPath))
            {
                g_lock_image_cache.dedicated_images.push_back(imgPath);
                g_lock_image_cache.dedicated_image_set.insert(std::string(imgPath.c_str()));
            }
        }

#if DBG_LOCKSCREEN
        Serial.printf("[LCK] identify_dedicated_images exit: %d dedicated out of %d, took %lu ms\n",
                      (int)g_lock_image_cache.dedicated_images.size(),
                      (int)g_lock_image_cache.candidates.size(),
                      millis() - lck_t0);
#endif
    }
}

// 前向声明
static String pick_book_cover_image(const char *dirPath);

// 打印书籍信息到 canvas（书名/章节/页码/阅读时长/天数/电量），内部调用 bin_font_flush_canvas
static void print_book_info_on_canvas(bool isshutdown)
{
    if (g_current_book == nullptr)
        return;

    // ---- 书名（去路径、去扩展名） ----
    std::string book_name = g_current_book->getBookName();
    {
        size_t dot = book_name.find_last_of('.');
        if (dot != std::string::npos)
            book_name = book_name.substr(0, dot);
    }

    // ---- 章节名（从 TOC idx 查找，与 drawBottomUI 一致） ----
    std::string chapter;
    if (g_current_book->isIndexed())
    {
        size_t cur_page_index = g_current_book->getCurrentPageIndex();
        size_t page_start = g_current_book->getPageStart(cur_page_index);
        if (page_start == (size_t)-1)
            page_start = g_current_book->position();

        size_t toc_idx = 0;
        int toc_page = -1, toc_row = -1;
        bool on_current = false;

        if (find_toc_entry_for_position(g_current_book->filePath(), page_start,
                                        toc_idx, toc_page, toc_row, on_current))
        {
            get_toc_title_for_index(g_current_book->filePath(), toc_idx, chapter);
        }
    }

    // ---- 页数 ----
    size_t cur_page = 1, total_page = 1;
    cur_page = g_current_book->getCurrentPageIndex() + 1;
    total_page = g_current_book->getTotalPages();

    // ---- 阅读时间 ----
    int16_t read_hour = g_current_book->getReadHour();
    int16_t read_min = g_current_book->getReadMin();

    // ---- 阅读天数跨度（解析 .rec 文件，统计不同 YYYYMMDD 数量） ----
    int day_span = 1;
    {
        std::string rec_path = getRecordFileName(g_current_book->filePath());
        std::unordered_set<std::string> unique_days;
        if (SDW::SD.exists(rec_path.c_str()))
        {
            File rf = SDW::SD.open(rec_path.c_str(), "r");
            if (rf)
            {
                if (rf.available())
                    rf.readStringUntil('\n');
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
                        std::string ts_str = ts.c_str();
                        if (ts_str.length() >= 8)
                        {
                            unique_days.insert(ts_str.substr(0, 8));
                        }
                    }
                }
                rf.close();
            }
        }
        day_span = (int)unique_days.size();
        if (day_span < 1)
            day_span = 1;
    }

    // ---- 电量 ----
    int battery = DeviceInterruptTask::getLastBatteryPercentage();

    // 绘制电池电量标识
    {
        int batteryBars = (battery + 19) / 20; // 每20%一格
        if (batteryBars > 5)
            batteryBars = 5;
        if (batteryBars < 1)
            batteryBars = 1;
        for (int i = 0; i < batteryBars; i++)
        {
            g_canvas->fillRect(16 + i * 9, 820, 6, 16, TFT_DARKGRAY);
        }
    }

    // ---- lock / poweroff 标识 ----
    const char *lock_mode = isshutdown ? "poweroff" : "lock";

    // ---- 组装显示字符串并打印 ----
    const int pos_x = 0;
    const int pos_y = 480;
    const uint8_t fsize = 24;
    const int line_h = 32;

    char buf[128];

    // Title
    snprintf(buf, sizeof(buf), "%s", book_name.c_str());
    bin_font_print(buf, 32, TFT_BLACK, 540, pos_x-6, pos_y, false, g_canvas, TEXT_ALIGN_CENTER_NEW, 400);
    // Chapter
    snprintf(buf, sizeof(buf), "%s", chapter.c_str());
    bin_font_print(buf, fsize, TFT_BLACK, 540, pos_x-6, pos_y + line_h + 12, false, g_canvas, TEXT_ALIGN_CENTER_NEW, 400);
    // Pages
    // snprintf(buf, sizeof(buf), "%zu/%zu", cur_page, total_page);
    snprintf(buf, sizeof(buf), "%zu", cur_page);
    bin_font_print(buf, 22, TFT_BLACK, 180, pos_x + 330, pos_y + line_h * 4, false, g_canvas, TEXT_ALIGN_CENTER, 180);
    // Time
    snprintf(buf, sizeof(buf), "%dh%dm", read_hour, read_min);
    bin_font_print(buf, 22, TFT_BLACK, 120, pos_x + 196, pos_y + line_h * 4, false, g_canvas, TEXT_ALIGN_CENTER, 120);
    // Days
    snprintf(buf, sizeof(buf), "%d天", day_span);
    bin_font_print(buf, 22, TFT_BLACK, 120, pos_x + 32, pos_y + line_h * 4, false, g_canvas, TEXT_ALIGN_CENTER, 120);

    // ---- 当前日期 YYYY/MM/DD ----
    {
        struct tm timeinfo;
        getLocalTime(&timeinfo, 0);
        snprintf(buf, sizeof(buf), "%04d/%02d/%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
        bin_font_print(buf, 18, TFT_BLACK, 160, pos_x + 370, pos_y + line_h * 12, false, g_canvas, TEXT_ALIGN_CENTER, 160);
    }

    // ---- 当前阅读百分比 --
    // Protect against divide-by-zero and clamp progress width to [0,540]
    int progress_width = 0;
    if (total_page > 0)
    {
        // use 64-bit intermediate just in case, then clamp
        int64_t pw = (int64_t)290 * (int64_t)cur_page / (int64_t)total_page;
        if (pw < 0)
            pw = 0;
        if (pw > 540)
            pw = 540;
        progress_width = (int)pw;
    }

    // snprintf(buf, sizeof(buf), "%zu/%zu", cur_page, total_page);
    // g_canvas->fillRoundRect(pos_x + 120, pos_y + line_h * 5 + 8, progress_width,36, 2, TFT_LIGHTGREY);
    g_canvas->fillRoundRect(pos_x + 124, pos_y + line_h * 5 + 9, progress_width, 36, 8, TFT_LIGHTGREY);

    bin_font_flush_canvas(false, false, true, RECT);
}

// 推送封面缩略图（1/3 尺寸 或更大），返回是否成功推送
static bool push_cover_thumbnail_scaled()
{
    String cover = pick_book_cover_image("/image");
    if (cover.length() > 0)
    {
        const float scale_factor = 0.4f;
        ui_push_image_to_canvas(cover.c_str(), 0, 0);
        ui_push_image_to_canvas_scaled(cover.c_str(), 540 / 2 - 108, 160, scale_factor, scale_factor, nullptr, false);
        return true;
    }
    return false;
}

// 尝试推送 exlibris.png 作为锁屏背景图片（不含任何追加信息或缩略图）
// 优先级：1) SD 根目录 /sd/exlibris.png  2) SD /image/ 目录下的 default.png  3) SPIFFS /spiffs/default.png
static bool try_push_default_lock_image(int x, int y, bool isshutdown)
{
#if DBG_LOCKSCREEN
    unsigned long lck_t0 = millis();
    Serial.println("[LCK] try_push_default_lock_image enter: attempting /sd/exlibris.png");
#endif
    // 第一步：尝试 SD 卡根目录 /sd/exlibris.png
    if (SDW::SD.exists("/exlibris.png"))
    {
#if DBG_LOCKSCREEN
        Serial.println("[LCK] try_push_default_lock_image: found /sd/exlibris.png, pushing...");
#endif
        ui_push_image_to_canvas("/sd/exlibris.png", x, y, nullptr, false);
#if DBG_LOCKSCREEN
        Serial.printf("[LCK] try_push_default_lock_image: /sd/exlibris.png done, took %lu ms\n", millis() - lck_t0);
#endif
        return true;
    }

#if DBG_LOCKSCREEN
    Serial.println("[LCK] try_push_default_lock_image: /sd/exlibris.png not found, trying /image/...");
#endif
    // 第二步：fallback 到 SD /image/ 目录下找 exlibris.png
    if (ensure_lock_image_candidates("/image"))
    {
        const auto &candidates = g_lock_image_cache.candidates;
        for (const String &p : candidates)
        {
            String file_with_ext = extract_filename(p);
            file_with_ext.toLowerCase();
            if (file_with_ext == "exlibris.png")
            {
                ui_push_image_to_canvas(p.c_str(), x, y, nullptr, false);
#if DBG_LOCKSCREEN
                Serial.printf("[LCK] try_push_default_lock_image: /image/exlibris.png done, took %lu ms\n", millis() - lck_t0);
#endif
                return true;
            }
        }
    }

#if DBG_LOCKSCREEN
    Serial.println("[LCK] try_push_default_lock_image: no default in /image/, trying SPIFFS...");
#endif

    // 第三步：fallback 到 SPIFFS /spiffs/exlibris.png
    if (SPIFFS.exists("/exlibris.png"))
    {
#if DBG_LOCKSCREEN
        Serial.println("[LCK] try_push_default_lock_image: found /spiffs/exlibris.png");
#endif
        ui_push_image_to_canvas("/spiffs/exlibris.png", x, y, nullptr, false);
#if DBG_LOCKSCREEN
        Serial.printf("[LCK] try_push_default_lock_image: /spiffs/exlibris.png done, took %lu ms\n", millis() - lck_t0);
#endif
        return true;
    }

#if DBG_LOCKSCREEN
    Serial.println("[LCK] try_push_default_lock_image: all fallbacks FAILED -> returns false");
#endif
    return false;
}

// 挑选一张与当前书籍匹配的封面图
// 返回图片完整路径；如果没有可用图片则返回空字符串
// 逻辑流程：
//   1. 先尝试匹配当前书籍的专属封面
//   2. 若未命中，查找 image 目录下是否存在 default.png，有则返回
//   3. 否则进入随机挑选（随机池中排除专属图片和 exlibris.png）
static String pick_book_cover_image(const char *dirPath)
{
#if DBG_LOCKSCREEN
    unsigned long lck_t0 = millis();
    Serial.printf("[LCK] pick_book_cover_image enter: dir=%s, mode=%s, freeHeap=%u\n",
                  dirPath, g_config.lockscreen_mode, (unsigned)ESP.getFreeHeap());
#endif
    if (!ensure_lock_image_candidates(dirPath))
    {
#if DBG_LOCKSCREEN
        Serial.println("[LCK] pick_book_cover_image: ensure_lock_image_candidates failed -> empty");
#endif
        return String();
    }

    const auto &candidates = g_lock_image_cache.candidates;
    if (candidates.empty())
    {
#if DBG_LOCKSCREEN
        Serial.println("[LCK] pick_book_cover_image: candidates empty");
#endif
        return String();
    }

    // 获取当前书的 basename
    String book_base;
    if (g_current_book)
    {
        std::string bp = getBookFilePath(g_current_book);
        if (!bp.empty())
        {
            String full(bp.c_str());
            int ls = full.lastIndexOf('/');
            String fname = (ls >= 0) ? full.substring(ls + 1) : full;
            int dot = fname.lastIndexOf('.');
            book_base = (dot >= 0) ? fname.substring(0, dot) : fname;
        }
    }

    // 如果有当前书籍，尝试匹配专属封面
    if (book_base.length() > 0)
    {
        String book_base_lower = book_base;
        book_base_lower.toLowerCase();
        String book_base_stripped = strip_trailing_digits_and_separators(book_base_lower);

        std::vector<String> matched;
        for (const String &p : candidates)
        {
            String img_base = extract_basename_no_ext(p);
            String img_base_lower = img_base;
            img_base_lower.toLowerCase();
            String img_base_stripped = strip_trailing_digits_and_separators(img_base_lower);

            // Exact stripped-name match
            if (img_base_stripped.length() > 0 && img_base_stripped == book_base_stripped)
            {
                matched.push_back(p);
                continue;
            }

            // Fuzzy: stripped image base contained in book base or vice versa
            if (img_base_stripped.length() > 0 &&
                (book_base_lower.indexOf(img_base_stripped) >= 0 ||
                 img_base_lower.indexOf(book_base_stripped) >= 0))
            {
                matched.push_back(p);
                continue;
            }
        }

        if (!matched.empty())
        {
            String series_key = book_base_stripped;

#if DBG_LOCKSCREEN
            Serial.printf("[LCK] pick_book_cover_image: book_base=%s, matched=%d, series_key=%s\n",
                          book_base_lower.c_str(), (int)matched.size(), series_key.c_str());
#endif

            // Same series as last time → rotate queue
            if (g_series_queue.size() > 0 && series_key == g_last_series_key)
            {
                if (g_series_index >= (int)g_series_queue.size())
                    g_series_index = 0;
                String ret = g_series_queue[g_series_index++];
#if DBG_LOCKSCREEN
                Serial.printf("[LCK] pick_book_cover_image: rotate queue -> %s\n", ret.c_str());
#endif
                return ret;
            }

            // Build new shuffled queue
            g_series_queue.clear();
            for (const String &p : matched)
                g_series_queue.push_back(p);

            randomSeed(millis());
            for (int i = (int)g_series_queue.size() - 1; i > 0; --i)
            {
                int j = random(i + 1);
                if (j != i)
                    std::swap(g_series_queue[i], g_series_queue[j]);
            }

            g_last_series_key = series_key;
            g_series_index = 0;

            if (g_series_queue.size() > 0)
            {
                String ret = g_series_queue[g_series_index++];
#if DBG_LOCKSCREEN
                Serial.printf("[LCK] pick_book_cover_image: new series -> %s\n", ret.c_str());
#endif
                return ret;
            }
        }
    }

    // 专属封面未命中 → 先查找 image/default.png，找到则直接返回
    {
        String default_png;
        for (const String &p : candidates)
        {
            String fname = extract_filename(p);
            fname.toLowerCase();
            if (fname == "default.png")
            {
                default_png = p;
                break;
            }
        }
        if (default_png.length() > 0)
        {
#if DBG_LOCKSCREEN
            Serial.printf("[LCK] pick_book_cover_image: found default.png -> %s\n", default_png.c_str());
#endif
            return default_png;
        }
    }

    // 如果不是 random 模式，不随机选图
    // 注释掉，由于已经修改过了，没有以前的'default'设置，始终随机选图
    //    if (strcmp(g_config.lockscreen_mode, "random") != 0)
    //        return String();

#if DBG_LOCKSCREEN
    Serial.printf("[LCK] pick_book_cover_image: random mode, freeHeap=%u, candidates=%d\n",
                  (unsigned)ESP.getFreeHeap(), (int)candidates.size());
#endif

    // 识别专属图片并从随机池中排除
    identify_dedicated_images();

    std::vector<String> available_for_random;
    for (const String &imgPath : candidates)
    {
        // 排除专属封面图片
        if (g_lock_image_cache.dedicated_image_set.find(std::string(imgPath.c_str())) !=
            g_lock_image_cache.dedicated_image_set.end())
        {
            continue;
        }
        // 排除 exlibris.png（默认锁屏图，不应出现在随机池中）
        {
            String fname = extract_filename(imgPath);
            fname.toLowerCase();
            if (fname == "exlibris.png")
            {
                continue;
            }
        }
        available_for_random.push_back(imgPath);
    }

#if DBG_LOCKSCREEN
    Serial.printf("[LCK] pick_book_cover_image: random pool=%d, dedicated=%d\n",
                  (int)available_for_random.size(), (int)g_lock_image_cache.dedicated_images.size());
#endif

    if (available_for_random.empty())
    {
#if DBG_LOCKSCREEN
        Serial.println("[LCK] pick_book_cover_image: random pool empty, using ALL candidates");
#endif
        available_for_random = candidates;
    }

    randomSeed(millis());
    int idx = random((int)available_for_random.size());
    String ret = available_for_random[idx];
#if DBG_LOCKSCREEN
    Serial.printf("[LCK] pick_book_cover_image exit: picked=%s, idx=%d/%d, took %lu ms\n",
                  ret.c_str(), idx, (int)available_for_random.size(), millis() - lck_t0);
#endif
    return ret;
}

// 挑选一张随机图片并全屏推送，**不**回退到 exlibris.png
// 返回 true 表示成功推送了一张随机图片，false 表示没有可用的随机图片
static bool push_random_sd_image_if_available(const char *dirPath, int x, int y, bool isshutdown)
{
#if DBG_LOCKSCREEN
    unsigned long lck_t0 = millis();
    Serial.printf("[LCK] push_random_sd_image_if_available enter: dir=%s, freeHeap=%u\n",
                  dirPath, (unsigned)ESP.getFreeHeap());
#endif
    String pick = pick_book_cover_image(dirPath);
    if (pick.length() > 0)
    {
#if DBG_LOCKSCREEN
        Serial.printf("[LCK] push_random_sd_image_if_available: picked=%s, took %lu ms\n",
                      pick.c_str(), millis() - lck_t0);
#endif
        ui_push_image_to_canvas(pick.c_str(), x, y, nullptr, true);
        return true;
    }

#if DBG_LOCKSCREEN
    Serial.println("[LCK] push_random_sd_image_if_available: no pick, returning false (no fallback)");
#endif
    return false;
}

// 绘制书名与页码的腰封（Name banner）
static void draw_name_banner(M5Canvas *canvas, const char *name_with_page, int32_t basey, bool invert = false, int curp = 0, int totalp = 0, bool forsnapshot = false)
{
    // Bias
    basey = basey - 3;

    canvas->fillRect(0, basey + 2, 540, 56, invert ? TFT_BLACK : TFT_WHITE);
    canvas->fillRect(0, basey + 5, 540, 50, invert ? TFT_BLACK : TFT_WHITE);

    // Protect against divide-by-zero and clamp progress width to [0,540]
    int progress_width = 0;
    if (totalp > 0)
    {
        // use 64-bit intermediate just in case, then clamp
        int64_t pw = (int64_t)540 * (int64_t)curp / (int64_t)totalp;
        if (pw < 0)
            pw = 0;
        if (pw > 540)
            pw = 540;
        progress_width = (int)pw;
    }

    // canvas->fillRect(0, basey + 8, PAPER_S3_WIDTH, 44, 0xE75C);
    // canvas->fillRect(0, basey + 12, progress_width, 36, 0xA554);
    canvas->fillRect(0, basey + 8, PAPER_S3_WIDTH, 44, 0xF7DE);
    if (!forsnapshot)
        canvas->fillRect(0, basey + 13, progress_width, 36, 0xB5D6);

    canvas->drawWideLine(0, basey + 10, 545, basey + 10, 1.1f, invert ? TFT_WHITE : TFT_BLACK);
    canvas->drawWideLine(0, basey + 50, 545, basey + 50, 1.1f, invert ? TFT_WHITE : TFT_BLACK);

    // 绘制书名
    bin_font_print(name_with_page, 24, TFT_BLACK, LOCKBOOKNAMEWIDTH, (PAPER_S3_WIDTH - LOCKBOOKNAMEWIDTH) / 2 - 24, basey + 20 - 1, false, canvas, TEXT_ALIGN_CENTER, LOCKBOOKNAMEWIDTH, g_current_book ? g_current_book->getKeepOrg() : false);

    // 尝试获取并显示当前章节名（如果存在TOC）
    if (g_current_book && g_current_book->isOpen())
    {
        TextPageResult tp = g_current_book->currentPage();
        if (tp.success)
        {
            size_t entry_index;
            int page, row;
            bool on_current;
            if (find_toc_entry_for_position(g_current_book->filePath(), tp.file_pos,
                                            entry_index, page, row, on_current))
            {
                std::string toc_title;
                if (get_toc_title_for_index(g_current_book->filePath(), entry_index, toc_title))
                {
                    // 在书名下方绘制章节名（使用较小字号）
                    // canvas->fillRect(0, basey + 52, PAPER_S3_WIDTH, 40, 0xF7DE);
                    canvas->fillRect(0, basey + 52, PAPER_S3_WIDTH, 40, TFT_BLACK);
                    bin_font_print(toc_title.c_str(), 24, 0, LOCKBOOKNAMEWIDTH, (PAPER_S3_WIDTH - LOCKBOOKNAMEWIDTH) / 2 - 24, basey + 60, false, canvas, TEXT_ALIGN_CENTER, LOCKBOOKNAMEWIDTH, false, false, false, true);
                    canvas->drawWideLine(0, basey + 90, 540, basey + 90, 1.1, invert ? TFT_WHITE : TFT_BLACK);
                    //                    canvas->drawWideLine(30, basey + 65, 30, basey + 75, 2.0, invert ? TFT_WHITE : TFT_BLACK);
                    //                   canvas->drawWideLine(510, basey + 65, 510, basey + 75, 2.0, invert ? TFT_WHITE : TFT_BLACK);
                }
            }
        }
    }

    drawScrew(g_canvas, 20, basey + 30);
    drawScrew(g_canvas, 520, basey + 30);
    canvas->drawLine(0, basey + 30, 55, basey + 30, TFT_BLACK);
    canvas->drawLine(485, basey + 30, 540, basey + 30, TFT_BLACK);

    //    drawScrew(g_canvas, 20, basey + 70);
    //    drawScrew(g_canvas, 520, basey + 70);
}

// 绘制左侧垂直条及竖排文摘（vertical banner）
static void draw_vertical_banner(M5Canvas *canvas, const std::string &digest, int basex = 0, int basew = 160, int offsetx = 402, int bg = TFT_BLACK, int fg = TFT_LIGHTGREY, uint8_t digest_font_size = 28)
{
    // Draw a black vertical strip and white separator lines

    // Base
    canvas->fillRect(basex, 0, basew, 940, bg);
    // Side

    canvas->drawLine(basex + 5, 80, basex + 5, 935, fg);
    canvas->drawLine(basew + basex - 5, 80, basew + basex - 5, 935, fg);
    canvas->drawLine(basex + 5, 935, basex + basew - 5, 935, fg);

    // canvas->fillCircle(basex + basew / 2, 820, 15, TFT_WHITE);
    // canvas->drawWideLine(basex, 820, basex+basew, 820, 2.0, fg);
    // canvas->drawCircle(basex + basew / 2, 910, 20, fg);

    // Render digest vertically: area_width=900 (large value), vertical=true (last param true)
    // bin_font_print(digest.c_str(), 28, fg, 900, 120, 960 - basew - offsetx, false, canvas, TEXT_ALIGN_LEFT, 900, false, g_current_book->getKeepOrg(), true);
    bin_font_print(digest.c_str(), digest_font_size, fg, 900, 86, 960 - basew - offsetx, false, canvas, TEXT_ALIGN_LEFT, 900, true, true, true);

    // canvas->fillRect(basex, 760, basew, 100, TFT_BLACK);
    canvas->fillRect(basex, 0, basew, 80, TFT_BLACK);
    // RING
    canvas->fillCircle(basex + basew / 2, 40, 15, TFT_WHITE);
    canvas->drawWideLine(basex, 40, basex + basew, 40, 2.0, TFT_WHITE);
    canvas->drawLine(basex, 5, basex + basew, 5, TFT_WHITE);
    canvas->drawCircle(basex + basew / 2, 40, 20, TFT_WHITE);
}

static uint8_t resolve_lockscreen_digest_font_size(float lockscreen_font_size)
{
    // Keep original visual proportion: digest was 28 when lockscreen base was 30.
    const float digest_ratio = 28.0f / 30.0f;

    float effective_font_size = get_configured_reading_font_size(get_font_size_from_file());
    if (effective_font_size <= 0.0f)
    {
        effective_font_size = (lockscreen_font_size > 0.0f) ? lockscreen_font_size : 30.0f;
    }

    int digest_size = (int)(effective_font_size * digest_ratio + 0.5f);
    if (digest_size < 12)
        digest_size = 12;
    if (digest_size > 72)
        digest_size = 72;
    return (uint8_t)digest_size;
}

void show_start_screen(const char *subtitle)
{
    // Skip
    // 使用 canvas 方式推送图片 - fast
    //    ui_push_image_to_canvas("/spiffs/screen.png", 0, 0);
    //    bin_font_flush_canvas(false,false);

    bin_font_clear_canvas();
    // ui_push_image_to_canvas("/spiffs/start.png", 110, 390);
    ui_push_image_to_canvas("/spiffs/start.png", 0, 0);
    // If subtitle provided, draw centered below image using native drawString
    // (at this early stage fonts may not be loaded, so avoid bin_font_print)
    if (subtitle && subtitle[0] != '\0')
    {
        bin_font_print(subtitle, 24, 0, 540, 100, 560, false, g_canvas, TEXT_ALIGN_LEFT);
    }

    // 在屏幕底部显示 /version 文件第三行（如果存在），使用 TextSize(2)
    if (SPIFFS.exists("/version"))
    {
        File vf = SPIFFS.open("/version", "r");
        if (vf)
        {
            std::string curLine;
            std::string lastNonEmpty;
            int lineNo = 0;
            while (vf.available())
            {
                char c = vf.read();
                if (c == '\r')
                    continue;
                if (c == '\n')
                {
                    lineNo++;
                    if (lineNo == 3)
                    {
                        ver = curLine;
                        break;
                    }
                    if (!curLine.empty())
                        lastNonEmpty = curLine;
                    curLine.clear();
                }
                else
                {
                    curLine.push_back(c);
                }
            }

            // handle case where file ends without trailing newline
            if (ver.empty())
            {
                if (!curLine.empty())
                {
                    lineNo++;
                    if (lineNo == 3)
                        ver = curLine;
                    if (ver.empty() && !lastNonEmpty.empty())
                        ver = lastNonEmpty;
                }
                else if (!lastNonEmpty.empty())
                {
                    ver = lastNonEmpty;
                }
            }

            vf.close();

            // trim whitespace
            if (!ver.empty())
            {
                size_t st = 0;
                while (st < ver.size() && isspace((unsigned char)ver[st]))
                    st++;
                size_t ed = ver.size();
                while (ed > st && isspace((unsigned char)ver[ed - 1]))
                    ed--;
                if (ed > st)
                    ver = ver.substr(st, ed - st);
                else
                    ver.clear();
            }
        }
    }

    if (!ver.empty())
    {
        bin_font_print(ver.c_str(), 24, 0, 540, 10, 920, false, g_canvas, TEXT_ALIGN_LEFT);
    }
    bin_font_flush_canvas(false, false, true, RECT);
    delay(500);
    M5.Display.waitDisplay();
}

void show_lockscreen(int16_t area_width, int16_t area_height, float font_size, const char *text, bool isshutdown, const char *labelpos, bool forsnapshot)
{
#if DBG_LOCKSCREEN
    Serial.printf("[LCK] show_lockscreen enter: shutdown=%d, forsnapshot=%d, mode=%s, text=%s, labelpos=%s, freeHeap=%u\n",
                  (int)isshutdown, (int)forsnapshot,
                  g_config.lockscreen_mode ? g_config.lockscreen_mode : "null",
                  text ? text : "null",
                  labelpos ? labelpos : "null",
                  (unsigned)ESP.getFreeHeap());
#endif
#if DBG_POWERMGT
    Serial.println("[POWER] 10分钟无操作，自动关机");
#endif

    // Lazy overrideing from global now..
    // Override labelpos from global config when configured
    // Use a local copy to ensure lifetime if you prefer safety
    static std::string labelpos_copy; // or non-static if you prefer per-call storage
    if (g_config.labelposition != nullptr && g_config.labelposition[0] != '\0')
    {
        labelpos_copy = g_config.labelposition;
        labelpos = labelpos_copy.c_str();
    }

    if (!forsnapshot)
    {
        if (getCurrentSystemState() != STATE_IDLE)
        {
            ui_push_image_to_display_direct("/spiffs/wait.png", 240, 450);
            M5.Display.waitDisplay();
        }

        const bool is_default_mode = (strcmp(g_config.lockscreen_mode, "default") == 0);

        if (is_default_mode)
        {
            // 如果是关机时刻，则推送start.png先
            //if (isshutdown)
            if (false)
            {
                ui_push_image_to_canvas("/spiffs/start.png", 0, 0);
            }
            push_cover_thumbnail_scaled();
            // [LCK] default 模式：先画封面缩略图(背景层) → 全屏 exlibris.png(透明开窗露出封面) → 书籍信息
            if (try_push_default_lock_image(0, 0, isshutdown))
            {
                if (isshutdown)
                    ui_push_image_to_canvas("/spiffs/power-icon.png", 30, 150);
                else
                    ui_push_image_to_canvas("/spiffs/lock-icon.png", 30, 150);
                print_book_info_on_canvas(isshutdown);
                return;
            }
            // exlibris.png 不存在则 fallthrough 到 random（继续展示图片而非白屏）
        }

        // [LCK] random 模式 / default 回退：挑一张随机图片全屏推送
        if (!push_random_sd_image_if_available("/image", 0, 0, isshutdown))
        {
            if (g_current_book && g_current_book->getVerticalText())
                ui_push_image_to_canvas("/spiffs/screen.png", 0, 0, nullptr, true);
            else
                ui_push_image_to_canvas("/spiffs/screenH.png", 0, 0, nullptr, true);
        }
        // 继续 label/info/边角绘制（下方现有逻辑）
    }

    g_canvas->drawRect(0, 0, 540, 960, TFT_WHITE);
    //    delay(100);
    // Use provided text (or default) instead of reading shutdown.txt
    if (text == nullptr)
    {
        // In failulre , even can't write font
        //     ui_push_image_to_canvas("/spiffs/wait.png", 240, 450);
    }
    else
    {
        // 如果当前书籍的showlabel为false，只显示screen.png，跳过其他内容
        if (g_current_book == nullptr || !g_current_book->getShowLabel())
        {
            if (forsnapshot)
            { // Do nothing
                return;
            }
            // put on top-right conner
            g_canvas->fillTriangle(480, 0, 540, 0, 540, 60, 0x0005);
            g_canvas->drawWideLine(480, 0, 540, 60, 0.5, TFT_WHITE);

            if (isshutdown)
            {
                ui_push_image_to_canvas("/spiffs/power-icon.png", 508, 0);
            }
            else
            {
                ui_push_image_to_canvas("/spiffs/lock-icon.png", 508, 0);
            }

            bin_font_flush_canvas(false, false, true, RECT);
            M5.Display.waitDisplay();

            return; // 提前返回，跳过后续的图片和文字
        }

        //        std::string display_text = text ? std::string(text) : std::string("坐看云起时");
        //       bin_font_print(display_text.c_str(), 30, 2, 540, 60, 900, false); // 1.0f * 30 = 30
        // 腰封 - 只有在showlabel为true时才显示完整内容
        {
            const uint8_t digest_font_size = resolve_lockscreen_digest_font_size(font_size);

            // 主题颜色变量（集中定义，便于主题替换）
            // 三角形/装饰色
            uint16_t theme_tri_color = 0x0005;
            // 垂直条（vertical banner）背景/前景
            uint16_t theme_vertical_bg = 0x2222;
            uint16_t theme_vertical_fg = TFT_WHITE;
            // 腰封/条带背景与文字颜色
            uint16_t theme_strip_bg = 0x2222;
            uint16_t theme_strip_fg = TFT_WHITE;

            std::string curtheme = g_config.marktheme; // 书签主题: 'dark' / 'light' / 'random'
            bool darktheme;
            if (curtheme == "dark")
            {
                darktheme = true;
            }
            else if (curtheme == "light")
            {
                darktheme = false;
            }
            else if (curtheme == "random")
            {
                randomSeed(millis());
                darktheme = (random(2) == 0);
            }
            else
            {
                // Default to light if unknown
                darktheme = false;
            }

            if (darktheme)
            {
                // DARK
                //  三角形/装饰色
                theme_tri_color = 0x0005;
                // 垂直条（vertical banner）背景/前景
                theme_vertical_bg = 0x2222;
                theme_vertical_fg = TFT_WHITE;
                // 腰封/条带背景与文字颜色
                theme_strip_bg = 0x2222;
                theme_strip_fg = TFT_WHITE;
            }
            else
            {
                //  三角形/装饰色
                theme_tri_color = 0x00aa;
                // 垂直条（vertical banner）背景/前景
                theme_vertical_bg = TFT_WHITE;
                theme_vertical_fg = TFT_BLACK;
                // 腰封/条带背景与文字颜色
                theme_strip_bg = TFT_WHITE;
                theme_strip_fg = TFT_BLACK;
            }

            // 根据 labelpos 调整整体腰封 Y 偏移
            int deltaY = 0; // middle 默认
            if (labelpos != nullptr)
            {
                // For horizon
                if (strcmp(labelpos, "top") == 0)
                {
                    deltaY = -450;
                }
                else if (strcmp(labelpos, "default") == 0)
                {
                    deltaY = 220;
                }
                else if (strcmp(labelpos, "middle") == 0)
                {
                    deltaY = 0;
                }

                if (!forsnapshot)
                { // 截屏不需要 triangle , 腰封
                    if (g_current_book && g_current_book->getVerticalText() && strcmp(labelpos, "default") == 0)
                    {
                        // put on bottom-right conner
                        g_canvas->fillTriangle(0, 0, 60, 0, 0, 60, theme_tri_color);
                        g_canvas->drawWideLine(60, 0, 0, 60, 0.5, TFT_WHITE);
                        if (isshutdown)
                        {
                            ui_push_image_to_canvas("/spiffs/power-icon.png", 1, 4);
                        }
                        else
                        {
                            ui_push_image_to_canvas("/spiffs/lock-icon.png", 1, 4);
                        }
                    }
                    else if (strcmp(labelpos, "top") == 0 && (g_current_book && !g_current_book->getVerticalText()))
                    {
                        // put on bottom-right conner
                        g_canvas->fillTriangle(480, 960, 540, 960, 540, 900, theme_tri_color);
                        g_canvas->drawWideLine(480, 960, 540, 900, 0.5, TFT_WHITE);

                        if (isshutdown)
                        {
                            ui_push_image_to_canvas("/spiffs/power-icon.png", 508, 960 - 35);
                        }
                        else
                        {
                            ui_push_image_to_canvas("/spiffs/lock-icon.png", 508, 960 - 35);
                        }
                    }
                    else
                    {
                        // put on top-right conner
                        g_canvas->fillTriangle(480, 0, 540, 0, 540, 60, theme_tri_color);
                        g_canvas->drawWideLine(480, 0, 540, 60, 0.5, TFT_WHITE);

                        if (isshutdown)
                        {
                            ui_push_image_to_canvas("/spiffs/power-icon.png", 508, 0);
                        }
                        else
                        {
                            ui_push_image_to_canvas("/spiffs/lock-icon.png", 508, 0);
                        }
                    }
                }
            }
            // 当前书名
            std::string path = getBookFilePath(g_current_book);
            // 去掉路径，只保留文件名
            size_t pos = path.find_last_of("/\\");
            std::string name = (pos == std::string::npos) ? path : path.substr(pos + 1);
            // 去掉扩展名
            size_t dot = name.find_last_of('.');
            if (dot != std::string::npos)
                name = name.substr(0, dot);
            // 拼接页码信息
            size_t cur_page = 1, total_page = 1;
            if (g_current_book)
            {
                cur_page = g_current_book->getCurrentPageIndex() + 1;
                total_page = g_current_book->getTotalPages();
            }
            char name_with_page[128];
            {
                std::string shortname = name;
                // UTF-8 safe truncate by Unicode codepoints (won't cut a multi-byte char)
                auto utf8_truncate = [](const std::string &s, size_t max_chars) -> std::string
                {
                    std::string out;
                    out.reserve(s.size());
                    const unsigned char *data = (const unsigned char *)s.data();
                    size_t i = 0, n = s.size(), chars = 0;
                    while (i < n && chars < max_chars)
                    {
                        unsigned char c = data[i];
                        size_t clen;
                        if (c < 0x80)
                            clen = 1;
                        else if ((c & 0xE0) == 0xC0)
                            clen = 2;
                        else if ((c & 0xF0) == 0xE0)
                            clen = 3;
                        else if ((c & 0xF8) == 0xF0)
                            clen = 4;
                        else
                            break; // invalid start byte -> stop
                        if (i + clen > n)
                            break; // truncated sequence at end -> stop
                        out.append((const char *)&data[i], clen);
                        i += clen;
                        ++chars;
                    }
                    return out;
                };

                // Limit to 18 characters (Unicode-aware)
                std::string display_name = utf8_truncate(shortname, 22);
                // snprintf(name_with_page, sizeof(name_with_page), "%s | %zu/%zu", display_name.c_str(), cur_page, total_page);
                snprintf(name_with_page, sizeof(name_with_page), "%s", display_name.c_str());
            }

            // If vertical text mode, render a left-side vertical label area instead of the frame image
            if (g_current_book && g_current_book->getVerticalText())
            {
                // Draw vertical banner (left strip and vertical digest)
                if (!forsnapshot)
                {
                    int vb_basex = 200; // default
                    if (labelpos)
                    {
                        if (strcmp(labelpos, "top") == 0)
                            vb_basex = 10;
                        else if (strcmp(labelpos, "middle") == 0)
                            vb_basex = 200;
                        else if (strcmp(labelpos, "default") == 0)
                            vb_basex = 370;
                    }
                    draw_vertical_banner(g_canvas, g_current_book->getCurrentDigest(), vb_basex, 160, 402 + vb_basex, theme_vertical_bg, theme_vertical_fg, digest_font_size);
                }

                // Name banner
                int32_t basey = 820;

                draw_name_banner(g_canvas, name_with_page, basey, !darktheme, cur_page, total_page, forsnapshot);
            }
            else
            {
                /*
                 //                ui_push_image_to_canvas("/spiffs/frame.png", 0, 380 + BOOKMARKOFFSET + deltaY);
                 bin_font_print(name_with_page, 21, 4, 540, 10, 590 + BOOKMARKOFFSET + deltaY, false); // 0.7f * 30 = 21
                 // 文摘刷新
                 bin_font_print(g_current_book->getCurrentDigest().c_str(), 26, 2, 540, 50, 430 + BOOKMARKOFFSET + deltaY, false); // 0.85f * 30 = 25.5 ≈ 26
                 */
                int basey = 382 + BOOKMARKOFFSET + deltaY;
                int baseh = 160;
                if (!forsnapshot)
                {

                    // Draw a themed vertical strip and separator lines
                    g_canvas->fillRect(0, basey, 60, baseh, TFT_BLACK);
                    g_canvas->fillRect(0, basey, 540, baseh, theme_strip_bg);
                    g_canvas->drawRect(0, basey, 540, baseh, TFT_BLACK);

                    bin_font_print(g_current_book->getCurrentDigest().c_str(), digest_font_size, theme_strip_fg, 540, 70, basey + 20, false, g_canvas, TEXT_ALIGN_LEFT, 0, false, true);
                    // head
                    g_canvas->fillRect(0, basey, 60, baseh, TFT_BLACK);
                    g_canvas->drawLine(60, basey + 5, 540, basey + 5, theme_strip_fg);
                    g_canvas->drawLine(60, basey + baseh - 5, 540, baseh + basey - 5, theme_strip_fg);

                    g_canvas->drawLine(0, basey + 5, 60, basey + 5, TFT_WHITE);
                    g_canvas->drawLine(0, basey + baseh - 5, 60, baseh + basey - 5, TFT_WHITE);

                    g_canvas->drawCircle(30, basey + baseh / 2, 20, TFT_WHITE);
                    g_canvas->fillCircle(30, basey + baseh / 2, 15, TFT_WHITE);
                    g_canvas->drawWideLine(0, basey + baseh / 2, 60, basey + baseh / 2, 1.5, TFT_WHITE);
                }
                draw_name_banner(g_canvas, name_with_page, basey + 162, false, cur_page, total_page, forsnapshot);
            }
        }
    }
    if (!forsnapshot)                                    // 截屏不需要 triangle , 腰封
        bin_font_flush_canvas(false, false, true, RECT); // quality will be reset by the bin flush way!
};

void lockscreen_image_cache_invalidate()
{
    reset_lock_image_cache();
    g_lock_image_cache.sd_ready = false;
}