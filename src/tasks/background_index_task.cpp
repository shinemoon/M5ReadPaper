#include "background_index_task.h"
#include "text/book_handle.h"
#include "globals.h"
#include "current_book.h"
#include "../SD/SDWrapper.h"
#include "task_priorities.h"
#include <Arduino.h>
#include <vector>
#include "device/safe_fs.h"
#include "test/per_file_debug.h"
#include "text/tags_handle.h"

// ----- Debug logging (compile-time switch) -----
#ifndef BG_INDEX_DEBUG
#define BG_INDEX_DEBUG 0  // 打开调试日志
#endif
#if BG_INDEX_DEBUG
#define BGLOG(...) Serial.printf(__VA_ARGS__)
#else
#define BGLOG(...) do { } while (0)
#endif

// Local helpers to manipulate page/progress/complete files from background task.
// Forward declarations for local helpers used earlier in this file
static bool patchPageFileCountLocal(const std::string &page_file, uint32_t count);
static bool writeCompleteMarkerLocal(BookHandle *bh);

// Public wrapper for patchPageFileCountLocal (used by config_manager when switching books)
bool patchPageFileCount(const std::string &page_file, uint32_t count)
{
    return patchPageFileCountLocal(page_file, count);
}

static bool writeProgressFor(BookHandle *bh)
{
    if (!bh)
        return false;
    if (!ensureBookmarksFolder())
        return false;
    std::string progress_file = bh->getProgressFileName();
    return SafeFS::safeWrite(progress_file, [&](File &f) {
        f.printf("file_path=%s\n", bh->filePath().c_str());
        f.printf("file_size=%zu\n", bh->getIndexingFileSize());
        f.printf("current_pos=%zu\n", bh->getIndexingCurrentPos());
        f.printf("pages_generated=%zu\n", bh->getTotalPages());
        f.printf("area_width=%d\n", bh->getAreaWidth());
        f.printf("area_height=%d\n", bh->getAreaHeight());
        f.printf("font_size=%.2f\n", bh->getFontSize());
        f.printf("encoding=%d\n", (int)bh->getEncoding());
        f.printf("start_time=%lu\n", (unsigned long)0);
        f.printf("last_update=%lu\n", millis());
        f.printf("valid=true\n");
        return true;
    });
}

// Finalize completion across related artifacts: ensure page count patched,
// write .complete marker, remove .progress, and refresh bookmark (.bm) with
// authoritative total_pages from the in-memory index (page_positions).
static void finalizeIndexArtifacts(BookHandle *bh)
{
    if (!bh)
        return;
    std::string page_file = bh->getPageFileName();
    
    (void)patchPageFileCountLocal(page_file, (uint32_t)bh->getTotalPages());
    
    (void)writeCompleteMarkerLocal(bh);
    
    // 【关键修复】先保存书签（使用当前内存中的完整page_positions），
    // 最后才设置index_just_completed标志，避免saveBookmarkForFile中的getTotalPages触发重新加载
    (void)saveBookmarkForFile(bh);
    
    // Synchronize in-memory complete flag (also removes .progress)
    // 【重要】放在最后，这样前面的getTotalPages调用不会触发重新加载
    bh->markIndexingComplete();
}

// Append page offsets to an already-open page file handle.
// Important: keep SD critical section short and avoid extra open/close/flush per batch.
static bool appendOffsetsToPageFile(File &pf, const std::vector<uint32_t> &offsets)
{
    if (offsets.empty())
        return true;

    for (uint32_t o : offsets)
    {
        pf.write((const uint8_t *)&o, sizeof(o));
    }

    // Note: This approach means the page file's count field (at offset 8) will be stale
    // until patchPageFileCountLocal is called. That's acceptable since we update it
    // at the end of each batch and on completion.
    
    return true;
}

