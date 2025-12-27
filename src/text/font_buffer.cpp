#include "font_buffer.h"
#include "text/book_handle.h"
#include "text/text_handle.h"
#include "text/bin_font_print.h"
#include "SD/SDWrapper.h"
#include <unordered_map>
#include <algorithm>
#include <Arduino.h>
#include <SPIFFS.h>

// 全局字体缓存管理器实例
FontBufferManager g_font_buffer_manager;

// 通用字符缓存（UI和常用字符）
PageFontCache g_common_char_cache;

// 书籍文件名字体缓存（用于文件列表显示）
PageFontCache g_bookname_char_cache;

// TOC（目录）专用字体缓存
PageFontCache g_toc_char_cache;

// 通用回收池缓存（回收其他缓存释放的字符）
PageFontCache g_common_recycle_pool;

// 获取通用字符列表
std::string getCommonCharList()
{
    return
        // 阿拉伯数字
        "0123456789"
        // 英文字母
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
        // 菜单常用字符
        "：:锁屏书签下划线重新索引已读跳过繁简转换竖排显示深色模式？?/第页体按照文件名字"
        "体打开清理显示连接返回无线方式阅读设置默认方向手柄向上下翻左右手习惯底部中主题"
        "浅随机通用壁纸恢复出厂籍图片影响不受残存无对应的留缓存内容确认重置选择";
}

// ========== PageFontCache 实现 ==========

PageFontCache::PageFontCache()
    : buffer_(nullptr), header_(nullptr), index_area_(nullptr), bitmap_area_(nullptr)
{
}

PageFontCache::~PageFontCache()
{
    clear();
}

void PageFontCache::clear()
{
    if (buffer_)
    {
        // 在释放前，回收字符到通用回收池
        recycleCharsToPool(this);

        free(buffer_);
        buffer_ = nullptr;
        header_ = nullptr;
        index_area_ = nullptr;
        bitmap_area_ = nullptr;
    }
    stats_ = {};
}

size_t PageFontCache::getCharCount() const
{
    if (!header_)
        return 0;
    return header_->char_count;
}

size_t PageFontCache::getTotalSize() const
{
    if (!header_)
        return 0;
    return header_->total_size;
}

// 从UTF-8文本中提取唯一的Unicode字符（返回使用 PSRAMAllocator 的向量）
std::vector<uint16_t, PSRAMAllocator<uint16_t>> PageFontCache::extractUniqueChars(const std::string &page_text)
{
    std::unordered_set<uint16_t> unique_set;
    std::vector<uint16_t, PSRAMAllocator<uint16_t>> result;

    const uint8_t *p = reinterpret_cast<const uint8_t *>(page_text.c_str());
    const uint8_t *end = p + page_text.size();

    while (p < end)
    {
        uint32_t unicode = 0;
        int bytes = 0;

        // 解析UTF-8字符
        if (*p < 0x80)
        {
            // ASCII (1 byte)
            unicode = *p;
            bytes = 1;
        }
        else if ((*p & 0xE0) == 0xC0)
        {
            // 2 bytes
            if (p + 1 < end)
            {
                unicode = ((*p & 0x1F) << 6) | (*(p + 1) & 0x3F);
                bytes = 2;
            }
        }
        else if ((*p & 0xF0) == 0xE0)
        {
            // 3 bytes
            if (p + 2 < end)
            {
                unicode = ((*p & 0x0F) << 12) | ((*(p + 1) & 0x3F) << 6) | (*(p + 2) & 0x3F);
                bytes = 3;
            }
        }
        else if ((*p & 0xF8) == 0xF0)
        {
            // 4 bytes (但我们只支持BMP，这里忽略)
            bytes = 4;
        }

        if (bytes > 0)
        {
            p += bytes;
            // 只保留BMP字符（0x0000-0xFFFF）
            if (unicode <= 0xFFFF && unicode > 0)
            {
                uint16_t unicode16 = static_cast<uint16_t>(unicode);
                if (unique_set.insert(unicode16).second)
                {
                    result.push_back(unicode16);
                }
            }
        }
        else
        {
            // 解析失败，跳过
            p++;
        }
    }

    // 按Unicode排序，便于后续二分查找
    std::sort(result.begin(), result.end());
    return result;
}

// 计算所需的缓冲区大小，并填充字形信息（chars/glyph_infos 使用 PSRAMAllocator）
size_t PageFontCache::calculateBufferSize(const std::vector<uint16_t, PSRAMAllocator<uint16_t>> &chars,
                                          std::vector<CharGlyphInfo, PSRAMAllocator<CharGlyphInfo>> &glyph_infos)
{
    glyph_infos.clear();
    glyph_infos.reserve(chars.size());

    size_t bitmap_total_size = 0;

    for (uint16_t unicode : chars)
    {
        // 从字体中查找字符信息
        const BinFontChar *fc = find_char(unicode);
        if (!fc)
        {
            // 字符不存在于字体中，跳过
            continue;
        }

        CharGlyphInfo info;
        info.unicode = unicode;
        info.width = fc->width;
        info.bitmapW = fc->bitmapW;
        info.bitmapH = fc->bitmapH;
        info.x_offset = fc->x_offset;
        info.y_offset = fc->y_offset;
        info.bitmap_size = fc->bitmap_size;
        info.bitmap_offset = bitmap_total_size; // 累计偏移

        glyph_infos.push_back(info);
        bitmap_total_size += fc->bitmap_size;
    }

    // 计算总大小：头部 + 索引区 + 位图区
    size_t header_size = sizeof(PageFontCacheHeader);
    size_t index_size = glyph_infos.size() * sizeof(CharGlyphInfo);
    size_t total_size = header_size + index_size + bitmap_total_size;

    return total_size;
}