static bool patchPageFileCountLocal(const std::string &page_file, uint32_t count)
{
    // CRITICAL FIX: Use in-place update with r+ mode instead of copy-whole-file.
    // The copy approach is too risky during hardware reset for large files.
    // 
    // Risk analysis:
    // - Old approach: copy entire file to .tmp (may take 100-500ms for large files)
    //   If reset occurs mid-copy, both files may be corrupted
    // - New approach: direct seek+write at offset 8 (takes <10ms)
    //   If reset occurs mid-write, only the count field may be corrupted (4 bytes)
    //   but the rest of the file (all page offsets) remain intact
    // 
    // Recovery: if count field is corrupted, we can infer the correct count from file size:
    //   count = (file_size - 12) / 4  (header is 12 bytes, each offset is 4 bytes)
    
    File fh = SDW::SD.open(page_file.c_str(), "r+");
    if (!fh)
        return false;
    
    fh.seek(8);
    fh.write((const uint8_t *)&count, sizeof(count));
    fh.flush();
    // CRITICAL: Add delay to ensure SD card internal buffer is flushed
    delay(20);
    fh.close();
    
    return true;
}

static bool writeCompleteMarkerLocal(BookHandle *bh)
{
    if (!bh)
        return false;
    std::string page_file = bh->getPageFileName();
    // In case a power loss left a .tmp during last write, try to restore
    SafeFS::restoreFromTmpIfNeeded(page_file);
    // derive complete marker by replacing .page with .complete if possible
    std::string complete_marker;
    if (page_file.size() >= 5 && page_file.compare(page_file.size() - 5, 5, ".page") == 0)
    {
        complete_marker = page_file.substr(0, page_file.size() - 5) + ".complete";
    }
    else
    {
        // fallback: construct from file path
        complete_marker = std::string("/bookmarks/") + bh->filePath();
    }

    SafeFS::safeWrite(complete_marker, [&](File &cm) {
        cm.println("complete");
        return true;
    });

    // 【强化】删�?.progress 文件（包括临时文件），并验证删除结果
    std::string progress_file = bh->getProgressFileName();
    bool removed_main = false;
    bool removed_tmp = false;
    
    if (SDW::SD.exists(progress_file.c_str()))
    {
        removed_main = SDW::SD.remove(progress_file.c_str());
        if (!removed_main)
        {
            BGLOG("[BgIndex] WARNING: failed to remove progress file: %s\n", progress_file.c_str());
        }
    }
    else
    {
        removed_main = true; // 不存在视为已删除
    }
    
    std::string tmp = SafeFS::tmpPathFor(progress_file);
    if (SDW::SD.exists(tmp.c_str()))
    {
        removed_tmp = SDW::SD.remove(tmp.c_str());
        if (!removed_tmp)
        {
            BGLOG("[BgIndex] WARNING: failed to remove progress tmp file: %s\n", tmp.c_str());
        }
    }
    else
    {
        removed_tmp = true; // 不存在视为已删除
    }
    
    BGLOG("[BgIndex] Complete marker written, progress cleanup: main=%s tmp=%s\n",
          removed_main ? "ok" : "FAILED", removed_tmp ? "ok" : "FAILED");
    
    return true;
}

// Background incremental page file generator that uses BookHandle's public wrappers.
// Returns true if this segment did useful work; false on no-progress or error.
bool backgroundGeneratePageFileIncremental(BookHandle *bh)
{
    if (!bh)
        return false;

    // Defensive check: if the stored file path is corrupted (doesn't start with
    // '/'), avoid trying to open it which will spam logs and cause wasteful retries.
    std::string fp_check = bh->filePath();
    if (fp_check.empty() || fp_check[0] != '/')
    {
        //Serial.printf("[BgIndex] error: BookHandle filePath appears invalid or corrupted: '%s'\n", fp_check.c_str());
        // print hex preview for diagnostics
        for (size_t i = 0; i < fp_check.size() && i < 64; ++i)
        {
            //Serial.printf("%02x ", (uint8_t)fp_check[i]);
        }
        //Serial.println();
        return false;
    }

    // If the book is being closed, abort early to avoid use-after-free
    if (bh->isClosing())
    {
        //Serial.println("[BgIndex] background: BookHandle isClosing()==true, aborting segment");
        return false;
    }

    // If a force reindex has been requested, abort early so the background task
    // can perform deletion+rebuild logic as soon as possible.
    if (isForceReindexPending())
    {
        //Serial.println("[BgIndex] background: isForceReindexPending detected at entry, aborting segment");
        return false;
    }

#if DBG_BOOK_HANDLE
    //Serial.printf("[BgIndex] backgroundGeneratePageFileIncremental: called for %s\n", bh->filePath().c_str());
#else
    //Serial.printf("[BgIndex] backgroundGeneratePageFileIncremental: called\n");
#endif

    // Log task stack high water mark for debugging (words)
    UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
    (void)hw; // silence unused in release
    //Serial.printf("[BgIndex] background: task stack high-water mark (words)=%u\n", (unsigned int)hw);

    // Open read handle
    File indexing_file = bh->openIndexingReadHandle();
    if (!indexing_file)
    {
        //Serial.println("[BgIndex] background: failed to open read handle");
        return false;
    }

    size_t file_size = indexing_file.size();
    bh->setIndexingFileSize(file_size);

    // 【关键修复】在加载进度前，先检�?.complete 标记
    // 如果书籍已经完成索引，直接返回，避免重复索引
    // 【强化】增加内存标志检查，避免在finalize后立即re-enter导致清空page_positions
    std::string complete_marker = bh->getCompleteFileName();
    if (SDW::SD.exists(complete_marker.c_str()) || bh->isIndexingComplete())
    {
        // 如果内存标志已标记完成，立即返回，不要再验证.page文件
        // 这样可以避免在finalize后的re-entry中误删page_positions
        if (bh->isIndexingComplete())
        {
            indexing_file.close();
            BGLOG("[BgIndex] indexing_complete flag set, skipping for %s\n", bh->filePath().c_str());
            return false;
        }
        
        // 如果只是.complete存在但内存标志未设置，验�?page文件
        std::string page_file = bh->getPageFileName();
        if (SDW::SD.exists(page_file.c_str()))
        {
            File pf = SDW::SD.open(page_file.c_str(), "r");
            if (pf)
            {
                uint8_t magic[4] = {0};
                if (pf.read(magic, 4) == 4 && 
                    magic[0] == 'B' && magic[1] == 'P' && magic[2] == 'G' && magic[3] == '1')
                {
                    // .complete 存在�?.page 文件有效 �?已完成，无需索引
                    bh->markIndexingComplete();
                    indexing_file.close();
                    BGLOG("[BgIndex] .complete marker exists and valid, skipping indexing for %s\n", 
                          bh->filePath().c_str());
                    return false;
                }
                pf.close();
            }
        }
    }

    // Try to load progress from disk (no-op if already loaded)
    bool resume_from_progress = bh->loadIndexProgressFromDisk();
    BGLOG("[BgIndex] seg-begin path=%s cur=%zu size=%zu resume=%d\n", bh->filePath().c_str(), bh->getIndexingCurrentPos(), file_size, resume_from_progress ? 1 : 0);

    // If not resuming from progress, try to recover from existing .page file
    std::string page_file = bh->getPageFileName();
    if (!resume_from_progress)
    {
        // If there exists a .page file, attempt to load it (may be partial)
        if (SDW::SD.exists(page_file.c_str()))
        {
            // 【关键修复】先加载现有的page_positions到内存，避免丢失已索引的页面
            size_t pages_before_load = bh->getTotalPages();
            if (bh->loadPageFile())
            {
                size_t pages_after_load = bh->getTotalPages();
                BGLOG("[BgIndex] Loaded .page file: pages_before=%zu, pages_after=%zu\n", 
                      pages_before_load, pages_after_load);
                
                // If page_positions were loaded, set indexing_current_pos to last offset by reading the .page file tail
                // Read last uint32 from binary page file
                File pf_r = SDW::SD.open(page_file.c_str(), "r");
                if (pf_r)
                {
                    // header: 4+1+3+4 = 12 bytes, then count*4 offsets
                    pf_r.seek(0, SeekEnd);
                    size_t pf_size = pf_r.position();
                    if (pf_size >= 12 + 4)
                    {
                        // read count
                        pf_r.seek(8);
                        uint32_t count = 0;
                        pf_r.read((uint8_t *)&count, sizeof(count));
                        BGLOG("[BgIndex] .page file: size=%zu bytes, count_field=%u, loaded_pages=%zu\n",
                              pf_size, count, pages_after_load);
                        
                        if (count > 0)
                        {
                            // read last offset
                            size_t last_offset_pos = 12 + (count - 1) * 4;
                            if (last_offset_pos + 4 <= pf_size)
                            {
                                pf_r.seek(last_offset_pos);
                                uint32_t last_off = 0;
                                pf_r.read((uint8_t *)&last_off, sizeof(last_off));
                                bh->setIndexingCurrentPos((size_t)last_off);
                                resume_from_progress = true;
                                BGLOG("[BgIndex] Resume from .page file: last_offset=%u (pos %zu/%zu in file)\n",
                                      last_off, last_offset_pos, pf_size);
                            }
                        }
                    }
                    pf_r.close();
                }
            }
            else
            {
                BGLOG("[BgIndex] WARNING: .page file exists but loadPageFile() failed\n");
            }
        }
    }

    // If still not resuming, create new page file and initialize in-memory positions
    // 【额外防护】在清空page_positions前，最后一次检查是否已完成
    // 避免在finalize后的re-entry中误删已完成的索引数�?
    if (!resume_from_progress)
    {
        // 如果内存标志已标记完成，说明是finalize后的re-entry，立即返�?
        if (bh->isIndexingComplete())
        {
            indexing_file.close();
            BGLOG("[BgIndex] indexing_complete detected before clearPagePositions, aborting fresh start\n");
            return false;
        }
        
        // 如果page_positions已有大量数据�?100页），且.complete标记存在�?
        // 也应该避免清空，这可能是一个刚完成索引的re-entry
        if (bh->getTotalPages() > 100)
        {
            std::string complete_chk = bh->getCompleteFileName();
            if (SDW::SD.exists(complete_chk.c_str()))
            {
                indexing_file.close();
                BGLOG("[BgIndex] .complete exists and %zu pages loaded, aborting fresh start\n", 
                      bh->getTotalPages());
                bh->markIndexingComplete();
                return false;
            }
        }
        
        // start fresh
        BGLOG("[BgIndex] Starting fresh: clearing page_positions and creating new .page file\n");
        bh->clearPagePositions();
        // Create page file with header and first page offset 0
        File wf = SDW::SD.open(page_file.c_str(), "w");
        if (!wf)
        {
            //Serial.printf("[BgIndex] background: cannot create page file: %s\n", page_file.c_str());
            indexing_file.close();
            return false;
        }
        const uint8_t magic[4] = {'B', 'P', 'G', '1'};
        wf.write(magic, 4);
        uint8_t ver = 1;
        wf.write((const uint8_t *)&ver, 1);
        uint8_t reserved[3] = {0, 0, 0};
        wf.write(reserved, 3);
        uint32_t zero = 0;
        wf.write((const uint8_t *)&zero, sizeof(zero)); // placeholder count
        uint32_t p0 = 0;
        wf.write((const uint8_t *)&p0, sizeof(p0));
        wf.close();
        bh->appendPagePosition(0);
        bh->setIndexingCurrentPos(0);
    }

    // 记录本次工作循环的“读取起点”，用于在主循环里做无前进的收尾判断
    {
        size_t cycle_start = bh->getIndexingCurrentPos();
        bh->setLastIndexCycleStart(cycle_start);
    }

    // Open for append binary once per segment (minimize FAT metadata churn)
    File pf = SDW::SD.open(page_file.c_str(), "a");
    if (!pf)
    {
        //Serial.printf("[BgIndex] background: cannot open page file for append: %s\n", page_file.c_str());
        indexing_file.close();
        return false;
    }

    bh->setIndexingInProgress(true);

    // Tune for responsiveness: smaller chunk and single batch keeps SD bus free more often
    // 【优化】进一步缩短时间片和批次，优先保证翻页流畅性
    const size_t CHUNK_PAGES = 16;   // 从16降到8，减少单批页数
    const int MAX_BATCHES = 4;      // 从4降到2，减少连续批次
    unsigned long segment_start = millis();
    const unsigned long MAX_SEGMENT_MS = 50; // 从80ms降到50ms，留更多时间给翻页

    // Rate-limit progress file writes to reduce FAT updates (and long flushes)
    auto writeProgressRateLimited = [&](bool force) -> bool {
        static uint32_t last_ms = 0;
        uint32_t now = millis();
        if (!force && (now - last_ms) < 500) // 最多每秒写一次进�?
            return true;
        last_ms = now;
        return writeProgressFor(bh);
    };

    int total_new_pages = 0;
    bool reached_eof = false;

    for (int batch_no = 0; batch_no < MAX_BATCHES; ++batch_no)
    {
        // if the BookHandle is being closed, stop immediately
        if (bh->isClosing())
        {
            //Serial.printf("[BgIndex] background: detected closing during batch %d, saving progress and aborting\n", batch_no);
            writeProgressRateLimited(true);
            bh->setIndexingInProgress(false);
            return false;
        }
        // If a global force reindex request was posted, abort this segment to
        // let the background task handle the atomic rebuild path.
        if (isForceReindexPending())
        {
            //Serial.println("[BgIndex] background: isForceReindexPending detected inside loop, aborting segment");
            // persist progress and clear in-progress flag
            writeProgressRateLimited(true);
            bh->setIndexingInProgress(false);
            break;
        }

        if (bh->getAndClearIndexingShouldStop())
        {
            BGLOG("[BgIndex] *** STOP REQUESTED *** Aborting segment at pos=%zu, pages=%zu\n",
                  bh->getIndexingCurrentPos(), bh->getTotalPages());
            // persist progress
            writeProgressRateLimited(true);
            break;
        }

        size_t start_pos = bh->getIndexingCurrentPos();
        if (start_pos >= file_size)
            break;

        int16_t area_w = bh->getAreaWidth();
        int16_t area_h = bh->getAreaHeight();
        float font_size = bh->getFontSize();
        TextEncoding enc = bh->getEncoding();
        bool vertical = bh->getVerticalText();

        // Prefer UI access: avoid waiting on file lock; if UI is busy reading, skip this cycle
        // 【优化】获取锁前主动让步，让翻页任务有机会先执行
        taskYIELD();
        
        bool lock_acquired = bh->tryAcquireFileLock(pdMS_TO_TICKS(0));
        if (!lock_acquired)
        {
            // UI 正在读文件，稍后再来
            BGLOG("[BgIndex] skip: UI holds file lock, cur=%zu\n", bh->getIndexingCurrentPos());
            writeProgressRateLimited(false);
            break;
        }
        
        // 【优化】持有锁时间尽可能短，立即执行分页并释放
    BuildIndexResult br = build_book_page_index(indexing_file, bh->filePath(), area_w, area_h, font_size, enc, CHUNK_PAGES, start_pos, vertical, bh);
        bh->releaseFileLockPublic();
        
        // 【优化】释放锁后立即让步，让翻页任务有机会执行
        taskYIELD();
        std::vector<size_t> batch = br.pages;

        // 【关键日志】每次分页调用的返回结果
        BGLOG("[BgIndex] build_book_page_index returned: start_pos=%zu pages=%zu reached_eof=%s\n", 
              start_pos, batch.size(), br.reached_eof ? "true" : "false");
        
#if DBG_BOOK_HANDLE
    if (!batch.empty())
    {
        size_t first = batch.front();
        size_t last = batch.back();
        BGLOG("[BgIndex] batch offsets: first=%zu last=%zu\n", first, last);
    }
#endif

        if (batch.empty())
        {
            // nothing more to do in this segment
            break;
        }

        // collect offsets (skip first which equals start_pos)
        std::vector<uint32_t> offsets_to_write;
        for (size_t i = 1; i < batch.size(); ++i)
        {
            uint32_t p32 = (uint32_t)batch[i];
            offsets_to_write.push_back(p32);
        }

        // write offsets to page file in one append
        if (!appendOffsetsToPageFile(pf, offsets_to_write))
        {
            //Serial.printf("[BgIndex] background: failed to append offsets to %s\n", page_file.c_str());
            // try to save progress and abort
            writeProgressRateLimited(true);
            break;
        }
        
        // 【优化】写入后主动让步，让翻页任务有机会执行
        taskYIELD();

        // update in-memory positions
        for (uint32_t o : offsets_to_write)
        {
            bh->appendPagePosition((size_t)o);
            total_new_pages++;
        }
        
        // Log actual page count after appending to memory
        BGLOG("[BgIndex] appended %zu offsets, pages_total=%zu\n", offsets_to_write.size(), bh->getTotalPages());

#if DBG_BOOK_HANDLE
    //Serial.printf("[BgIndex] appended %zu offsets to %s\n", offsets_to_write.size(), page_file.c_str());
#endif

        size_t last_pos = batch.back();

        bh->setIndexingCurrentPos(last_pos);
        writeProgressRateLimited(false);
        BGLOG("[BgIndex] advance cur -> %zu of %zu\n", last_pos, file_size);

#if DBG_BOOK_HANDLE
        //Serial.printf("[BgIndex] setIndexingCurrentPos -> %zu file_size=%zu total_pages=%zu\n", last_pos, file_size, bh->getTotalPages());
#endif

        // 【主要完成判断】检查分页算法是否已到达文件末尾
        // reached_eof 由 build_book_page_index 在读到 file.available()==false 时设置
        if (br.reached_eof)
        {
            
            // 基本合理性检查：最后偏移不应超过文件大小
            if (!batch.empty())
            {
                size_t last_offset = batch.back();
                if (last_offset <= file_size)
                {
                    reached_eof = true;
                    finalizeIndexArtifacts(bh);

                    // 完成后重置无前进计数�?
                    bh->setNoProgressStreak(0);
                    break;
                }
                else
                {
                    BGLOG("[BgIndex] warning: reached_eof but last_offset=%zu > file_size=%zu, continuing\n", 
                          last_offset, file_size);
                }
            }
        }
        
        // 【备用防御】如�?reached_eof 未正确设置（异常情况），用连续无前进作为备用判断
        // 这是一个安全网，防止在极端情况下索引永远无法完�?
        if (last_pos <= start_pos)
        {
            uint8_t streak = bh->getNoProgressStreak() + 1;
            bh->setNoProgressStreak(streak);
            
            BGLOG("[BgIndex] No forward progress: start=%zu last=%zu, streak=%u/10\n", 
                  start_pos, last_pos, streak);
            
            const uint8_t NO_PROGRESS_THRESHOLD = 10;
            if (streak >= NO_PROGRESS_THRESHOLD)
            {
                BGLOG("[BgIndex] No-progress threshold reached -> marking complete\n");
                reached_eof = true;
                finalizeIndexArtifacts(bh);
                
                bh->setNoProgressStreak(0);
                break;
            }
            else
            {
                // 暂停并保存进度，等待下次继续
                writeProgressFor(bh);
                break;
            }
        }
        else
        {
            // 有前进，重置计数�?
            if (bh->getNoProgressStreak() > 0)
            {
                BGLOG("[BgIndex] Progress made, reset streak\n");
                bh->setNoProgressStreak(0);
            }
        }

        // time-slice check
        if ((millis() - segment_start) >= MAX_SEGMENT_MS)
        {
            break;
        }

        // reopen indexing file position in case build_book_page_index consumed data
        // (ensure file handle position is valid for next batch)
        // build_book_page_index is expected to update based on start_pos; we keep open handle
    // Do not wait here; return control to caller quickly in main-loop driven mode
    }

    if (pf)
    {
        // Flush once per segment to reduce long-latency FAT updates
        pf.flush();
        // Yield briefly so higher-priority tasks can run
    // Avoid extra waits here to keep the main loop responsive
        pf.close();
    }

    // close reader
    indexing_file.close();

    // 【关键修复】Segment 结束前强制写入进度，避免下次加载时回退到旧位置
    // 同时更新.page文件的count字段，确保文件状态与内存一�?
    if (!reached_eof && total_new_pages > 0)
    {
        writeProgressFor(bh);
        // 【新增】每个segment结束时更�?page文件的count字段
        // 这样即使在切换书籍或断电时，.page文件也是最新的
        std::string page_file_to_patch = bh->getPageFileName();
        (void)patchPageFileCountLocal(page_file_to_patch, (uint32_t)bh->getTotalPages());
        BGLOG("[BgIndex] seg-end: forced progress write at cur=%zu\n", bh->getIndexingCurrentPos());
    }

    if (total_new_pages > 0)
    {
        //Serial.printf("[BgIndex] background: processed %d new pages in this segment\n", total_new_pages);
    }

    if (!reached_eof)
        bh->setIndexingInProgress(false);
    BGLOG("[BgIndex] seg-end new_pages=%d reached_eof=%d cur=%zu\n", total_new_pages, reached_eof ? 1 : 0, bh->getIndexingCurrentPos());
    return (total_new_pages > 0) || reached_eof;
}