// 构建页面字体缓存
bool PageFontCache::build(BookHandle *book, size_t page_index)
{
    if (!book || !book->isOpen())
    {
        Serial.println("[FontCache] Error: Invalid book handle");
        return false;
    }

    // 如果当前使用的是PROGMEM字体，不需要缓存（PROGMEM访问已经很快）
    if (g_using_progmem_font)
    {
#if DBG_FONT_BUFFER
        Serial.println("[FontCache] Skipping cache for PROGMEM font");
#endif
        return false;
    }

    // 清理旧缓存
    clear();

    // 1. 读取页面内容（不触发缓存更新）
    size_t total_pages = book->getTotalPages();
    if (page_index >= total_pages)
    {
        Serial.printf("[FontCache] Error: Page index %u out of range (total=%u)\n",
                      (unsigned)page_index, (unsigned)total_pages);
        return false;
    }

    // 【关键】：临时锁定缓存初始化，避免递归更新
    extern FontBufferManager g_font_buffer_manager;
    bool was_locked = g_font_buffer_manager.isInitializationLocked();
    g_font_buffer_manager.setInitializationLocked(true);

    TextPageResult page_result;
    // 保存当前位置
    size_t saved_page_index = book->getCurrentPageIndex();

    // 临时跳转到目标页面（不触发缓存更新）
    if (page_index != saved_page_index)
    {
        book->jumpToPage(page_index);
    }
    page_result = book->currentPage();

    // 恢复原位置
    if (page_index != saved_page_index)
    {
        book->jumpToPage(saved_page_index);
    }

    // 解除锁定
    g_font_buffer_manager.setInitializationLocked(was_locked);

    if (!page_result.success || page_result.page_text.empty())
    {
        Serial.printf("[FontCache] Error: Failed to read page %u\n", (unsigned)page_index);
        return false;
    }

    // 2. 提取唯一字符（存放在 PSRAM）
    std::vector<uint16_t, PSRAMAllocator<uint16_t>> unique_chars = extractUniqueChars(page_result.page_text);
    if (unique_chars.empty())
    {
        Serial.printf("[FontCache] Warning: Page %u has no valid characters\n", (unsigned)page_index);
        return false;
    }

    // 3. 计算所需缓冲区大小（glyph_infos 存放在 PSRAM）
    std::vector<CharGlyphInfo, PSRAMAllocator<CharGlyphInfo>> glyph_infos;
    size_t total_size = calculateBufferSize(unique_chars, glyph_infos);

    if (glyph_infos.empty())
    {
        Serial.printf("[FontCache] Warning: Page %u has no glyphs in font\n", (unsigned)page_index);
        return false;
    }

    // 4. 分配缓冲区（使用PSRAM以节省内部DRAM）
    buffer_ = static_cast<uint8_t *>(heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM));
    if (!buffer_)
    {
        // PSRAM分配失败，尝试使用内部RAM
        buffer_ = static_cast<uint8_t *>(malloc(total_size));
        if (!buffer_)
        {
            Serial.printf("[FontCache] Error: Failed to allocate %u bytes for page %u\n",
                          (unsigned)total_size, (unsigned)page_index);
            return false;
        }
#if DBG_FONT_BUFFER
        Serial.printf("[FontCache] Warning: Using internal RAM for page %u cache (%u bytes)\n",
                      (unsigned)page_index, (unsigned)total_size);
#endif
    }

    // 5. 初始化头部
    header_ = reinterpret_cast<PageFontCacheHeader *>(buffer_);
    header_->total_size = total_size;
    header_->char_count = glyph_infos.size();
    header_->index_offset = sizeof(PageFontCacheHeader);
    header_->bitmap_offset = header_->index_offset + glyph_infos.size() * sizeof(CharGlyphInfo);

    // 6. 填充索引区
    index_area_ = reinterpret_cast<CharGlyphInfo *>(buffer_ + header_->index_offset);
    memcpy(index_area_, glyph_infos.data(), glyph_infos.size() * sizeof(CharGlyphInfo));

    // 7. 填充位图区
    bitmap_area_ = buffer_ + header_->bitmap_offset;

    // 从SD卡字体文件读取每个字符的位图数据
    if (!g_bin_font.fontFile)
    {
        Serial.println("[FontCache] Error: Font file not open");
        free(buffer_);
        buffer_ = nullptr;
        header_ = nullptr;
        index_area_ = nullptr;
        bitmap_area_ = nullptr;
        return false;
    }

    // 构建位图区，优先复用其他页面缓存
    stats_ = {}; // reset
    uint32_t start_ms = millis();
    stats_.unique_chars = glyph_infos.size();

    // 获取字体文件互斥锁
    extern SemaphoreHandle_t bin_font_get_file_mutex();
    SemaphoreHandle_t mutex = bin_font_get_file_mutex();

    for (size_t i = 0; i < glyph_infos.size(); i++)
    {
        const CharGlyphInfo &info = glyph_infos[i];

        // 从字体中获取原始字符信息（包含文件偏移）
        const BinFontChar *fc = find_char(info.unicode);
        if (!fc || fc->bitmap_size == 0)
        {
            continue;
        }

        uint8_t *dest = bitmap_area_ + info.bitmap_offset;

        // 尝试从其他缓存复用位图
        const uint8_t *cached = g_font_buffer_manager.getCharBitmapAny(info.unicode);
        // 避免使用当前正在构建的缓存中的未初始化数据
        if (cached && !(cached >= buffer_ && cached < buffer_ + header_->total_size))
        {
            memcpy(dest, cached, fc->bitmap_size);
            stats_.reused_from_cache++;
            continue;
        }

        // 缓存未命中，从SD卡读取（使用互斥锁保护）
        bool got_lock = false;
        if (mutex != nullptr)
        {
            got_lock = (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE);
        }

        size_t read_bytes = SDW::SD.readAtOffset(g_bin_font.fontFile,
                                                 fc->bitmap_offset,
                                                 dest,
                                                 fc->bitmap_size);

        if (got_lock)
        {
            xSemaphoreGive(mutex);
        }

        stats_.loaded_from_sd++;

        if (read_bytes != fc->bitmap_size)
        {
            Serial.printf("[FontCache] Warning: Failed to read glyph for U+%04X (expected %u, got %u)\n",
                          info.unicode, fc->bitmap_size, (unsigned)read_bytes);
        }
    }

    stats_.build_ms = millis() - start_ms;
    stats_.total_chars = header_->char_count;

#if DBG_FONT_BUFFER
    Serial.printf("[FontCache] Built cache for page %u: %u chars, %u bytes (reuse=%u, sd=%u, %ums)\n",
                  (unsigned)page_index, (unsigned)glyph_infos.size(), (unsigned)total_size,
                  (unsigned)stats_.reused_from_cache, (unsigned)stats_.loaded_from_sd,
                  (unsigned)stats_.build_ms);

#endif
    return true;
}

bool PageFontCache::hasChar(uint16_t unicode) const
{
    if (!header_ || !index_area_)
        return false;

    // 优化的二分查找（减少分支判断）
    size_t count = header_->char_count;
    size_t first = 0;
    
    while (count > 0)
    {
        size_t step = count >> 1;  // count / 2
        size_t mid = first + step;
        
        if (index_area_[mid].unicode < unicode)
        {
            first = mid + 1;
            count -= step + 1;
        }
        else
        {
            count = step;
        }
    }
    
    return (first < header_->char_count && index_area_[first].unicode == unicode);
}

const CharGlyphInfo *PageFontCache::getCharGlyphInfo(uint16_t unicode) const
{
    if (!header_ || !index_area_)
        return nullptr;

    // 优化的二分查找（减少分支判断）
    size_t count = header_->char_count;
    size_t first = 0;
    
    while (count > 0)
    {
        size_t step = count >> 1;  // count / 2
        size_t mid = first + step;
        
        if (index_area_[mid].unicode < unicode)
        {
            first = mid + 1;
            count -= step + 1;
        }
        else
        {
            count = step;
        }
    }
    
    return (first < header_->char_count && index_area_[first].unicode == unicode) ? &index_area_[first] : nullptr;
}

const CharGlyphInfo *PageFontCache::getCharGlyphInfoByIndex(size_t index) const
{
    if (!header_ || !index_area_ || index >= header_->char_count)
    {
        return nullptr;
    }
    return &index_area_[index];
}

const uint8_t *PageFontCache::getCharBitmap(uint16_t unicode) const
{
    const CharGlyphInfo *info = getCharGlyphInfo(unicode);
    if (!info || !bitmap_area_)
        return nullptr;

    return bitmap_area_ + info->bitmap_offset;
}

void PageFontCache::swapWith(PageFontCache &other)
{
    std::swap(buffer_, other.buffer_);
    std::swap(header_, other.header_);
    std::swap(index_area_, other.index_area_);
    std::swap(bitmap_area_, other.bitmap_area_);
}

void PageFontCache::setCache(uint8_t *buffer, PageFontCacheHeader *header,
                             CharGlyphInfo *index_area, uint8_t *bitmap_area,
                             uint32_t build_ms, uint32_t loaded_from_sd,
                             uint32_t unique_chars, uint32_t total_chars)
{
    buffer_ = buffer;
    header_ = header;
    index_area_ = index_area;
    bitmap_area_ = bitmap_area;
    stats_.build_ms = build_ms;
    stats_.loaded_from_sd = loaded_from_sd;
    stats_.unique_chars = unique_chars;
    stats_.total_chars = total_chars;
}

// ========== FontBufferManager 实现 ==========

FontBufferManager::FontBufferManager()
    : current_page_index_(0), initialized_(false), initialization_locked_(false)
{
    resetStats();
}

// 运行时日志开关接口实现
void FontBufferManager::setLogEnabled(bool enabled)
{
    log_enabled_ = enabled;
}

bool FontBufferManager::isLogEnabled() const
{
    return log_enabled_;
}

FontBufferManager::~FontBufferManager()
{
    clearAll();
}

void FontBufferManager::clearAll()
{
    for (size_t i = 0; i < FONT_CACHE_PAGE_COUNT; i++)
    {
        caches_[i].clear();
    }
    initialized_ = false;
    resetStats();
}

int FontBufferManager::getCacheIndex(int page_offset) const
{
    // 缓存数组索引：-2,-1,0,+1,+2 映射到 0,1,2,3,4
    return FONT_CACHE_CENTER_INDEX + page_offset;
}

bool FontBufferManager::isValidPageOffset(int page_offset) const
{
    return page_offset >= -2 && page_offset <= 2;
}

bool FontBufferManager::initialize(BookHandle *book, size_t current_page_index)
{
    if (log_enabled_)
        Serial.printf("[FontBufferManager] initialize() called: book=%p, current_page_index=%u\n",
                      book, (unsigned)current_page_index);

    if (!book || !book->isOpen())
    {
        if (log_enabled_)
            Serial.println("[FontBufferManager] Error: Invalid book handle");
        return false;
    }

    // 如果当前使用的是PROGMEM字体，不需要缓存
    if (g_using_progmem_font)
    {
        if (log_enabled_)
            Serial.println("[FontBufferManager] PROGMEM font detected, cache disabled");
        return false;
    }

    clearAll();

    current_page_index_ = current_page_index;
    size_t total_pages = book->getTotalPages();

    if (log_enabled_)
        Serial.printf("[FontBufferManager] Book state: total_pages=%u\n",
                      (unsigned)total_pages);

    if (total_pages == 0)
    {
        if (log_enabled_)
            Serial.println("[FontBufferManager] Error: Book has no pages");
        return false;
    }

    if (log_enabled_)
        Serial.printf("[FontBufferManager] Initializing cache for page %u/%u\n",
                      (unsigned)current_page_index, (unsigned)total_pages);

    // 构建5个页面的缓存：前2页、前1页、当前页、后1页、后2页
    for (int offset = -2; offset <= 2; offset++)
    {
        int cache_idx = getCacheIndex(offset);
        int64_t target_page = static_cast<int64_t>(current_page_index) + offset;

        // 检查页面是否在有效范围内
        if (target_page < 0 || target_page >= static_cast<int64_t>(total_pages))
        {
            // 超出范围，保持nullptr
            if (log_enabled_)
                Serial.printf("[FontBufferManager]   Cache[%d] (offset %+d): out of range\n",
                              cache_idx, offset);
            continue;
        }

        // 构建缓存
        bool success = caches_[cache_idx].build(book, static_cast<size_t>(target_page));
        if (success)
        {
            if (log_enabled_)
                Serial.printf("[FontBufferManager]   Cache[%d] (offset %+d, page %lld): OK\n",
                              cache_idx, offset, target_page);
        }
        else
        {
            if (log_enabled_)
                Serial.printf("[FontBufferManager]   Cache[%d] (offset %+d, page %lld): FAILED\n",
                              cache_idx, offset, target_page);
        }
    }

    initialized_ = true;
    resetStats();
    return true;
}

bool FontBufferManager::scrollUpdate(BookHandle *book, size_t new_current_page, bool forward)
{
    if (!initialized_ || !book || !book->isOpen())
    {
        // 未初始化或book无效，执行完整初始化
        return initialize(book, new_current_page);
    }

    size_t total_pages = book->getTotalPages();
    if (new_current_page >= total_pages)
    {
        return false;
    }

    int page_diff = static_cast<int>(new_current_page) - static_cast<int>(current_page_index_);

    // 如果跨度太大（超过2页），重新初始化
    if (abs(page_diff) > 2)
    {
        if (log_enabled_)
            Serial.printf("[FontBufferManager] Large page jump (%d), reinitializing\n", page_diff);
        return initialize(book, new_current_page);
    }

    if (page_diff == 0)
    {
        // 没有变化
        return true;
    }

    if (log_enabled_)
        Serial.printf("[FontBufferManager] Scrolling from page %u to %u (diff=%d, forward=%d)\n",
                      (unsigned)current_page_index_, (unsigned)new_current_page,
                      page_diff, forward ? 1 : 0);

    // 滚动更新：移动缓存，构建新的边界页面
    if (forward)
    {
        // 向前翻页
        for (int i = 0; i < page_diff; i++)
        {
            // 向左滚动：丢弃最左侧缓存，右移所有缓存，构建新的最右侧缓存
            caches_[0].clear();
            for (size_t j = 0; j < FONT_CACHE_PAGE_COUNT - 1; j++)
            {
                // 交换缓存（避免内存拷贝）
                caches_[j].swapWith(caches_[j + 1]);
            }

            // 构建新的最右侧缓存 (+2页) —— 改为延迟预取，避免阻塞当前翻页渲染
            current_page_index_++;
            // 留空，后续可在空闲或需要时再构建 caches_[4]
        }
    }
    else
    {
        // 向后翻页
        for (int i = 0; i < -page_diff; i++)
        {
            // 向右滚动：丢弃最右侧缓存，左移所有缓存，构建新的最左侧缓存
            caches_[4].clear();
            for (int j = FONT_CACHE_PAGE_COUNT - 1; j > 0; j--)
            {
                caches_[j].swapWith(caches_[j - 1]);
            }

            // 构建新的最左侧缓存 (-2页) —— 改为延迟预取
            current_page_index_--;
            // 留空，后续可在空闲或需要时再构建 caches_[0]
        }
    }

    // 仅确保当前页缓存存在；其他预取延后到渲染完成后再做
    auto ensure_cache = [&](int offset)
    {
        int64_t target_page = static_cast<int64_t>(current_page_index_) + offset;
        if (target_page < 0 || target_page >= static_cast<int64_t>(total_pages))
            return;
        int idx = getCacheIndex(offset);
        if (caches_[idx].isValid())
            return;
        caches_[idx].build(book, static_cast<size_t>(target_page));
    };

    ensure_cache(0); // 当前页必须可用，命中则不重建

    return true;
}