// ============================================================================
// 单线程同步索引工作循环（主循环调用）
// ============================================================================

// pending flag for requesting a force reindex from main loop
static bool pending_force_reindex = false;
// Flag set when force-reindex has started processing
static bool force_reindex_started = false;

bool isForceReindexPending()
{
    return pending_force_reindex;
}

// Run a single work cycle synchronously: handle force-reindex if pending,
// then attempt one incremental indexing segment for the current book.
bool runBackgroundIndexWorkCycle()
{
    bool did_anything = false;

    // Snapshot current book to avoid races and ensure lifetime during this cycle
    std::shared_ptr<BookHandle> local_bh_sp = current_book_shared();
    BookHandle *local_bh = local_bh_sp ? local_bh_sp.get() : nullptr;

    // If the snapshot book is closing, skip doing work in this cycle
    if (local_bh && local_bh->isClosing())
    {
        return false;
    }

    // Passive defense: if indexing is effectively complete (on-disk .complete marker exists),
    // skip any work to avoid spurious post-complete indexing.
    if (local_bh && local_bh->isIndexingComplete())
    {
        return false;
    }

    // 1) Handle pending force reindex request first
    if (pending_force_reindex)
    {
        if (local_bh && !local_bh->isClosing())
        {
            // Non-blocking stop request; do not wait here
            auto cur_sp = std::atomic_load(&__g_current_book_shared);
            if (cur_sp)
            {
                cur_sp->requestStopIndexing();
            }

            // Indicate started so waiters can proceed
            force_reindex_started = true;

            extern void removeIndexFilesForBookForPath(const std::string &book_file_path);
            if (cur_sp)
            {
                // 【安全保护】在删除索引文件前，确保tags缓存是最新的
                // 虽然requestForceReindex已经保存了tags，但这里再次刷新缓存以确保一致性
                cur_sp->refreshTagsCache();
                
                removeIndexFilesForBookForPath(cur_sp->filePath());
                cur_sp->clearPagePositions();
                cur_sp->setPagesLoaded(false);
                cur_sp->setIndexingInProgress(false);
                cur_sp->setIndexingCurrentPos(0);
                cur_sp->setIndexingFileSize(0);
                cur_sp->setPageCompleted(false);
                // No need to set indexing_complete flag - we trust disk .complete file
                (void)cur_sp->getAndClearIndexingShouldStop();
                // 复位启发式记录：last 与 streak
                cur_sp->resetIndexCycleHeuristics();
                // 同步书签：重置索引相关字段（不删除 bm 文件）
                (void)saveBookmarkForFile(cur_sp.get());
                
                // 【安全保护】再次刷新tags缓存，确保UI可以正确显示tags
                // 这样即使在重新索引过程中，用户也能看到保存的书签
                cur_sp->refreshTagsCache();
            }

            // Do at most one incremental segment this cycle
            if (!local_bh->isClosing())
            {
                bool seg = backgroundGeneratePageFileIncremental(local_bh);
                did_anything = did_anything || seg;
            }

            force_reindex_started = false;
            // Clear pending only after we actually started processing
            pending_force_reindex = false;
        }
        else
        {
            // No valid book or closing; keep pending for next cycle
            return did_anything;
        }
    }


    // 2) If no force-reindex in flight (or after handling), do one incremental segment
    local_bh_sp = current_book_shared();
    local_bh = local_bh_sp ? local_bh_sp.get() : nullptr;
    if (local_bh && !local_bh->isClosing() && local_bh->canContinueIndexing())
    {
        bool seg = backgroundGeneratePageFileIncremental(local_bh);
        did_anything = did_anything || seg;
        BGLOG("[BgIndex] work-cycle: seg=%d complete=%d cur=%zu pages=%zu\n", seg ? 1 : 0, local_bh->isIndexingComplete() ? 1 : 0, local_bh->getIndexingCurrentPos(), local_bh->getTotalPages());
    }

    return did_anything;
}