bool FontBufferManager::hasChar(uint16_t unicode, int page_offset) const
{
    if (!initialized_ || !isValidPageOffset(page_offset))
    {
        return false;
    }

    int cache_idx = getCacheIndex(page_offset);
    return caches_[cache_idx].hasChar(unicode);
}

const CharGlyphInfo *FontBufferManager::getCharGlyphInfo(uint16_t unicode, int page_offset) const
{
    if (!initialized_ || !isValidPageOffset(page_offset))
    {
        return nullptr;
    }

    int cache_idx = getCacheIndex(page_offset);
    return caches_[cache_idx].getCharGlyphInfo(unicode);
}

const uint8_t *FontBufferManager::getCharBitmap(uint16_t unicode, int page_offset) const
{
    // 优化：优先查找最可能命中的页面缓存（快速路径）
    if (initialized_ && isValidPageOffset(page_offset))
    {
        int cache_idx = getCacheIndex(page_offset);
        const uint8_t *bmp = caches_[cache_idx].getCharBitmap(unicode);
        if (bmp)
        {
            stats_.hits++;
            return bmp;
        }
    }

    // 查找通用缓存（高频字符）
    if (g_common_char_cache.isValid())
    {
        const uint8_t *bmp = g_common_char_cache.getCharBitmap(unicode);
        if (bmp)
        {
            stats_.hits++;
            return bmp;
        }
    }

    // 查找通用回收池
    if (g_common_recycle_pool.isValid())
    {
        const uint8_t *bmp = g_common_recycle_pool.getCharBitmap(unicode);
        if (bmp)
        {
            stats_.hits++;
            return bmp;
        }
    }

    // 查找TOC缓存
    if (g_toc_char_cache.isValid())
    {
        const uint8_t *bmp = g_toc_char_cache.getCharBitmap(unicode);
        if (bmp)
        {
            stats_.hits++;
            return bmp;
        }
    }

    // 查找书籍文件名缓存
    if (g_bookname_char_cache.isValid())
    {
        const uint8_t *bmp = g_bookname_char_cache.getCharBitmap(unicode);
        if (bmp)
        {
            stats_.hits++;
            return bmp;
        }
    }

    stats_.misses++;
    return nullptr;
} // 从任意有效缓存中查找位图（用于构建新缓存时复用）
const uint8_t *FontBufferManager::getCharBitmapAny(uint16_t unicode) const
{
    // 优先查找通用缓存
    if (g_common_char_cache.isValid())
    {
        const uint8_t *bmp = g_common_char_cache.getCharBitmap(unicode);
        if (bmp)
            return bmp;
    }

    // 查找通用回收池
    if (g_common_recycle_pool.isValid())
    {
        const uint8_t *bmp = g_common_recycle_pool.getCharBitmap(unicode);
        if (bmp)
            return bmp;
    }

    // 查找TOC缓存
    if (g_toc_char_cache.isValid())
    {
        const uint8_t *bmp = g_toc_char_cache.getCharBitmap(unicode);
        if (bmp)
            return bmp;
    }

    // 查找书籍文件名缓存
    if (g_bookname_char_cache.isValid())
    {
        const uint8_t *bmp = g_bookname_char_cache.getCharBitmap(unicode);
        if (bmp)
            return bmp;
    }

    if (!initialized_)
        return nullptr;
    // 优先当前窗口顺序：0, +/-1, +/-2
    const int offsets[] = {0, -1, 1, -2, 2};
    for (int off : offsets)
    {
        if (!isValidPageOffset(off))
            continue;
        int idx = getCacheIndex(off);
        const uint8_t *bmp = caches_[idx].getCharBitmap(unicode);
        if (bmp)
        {
            return bmp;
        }
    }
    return nullptr;
}

void FontBufferManager::prefetchAround(BookHandle *book)
{
    if (!initialized_ || !book || !book->isOpen())
        return;
    size_t total_pages = book->getTotalPages();
    auto build_if_missing = [&](int offset)
    {
        int64_t target_page = static_cast<int64_t>(current_page_index_) + offset;
        if (target_page < 0 || target_page >= static_cast<int64_t>(total_pages))
            return;
        int idx = getCacheIndex(offset);
        if (caches_[idx].isValid())
            return;
        caches_[idx].build(book, static_cast<size_t>(target_page));
    };

    // 只补缺，不重建；顺序：邻近，再远端
    build_if_missing(-1);
    build_if_missing(+1);
    build_if_missing(-2);
    build_if_missing(+2);
}

bool FontBufferManager::isCacheValid(int page_offset) const
{
    if (!initialized_ || !isValidPageOffset(page_offset))
    {
        return false;
    }

    int cache_idx = getCacheIndex(page_offset);
    return caches_[cache_idx].isValid();
}

void FontBufferManager::resetStats()
{
    stats_.hits = 0;
    stats_.misses = 0;
}

void FontBufferManager::logStats() const
{
    if (log_enabled_)
    {
        Serial.printf("[FontBufferManager] Cache stats: hits=%u misses=%u initialized=%d current_page=%u\n",
                      (unsigned)stats_.hits, (unsigned)stats_.misses, initialized_ ? 1 : 0,
                      (unsigned)current_page_index_);
    }
}

// ========== 通用字符缓存 ==========

void buildCommonCharCache()
{
    extern bool g_using_progmem_font;
    if (g_using_progmem_font)
    {
#if DBG_FONT_BUFFER
        Serial.println("[CommonCache] Skip for PROGMEM font");
#endif
        return;
    }

    g_common_char_cache.clear();

    std::string common_chars = getCommonCharList();
    if (common_chars.empty())
    {
#if DBG_FONT_BUFFER
        Serial.println("[CommonCache] No common chars to cache");
#endif
        return;
    }

#if DBG_FONT_BUFFER
    Serial.printf("[CommonCache] Building cache for %u chars...\n", (unsigned)common_chars.size());
#endif

    uint32_t start_ms = millis();

    // 1. 提取字符列表
    std::unordered_set<uint16_t> unique_set;
    std::vector<uint16_t, PSRAMAllocator<uint16_t>> unique_chars;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(common_chars.c_str());
    const uint8_t *end = p + common_chars.size();

    while (p < end)
    {
        uint32_t unicode = 0;
        int bytes = 0;

        if (*p < 0x80)
        {
            unicode = *p;
            bytes = 1;
        }
        else if ((*p & 0xE0) == 0xC0)
        {
            if (p + 1 < end)
            {
                unicode = ((*p & 0x1F) << 6) | (*(p + 1) & 0x3F);
                bytes = 2;
            }
        }
        else if ((*p & 0xF0) == 0xE0)
        {
            if (p + 2 < end)
            {
                unicode = ((*p & 0x0F) << 12) | ((*(p + 1) & 0x3F) << 6) | (*(p + 2) & 0x3F);
                bytes = 3;
            }
        }
        else if ((*p & 0xF8) == 0xF0)
        {
            bytes = 4;
        }

        if (bytes > 0)
        {
            p += bytes;
            if (unicode <= 0xFFFF && unicode > 0)
            {
                uint16_t unicode16 = static_cast<uint16_t>(unicode);
                if (unique_set.insert(unicode16).second)
                {
                    unique_chars.push_back(unicode16);
                }
            }
        }
        else
        {
            p++;
        }
    }

    std::sort(unique_chars.begin(), unique_chars.end());

    // 2. 计算缓冲区大小
    std::vector<CharGlyphInfo, PSRAMAllocator<CharGlyphInfo>> glyph_infos;
    glyph_infos.reserve(unique_chars.size());
    size_t bitmap_total_size = 0;

    for (uint16_t unicode : unique_chars)
    {
        const BinFontChar *fc = find_char(unicode);
        if (!fc)
            continue;

        CharGlyphInfo info;
        info.unicode = unicode;
        info.width = fc->width;
        info.bitmapW = fc->bitmapW;
        info.bitmapH = fc->bitmapH;
        info.x_offset = fc->x_offset;
        info.y_offset = fc->y_offset;
        info.bitmap_size = fc->bitmap_size;
        info.bitmap_offset = bitmap_total_size;

        glyph_infos.push_back(info);
        bitmap_total_size += fc->bitmap_size;
    }

    if (glyph_infos.empty())
    {
#if DBG_FONT_BUFFER
        Serial.println("[CommonCache] No glyphs found in font");
#endif
        return;
    }

    // 3. 分配缓冲区（使用PSRAM以节省内部DRAM）
    size_t header_size = sizeof(PageFontCacheHeader);
    size_t index_size = glyph_infos.size() * sizeof(CharGlyphInfo);
    size_t total_size = header_size + index_size + bitmap_total_size;

    uint8_t *buffer = static_cast<uint8_t *>(heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM));
    if (!buffer)
    {
        // PSRAM分配失败，尝试使用内部RAM
        buffer = static_cast<uint8_t *>(malloc(total_size));
        if (!buffer)
        {
#if DBG_FONT_BUFFER
            Serial.printf("[CommonCache] Failed to allocate %u bytes\n", (unsigned)total_size);
#endif
            return;
        }
#if DBG_FONT_BUFFER
        Serial.printf("[CommonCache] Warning: Using internal RAM (%u bytes)\n", (unsigned)total_size);
#endif
    }

    // 4. 初始化结构
    PageFontCacheHeader *header = reinterpret_cast<PageFontCacheHeader *>(buffer);
    header->total_size = total_size;
    header->char_count = glyph_infos.size();
    header->index_offset = header_size;
    header->bitmap_offset = header_size + index_size;

    CharGlyphInfo *index_area = reinterpret_cast<CharGlyphInfo *>(buffer + header->index_offset);
    memcpy(index_area, glyph_infos.data(), glyph_infos.size() * sizeof(CharGlyphInfo));

    uint8_t *bitmap_area = buffer + header->bitmap_offset;

    // 5. 加载位图
    extern BinFont g_bin_font;
    extern SemaphoreHandle_t bin_font_get_file_mutex();
    SemaphoreHandle_t mutex = bin_font_get_file_mutex();
    uint32_t loaded_from_sd = 0;

    for (size_t i = 0; i < glyph_infos.size(); i++)
    {
        const CharGlyphInfo &info = glyph_infos[i];
        const BinFontChar *fc = find_char(info.unicode);
        if (!fc || fc->bitmap_size == 0)
            continue;

        uint8_t *dest = bitmap_area + info.bitmap_offset;

        bool got_lock = false;
        if (mutex != nullptr)
        {
            got_lock = (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE);
        }

        size_t read_bytes = SDW::SD.readAtOffset(g_bin_font.fontFile,
                                                 fc->bitmap_offset,
                                                 dest,
                                                 fc->bitmap_size);

        if (got_lock)
        {
            xSemaphoreGive(mutex);
        }

        if (read_bytes == fc->bitmap_size)
        {
            loaded_from_sd++;
        }
    }

    // 6. 设置到全局缓存
    uint32_t build_ms = millis() - start_ms;
    g_common_char_cache.setCache(buffer, header, index_area, bitmap_area,
                                 build_ms, loaded_from_sd,
                                 glyph_infos.size(), glyph_infos.size());

#if DBG_FONT_BUFFER
    Serial.printf("[CommonCache] Built: %u chars, %u bytes, %u ms\n",
                  (unsigned)glyph_infos.size(), (unsigned)total_size,
                  (unsigned)build_ms);
#endif
}

// ========== 书籍文件名字体缓存 ==========

void clearBookNameCache()
{
    g_bookname_char_cache.clear();
#if DBG_FONT_BUFFER
    Serial.println("[BookNameCache] Cleared");
#endif
}