// ============================================================================
// Public API for force reindex
// ============================================================================

void requestForceReindex()
{
    // Request a force reindex in the main loop.
    // Immediately clean up index files and reset state.
    if (g_current_book)
    {
        // 【关键修复】在重新索引前，先确保当前页面的auto tag被保存
        // 这样即使重新索引后，用户也可以通过书签返回到之前的阅读位置
        {
            TextPageResult tp = g_current_book->currentPage();
            if (tp.success)
            {
                // 保存当前页面位置到auto tag (slot0)
                // 注意：insertAutoTagForFile会保留所有现有的manual tags，不会清除它们
                bool tag_saved = insertAutoTagForFile(g_current_book->filePath(), tp.file_pos);
                if (tag_saved)
                {
                    // 刷新内存缓存，确保BookHandle中的cached_tags是最新的
                    g_current_book->refreshTagsCache();
                }
                else
                {
                }
            }
            else
            {
            }
        }
        
        // 【关键修复】立即清空页面索引并设置pages_loaded=false，防止renderCurrentPage中的
        // "对齐书签到页面开头"逻辑误删用户的手动书签
        // 必须在保存书签后、requestStopIndexing前执行
        g_current_book->clearPagePositions();
        g_current_book->setPagesLoaded(false);
        
        g_current_book->requestStopIndexing();

        // Immediate cleanup: reset index files and in-memory state
        {
            // Use atomic load to get the shared BookHandle
            auto cur_sp_now = std::atomic_load(&__g_current_book_shared);
            if (cur_sp_now)
            {
                std::string fp = cur_sp_now->filePath();
                // remove on-disk index/progress/complete files
                extern void removeIndexFilesForBookForPath(const std::string &book_file_path);
                removeIndexFilesForBookForPath(fp);

                // Clear in-memory indexing state to avoid false-complete detection
                cur_sp_now->clearPagePositions();
                cur_sp_now->setPagesLoaded(false);
                cur_sp_now->setIndexingInProgress(false);
                cur_sp_now->setIndexingCurrentPos(0);
                cur_sp_now->setIndexingFileSize(0);
                cur_sp_now->setPageCompleted(false);
                // No need to set indexing_complete flag - we trust disk .complete file
                // Clear any stop request flag and heuristics
                (void)cur_sp_now->getAndClearIndexingShouldStop();
                cur_sp_now->resetIndexCycleHeuristics();

                // Persist bookmark reflecting reset index state
                (void)saveBookmarkForFile(cur_sp_now.get());
            }
        }
    }

    // Set flag to trigger reindex in main loop
    pending_force_reindex = true;
    force_reindex_started = false;
}

bool waitForForceReindexStart(unsigned long timeout_ms)
{
    unsigned long start = millis();
    while (!force_reindex_started)
    {
        if ((millis() - start) >= timeout_ms)
            return false;
        delay(20);
    }
    return true;
}