void addBookNamesToCache(const std::vector<std::string> &bookNames)
{
    extern bool g_using_progmem_font;
    if (g_using_progmem_font)
    {
#if DBG_FONT_BUFFER
        Serial.println("[BookNameCache] Skip for PROGMEM font");
#endif
        return;
    }

    if (bookNames.empty())
    {
#if DBG_FONT_BUFFER
        Serial.println("[BookNameCache] No book names to cache");
#endif
        return;
    }

#if DBG_FONT_BUFFER
    Serial.printf("[BookNameCache] Adding %u book names...\n", (unsigned)bookNames.size());
#endif

    uint32_t start_ms = millis();

    // 1. 从现有缓存中提取已有字符
    std::unordered_set<uint16_t> existing_chars;
    if (g_bookname_char_cache.isValid())
    {
        size_t char_count = g_bookname_char_cache.getCharCount();
        for (uint32_t i = 0; i < char_count; i++)
        {
            const CharGlyphInfo *glyph = g_bookname_char_cache.getCharGlyphInfoByIndex(i);
            if (glyph)
            {
                existing_chars.insert(glyph->unicode);
            }
        }
    }

    // 2. 从书籍文件名中提取新字符
    std::vector<uint16_t, PSRAMAllocator<uint16_t>> new_chars;
    for (const auto &bookName : bookNames)
    {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(bookName.c_str());
        const uint8_t *end = p + bookName.size();

        while (p < end)
        {
            uint32_t unicode = 0;
            int bytes = 0;

            if (*p < 0x80)
            {
                unicode = *p;
                bytes = 1;
            }
            else if ((*p & 0xE0) == 0xC0)
            {
                if (p + 1 < end)
                {
                    unicode = ((*p & 0x1F) << 6) | (*(p + 1) & 0x3F);
                    bytes = 2;
                }
            }
            else if ((*p & 0xF0) == 0xE0)
            {
                if (p + 2 < end)
                {
                    unicode = ((*p & 0x0F) << 12) | ((*(p + 1) & 0x3F) << 6) | (*(p + 2) & 0x3F);
                    bytes = 3;
                }
            }
            else if ((*p & 0xF8) == 0xF0)
            {
                bytes = 4;
            }

            if (bytes > 0)
            {
                p += bytes;
                if (unicode <= 0xFFFF && unicode > 0)
                {
                    uint16_t unicode16 = static_cast<uint16_t>(unicode);
                    if (existing_chars.insert(unicode16).second)
                    {
                        new_chars.push_back(unicode16);
                    }
                }
            }
            else
            {
                p++;
            }
        }
    }

    // 3. 检查是否需要重建缓存（超过300字限制或有新字符）
    bool need_rebuild = false;
    if (existing_chars.size() > 300)
    {
        // 超过限制，保留最近的300字（简单策略：清空重建）
#if DBG_FONT_BUFFER
        Serial.printf("[BookNameCache] Size limit exceeded (%u > 300), rebuilding...\n",
                      (unsigned)existing_chars.size());
#endif
        existing_chars.clear();
        // 只保留新发现的字符（最多300个）
        for (size_t i = 0; i < new_chars.size() && i < 300; i++)
        {
            existing_chars.insert(new_chars[i]);
        }
        new_chars.clear();
        need_rebuild = true;
    }
    else if (!new_chars.empty())
    {
        need_rebuild = true;
    }

    if (!need_rebuild)
    {
#if DBG_FONT_BUFFER
        Serial.println("[BookNameCache] No new chars, keeping existing cache");
#endif
        return;
    }

    // 4. 构建字符列表 (置于 PSRAM)
    std::vector<uint16_t, PSRAMAllocator<uint16_t>> all_chars(existing_chars.begin(), existing_chars.end());
    std::sort(all_chars.begin(), all_chars.end());

    // 5. 计算缓冲区大小
    std::vector<CharGlyphInfo, PSRAMAllocator<CharGlyphInfo>> glyph_infos;
    glyph_infos.reserve(all_chars.size());
    size_t bitmap_total_size = 0;

    for (uint16_t unicode : all_chars)
    {
        const BinFontChar *fc = find_char(unicode);
        if (!fc)
            continue;

        CharGlyphInfo info;
        info.unicode = unicode;
        info.width = fc->width;
        info.bitmapW = fc->bitmapW;
        info.bitmapH = fc->bitmapH;
        info.x_offset = fc->x_offset;
        info.y_offset = fc->y_offset;
        info.bitmap_size = fc->bitmap_size;
        info.bitmap_offset = bitmap_total_size;

        glyph_infos.push_back(info);
        bitmap_total_size += fc->bitmap_size;
    }

    if (glyph_infos.empty())
    {
#if DBG_FONT_BUFFER
        Serial.println("[BookNameCache] No glyphs found in font");
#endif
        return;
    }

    // 6. 分配缓冲区（使用PSRAM以节省内部DRAM）
    size_t header_size = sizeof(PageFontCacheHeader);
    size_t index_size = glyph_infos.size() * sizeof(CharGlyphInfo);
    size_t total_size = header_size + index_size + bitmap_total_size;

    uint8_t *buffer = static_cast<uint8_t *>(heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM));
    if (!buffer)
    {
        // PSRAM分配失败，尝试使用内部RAM
        buffer = static_cast<uint8_t *>(malloc(total_size));
        if (!buffer)
        {
#if DBG_FONT_BUFFER
            Serial.printf("[BookNameCache] Failed to allocate %u bytes\n", (unsigned)total_size);
#endif
            return;
        }
#if DBG_FONT_BUFFER
        Serial.printf("[BookNameCache] Warning: Using internal RAM (%u bytes)\n", (unsigned)total_size);
#endif
    }

    // 7. 初始化结构
    PageFontCacheHeader *header = reinterpret_cast<PageFontCacheHeader *>(buffer);
    header->total_size = total_size;
    header->char_count = glyph_infos.size();
    header->index_offset = header_size;
    header->bitmap_offset = header_size + index_size;

    CharGlyphInfo *index_area = reinterpret_cast<CharGlyphInfo *>(buffer + header->index_offset);
    memcpy(index_area, glyph_infos.data(), glyph_infos.size() * sizeof(CharGlyphInfo));

    uint8_t *bitmap_area = buffer + header->bitmap_offset;

    // 8. 加载位图（优先从其他缓存复用）
    extern BinFont g_bin_font;
    extern SemaphoreHandle_t bin_font_get_file_mutex();
    SemaphoreHandle_t mutex = bin_font_get_file_mutex();
    uint32_t reused = 0;
    uint32_t loaded_from_sd = 0;

    for (size_t i = 0; i < glyph_infos.size(); i++)
    {
        const CharGlyphInfo &info = glyph_infos[i];
        const BinFontChar *fc = find_char(info.unicode);
        if (!fc || fc->bitmap_size == 0)
            continue;

        uint8_t *dest = bitmap_area + info.bitmap_offset;

        // 先尝试从其他缓存复用
        const uint8_t *cached = g_font_buffer_manager.getCharBitmapAny(info.unicode);
        if (cached)
        {
            memcpy(dest, cached, fc->bitmap_size);
            reused++;
        }
        else
        {
            // 从SD加载
            bool got_lock = false;
            if (mutex != nullptr)
            {
                got_lock = (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE);
            }

            size_t read_bytes = SDW::SD.readAtOffset(g_bin_font.fontFile,
                                                     fc->bitmap_offset,
                                                     dest,
                                                     fc->bitmap_size);

            if (got_lock)
            {
                xSemaphoreGive(mutex);
            }

            if (read_bytes == fc->bitmap_size)
            {
                loaded_from_sd++;
            }
        }
    }

    // 9. 清空旧缓存并设置新缓存
    g_bookname_char_cache.clear();
    uint32_t build_ms = millis() - start_ms;
    g_bookname_char_cache.setCache(buffer, header, index_area, bitmap_area,
                                   build_ms, loaded_from_sd,
                                   glyph_infos.size(), glyph_infos.size());

#if DBG_FONT_BUFFER
    Serial.printf("[BookNameCache] Built: %u chars (%u reused, %u from SD), %u bytes, %u ms\n",
                  (unsigned)glyph_infos.size(), (unsigned)reused, (unsigned)loaded_from_sd,
                  (unsigned)total_size, (unsigned)build_ms);
#endif
}

// ========== TOC 字体缓存 ==========

void clearTocCache()
{
    // 在释放前，回收字符到通用回收池
    recycleCharsToPool(&g_toc_char_cache);

    g_toc_char_cache.clear();
#if DBG_FONT_BUFFER
    Serial.println("[TocCache] Cleared");
#endif
}

void buildTocCharCache(const char *toc_file_path)
{
    extern bool g_using_progmem_font;
    if (g_using_progmem_font)
    {
#if DBG_FONT_BUFFER
        Serial.println("[TocCache] Skip for PROGMEM font");
#endif
        return;
    }

    if (!toc_file_path || strlen(toc_file_path) == 0)
    {
#if DBG_FONT_BUFFER
        Serial.println("[TocCache] Invalid TOC file path");
#endif
        return;
    }

#if DBG_FONT_BUFFER
    Serial.printf("[TocCache] Building cache from: %s\n", toc_file_path);
#endif

    // 清理旧缓存
    clearTocCache();

    uint32_t start_ms = millis();

    // 1. 打开TOC文件并读取全部内容（TOC通常不大）
    File tocFile;
    bool use_spiffs = (strncmp(toc_file_path, "/spiffs/", 8) == 0);

    if (use_spiffs)
    {
        // SPIFFS: 去掉 /spiffs 前缀
        const char *spiffs_path = toc_file_path + 7; // skip "/spiffs"
        tocFile = SPIFFS.open(spiffs_path, "r");
    }
    else
    {
        // SD: 去掉 /sd 前缀（如果有）
        const char *sd_path = toc_file_path;
        if (strncmp(toc_file_path, "/sd/", 4) == 0)
        {
            sd_path = toc_file_path + 3; // skip "/sd"
        }
        tocFile = SDW::SD.open(sd_path, "r");
    }

    if (!tocFile || !tocFile.available())
    {
#if DBG_FONT_BUFFER
        Serial.printf("[TocCache] Failed to open TOC file: %s\n", toc_file_path);
#endif
        return;
    }

    // 读取全部TOC内容
    size_t toc_size = tocFile.size();
    if (toc_size == 0 || toc_size > 1024 * 1024)
    { // 限制TOC大小（最大1MB）
#if DBG_FONT_BUFFER
        Serial.printf("[TocCache] TOC file size invalid: %u bytes\n", (unsigned)toc_size);
#endif
        tocFile.close();
        return;
    }

    std::string toc_content;
    toc_content.resize(toc_size);
    size_t bytes_read = tocFile.read((uint8_t *)toc_content.data(), toc_size);
    tocFile.close();

    if (bytes_read != toc_size)
    {
#if DBG_FONT_BUFFER
        Serial.printf("[TocCache] Failed to read TOC file (expected %u, got %u)\n",
                      (unsigned)toc_size, (unsigned)bytes_read);
#endif
        return;
    }

    // 2. 提取唯一字符（使用 PSRAM 分配器）
    std::unordered_set<uint16_t> unique_set;
    std::vector<uint16_t, PSRAMAllocator<uint16_t>> unique_chars;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(toc_content.c_str());
    const uint8_t *end = p + toc_content.size();

    while (p < end)
    {
        uint32_t unicode = 0;
        int bytes = 0;

        if (*p < 0x80)
        {
            unicode = *p;
            bytes = 1;
        }
        else if ((*p & 0xE0) == 0xC0)
        {
            if (p + 1 < end)
            {
                unicode = ((*p & 0x1F) << 6) | (*(p + 1) & 0x3F);
                bytes = 2;
            }
        }
        else if ((*p & 0xF0) == 0xE0)
        {
            if (p + 2 < end)
            {
                unicode = ((*p & 0x0F) << 12) | ((*(p + 1) & 0x3F) << 6) | (*(p + 2) & 0x3F);
                bytes = 3;
            }
        }
        else if ((*p & 0xF8) == 0xF0)
        {
            bytes = 4;
        }

        if (bytes > 0)
        {
            p += bytes;
            if (unicode <= 0xFFFF && unicode > 0)
            {
                uint16_t unicode16 = static_cast<uint16_t>(unicode);
                if (unique_set.insert(unicode16).second)
                {
                    unique_chars.push_back(unicode16);
                }
            }
        }
        else
        {
            p++;
        }
    }

    if (unique_chars.empty())
    {
#if DBG_FONT_BUFFER
        Serial.println("[TocCache] No valid characters found in TOC");
#endif
        return;
    }

    std::sort(unique_chars.begin(), unique_chars.end());

    // 3. 计算缓冲区大小
    std::vector<CharGlyphInfo, PSRAMAllocator<CharGlyphInfo>> glyph_infos;
    glyph_infos.reserve(unique_chars.size());
    size_t bitmap_total_size = 0;

    for (uint16_t unicode : unique_chars)
    {
        const BinFontChar *fc = find_char(unicode);
        if (!fc)
            continue;

        CharGlyphInfo info;
        info.unicode = unicode;
        info.width = fc->width;
        info.bitmapW = fc->bitmapW;
        info.bitmapH = fc->bitmapH;
        info.x_offset = fc->x_offset;
        info.y_offset = fc->y_offset;
        info.bitmap_size = fc->bitmap_size;
        info.bitmap_offset = bitmap_total_size;

        glyph_infos.push_back(info);
        bitmap_total_size += fc->bitmap_size;
    }

    if (glyph_infos.empty())
    {
#if DBG_FONT_BUFFER
        Serial.println("[TocCache] No glyphs found in font for TOC chars");
#endif
        return;
    }

    // 4. 分配缓冲区（使用PSRAM以节省内部DRAM）
    size_t header_size = sizeof(PageFontCacheHeader);
    size_t index_size = glyph_infos.size() * sizeof(CharGlyphInfo);
    size_t total_size = header_size + index_size + bitmap_total_size;

    uint8_t *buffer = static_cast<uint8_t *>(heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM));
    if (!buffer)
    {
        buffer = static_cast<uint8_t *>(malloc(total_size));
        if (!buffer)
        {
#if DBG_FONT_BUFFER
            Serial.printf("[TocCache] Failed to allocate %u bytes\n", (unsigned)total_size);
#endif
            return;
        }
#if DBG_FONT_BUFFER
        Serial.printf("[TocCache] Warning: Using internal RAM (%u bytes)\n", (unsigned)total_size);
#endif
    }

    // 5. 初始化结构
    PageFontCacheHeader *header = reinterpret_cast<PageFontCacheHeader *>(buffer);
    header->total_size = total_size;
    header->char_count = glyph_infos.size();
    header->index_offset = header_size;
    header->bitmap_offset = header_size + index_size;

    CharGlyphInfo *index_area = reinterpret_cast<CharGlyphInfo *>(buffer + header->index_offset);
    memcpy(index_area, glyph_infos.data(), glyph_infos.size() * sizeof(CharGlyphInfo));

    uint8_t *bitmap_area = buffer + header->bitmap_offset;

    // 6. 加载位图（优先从其他缓存复用）
    extern BinFont g_bin_font;
    extern SemaphoreHandle_t bin_font_get_file_mutex();
    SemaphoreHandle_t mutex = bin_font_get_file_mutex();
    uint32_t reused = 0;
    uint32_t loaded_from_sd = 0;

    for (size_t i = 0; i < glyph_infos.size(); i++)
    {
        const CharGlyphInfo &info = glyph_infos[i];
        const BinFontChar *fc = find_char(info.unicode);
        if (!fc || fc->bitmap_size == 0)
            continue;

        uint8_t *dest = bitmap_area + info.bitmap_offset;

        // 先尝试从其他缓存复用
        const uint8_t *cached = g_font_buffer_manager.getCharBitmapAny(info.unicode);
        if (cached)
        {
            memcpy(dest, cached, fc->bitmap_size);
            reused++;
        }
        else
        {
            // 从SD加载
            bool got_lock = false;
            if (mutex != nullptr)
            {
                got_lock = (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE);
            }

            size_t read_bytes = SDW::SD.readAtOffset(g_bin_font.fontFile,
                                                     fc->bitmap_offset,
                                                     dest,
                                                     fc->bitmap_size);

            if (got_lock)
            {
                xSemaphoreGive(mutex);
            }

            if (read_bytes == fc->bitmap_size)
            {
                loaded_from_sd++;
            }
        }
    }

    // 7. 设置到全局TOC缓存
    uint32_t build_ms = millis() - start_ms;
    g_toc_char_cache.setCache(buffer, header, index_area, bitmap_area,
                              build_ms, loaded_from_sd,
                              glyph_infos.size(), glyph_infos.size());

#if DBG_FONT_BUFFER
    Serial.printf("[TocCache] Built: %u chars (%u reused, %u from SD), %u bytes, %u ms\n",
                  (unsigned)glyph_infos.size(), (unsigned)reused, (unsigned)loaded_from_sd,
                  (unsigned)total_size, (unsigned)build_ms);
#endif
}

// ========== 通用回收池缓存 ==========

void clearCommonRecyclePool()
{
    g_common_recycle_pool.clear();
#if DBG_FONT_BUFFER
    Serial.println("[RecyclePool] Cleared");
#endif
}

void initCommonRecyclePool()
{
    // 清空旧池（如果存在）
    clearCommonRecyclePool();

#if DBG_FONT_BUFFER
    Serial.println("[RecyclePool] Initialized (empty pool ready)");
#endif
}

void recycleCharsToPool(const PageFontCache *cache)
{
    extern bool g_using_progmem_font;
    if (g_using_progmem_font)
    {
        return; // PROGMEM字体不需要回收池
    }

    if (!cache || !cache->isValid())
    {
        return; // 无效缓存，无需回收
    }

    // 使用全局宏定义的上限
    const size_t pool_limit = RECYCLE_POOL_LIMIT;

    // 1. 收集当前池中已有的字符
    std::unordered_set<uint16_t> existing_chars;
    if (g_common_recycle_pool.isValid())
    {
        size_t char_count = g_common_recycle_pool.getCharCount();
        for (size_t i = 0; i < char_count; i++)
        {
            const CharGlyphInfo *glyph = g_common_recycle_pool.getCharGlyphInfoByIndex(i);
            if (glyph)
            {
                existing_chars.insert(glyph->unicode);
            }
        }
    }

    // 2. 从待释放缓存中提取新字符（不在池中的）
    std::vector<uint16_t, PSRAMAllocator<uint16_t>> new_chars;
    size_t cache_char_count = cache->getCharCount();
    for (size_t i = 0; i < cache_char_count; i++)
    {
        const CharGlyphInfo *glyph = cache->getCharGlyphInfoByIndex(i);
        if (glyph && existing_chars.insert(glyph->unicode).second)
        {
            new_chars.push_back(glyph->unicode);
        }
    }

    if (new_chars.empty())
    {
        return; // 没有新字符需要回收
    }

    // 3. 检查是否超出上限，超出则按Unicode从大到小淘汰
    if (existing_chars.size() > pool_limit)
    {
        // 将所有字符按Unicode排序（从小到大）
        std::vector<uint16_t, PSRAMAllocator<uint16_t>> all_chars(existing_chars.begin(), existing_chars.end());
        std::sort(all_chars.begin(), all_chars.end());

        // 保留前 pool_limit 个（Unicode最小的），丢弃后面的（Unicode大的）
        existing_chars.clear();
        for (size_t i = 0; i < pool_limit && i < all_chars.size(); i++)
        {
            existing_chars.insert(all_chars[i]);
        }

#if DBG_FONT_BUFFER
        Serial.printf("[RecyclePool] Evicted %u chars (limit=%u)\n",
                      (unsigned)(all_chars.size() - pool_limit),
                      (unsigned)pool_limit);
#endif
    }

    // 4. 构建新的回收池
    std::vector<uint16_t, PSRAMAllocator<uint16_t>> final_chars(existing_chars.begin(), existing_chars.end());
    std::sort(final_chars.begin(), final_chars.end());

    // 5. 计算缓冲区大小
    std::vector<CharGlyphInfo, PSRAMAllocator<CharGlyphInfo>> glyph_infos;
    glyph_infos.reserve(final_chars.size());
    size_t bitmap_total_size = 0;

    for (uint16_t unicode : final_chars)
    {
        const BinFontChar *fc = find_char(unicode);
        if (!fc)
            continue;

        CharGlyphInfo info;
        info.unicode = unicode;
        info.width = fc->width;
        info.bitmapW = fc->bitmapW;
        info.bitmapH = fc->bitmapH;
        info.x_offset = fc->x_offset;
        info.y_offset = fc->y_offset;
        info.bitmap_size = fc->bitmap_size;
        info.bitmap_offset = bitmap_total_size;

        glyph_infos.push_back(info);
        bitmap_total_size += fc->bitmap_size;
    }

    if (glyph_infos.empty())
    {
        return;
    }

    // 6. 分配缓冲区（使用PSRAM）
    size_t header_size = sizeof(PageFontCacheHeader);
    size_t index_size = glyph_infos.size() * sizeof(CharGlyphInfo);
    size_t total_size = header_size + index_size + bitmap_total_size;

    uint8_t *buffer = static_cast<uint8_t *>(heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM));
    if (!buffer)
    {
        buffer = static_cast<uint8_t *>(malloc(total_size));
        if (!buffer)
        {
#if DBG_FONT_BUFFER
            Serial.printf("[RecyclePool] Failed to allocate %u bytes\n", (unsigned)total_size);
#endif
            return;
        }
    }

    // 7. 初始化结构
    PageFontCacheHeader *header = reinterpret_cast<PageFontCacheHeader *>(buffer);
    header->total_size = total_size;
    header->char_count = glyph_infos.size();
    header->index_offset = header_size;
    header->bitmap_offset = header_size + index_size;

    CharGlyphInfo *index_area = reinterpret_cast<CharGlyphInfo *>(buffer + header->index_offset);
    memcpy(index_area, glyph_infos.data(), glyph_infos.size() * sizeof(CharGlyphInfo));

    uint8_t *bitmap_area = buffer + header->bitmap_offset;

    // 8. 复制位图（从旧池或待回收缓存）
    uint32_t reused_from_old_pool = 0;
    uint32_t reused_from_cache = 0;

    for (size_t i = 0; i < glyph_infos.size(); i++)
    {
        const CharGlyphInfo &info = glyph_infos[i];
        const BinFontChar *fc = find_char(info.unicode);
        if (!fc || fc->bitmap_size == 0)
            continue;

        uint8_t *dest = bitmap_area + info.bitmap_offset;

        // 先尝试从旧池复用
        if (g_common_recycle_pool.isValid())
        {
            const uint8_t *old_bmp = g_common_recycle_pool.getCharBitmap(info.unicode);
            if (old_bmp)
            {
                memcpy(dest, old_bmp, fc->bitmap_size);
                reused_from_old_pool++;
                continue;
            }
        }

        // 从待回收缓存复用
        const uint8_t *cache_bmp = cache->getCharBitmap(info.unicode);
        if (cache_bmp)
        {
            memcpy(dest, cache_bmp, fc->bitmap_size);
            reused_from_cache++;
        }
    }

    // 9. 清空旧池并设置新池
    g_common_recycle_pool.clear();
    g_common_recycle_pool.setCache(buffer, header, index_area, bitmap_area,
                                   0, 0,
                                   glyph_infos.size(), glyph_infos.size());

#if DBG_FONT_BUFFER
    Serial.printf("[RecyclePool] Recycled: %u total chars (%u from old pool, %u from cache), %u bytes\n",
                  (unsigned)glyph_infos.size(), (unsigned)reused_from_old_pool,
                  (unsigned)reused_from_cache, (unsigned)total_size);
#endif
}
