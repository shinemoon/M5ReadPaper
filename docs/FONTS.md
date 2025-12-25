**字体处理流程文档（摘要）**

本文档概述本项目中字体相关的端到端流程：字体如何由 webapp/工具生成、在固件中以何种二进制结构存储、固件端如何读取并解码渲染、以及当前的字体缓存策略与优化点。

**一、字体生成（概要）**

  - 使用 `opentype.js`（见 `webapp/extension/vendors/opentype.min.js`）解析 TTF/OTF 字体并获取 glyph。
  - 在页面/Worker 中通过 Canvas 按需渲染字形灰度（见 `webapp/extension/pages/readpaper_renderer.js`）。
  - 渲染结果经过阈值二值化、可选平滑/去噪/膨胀，再打包为 1-bit 位图（函数 `ReadPaperRenderer._pack1Bit`），得到与固件兼容的位图字节流。
  - webapp 也可生成演示 PNG（unpack/preview 功能），方便人工校验字体效果。
  - 脚本可批量生成 `.bin` 字体文件；同时包含 `build_charset()` 用于生成字符集（GBK / Big5 / 日文 / ASCII 等）。
  - 项目约定：charset 的生成仍依赖该脚本（或类脚本），因此在说明中请勿只看 `tools/` JS 以外的实现。 (Till Ext V1.6.1, all later version relying on JS only)

  - 为了生成ReadPaper的PROGMEM字体，在扩展的生成页面，控制台输入 `showmethemoney(true)` 来打开隐藏的勾选项目

**二、字体文件（二进制）格式与数据结构**

  - char_count (uint32_t, 4 bytes)
  - font_height (uint8_t, 1 byte)
  - version (uint8_t, 1 byte)
  - family_name (64 bytes, utf-8 nul-padded)
  - style_name (64 bytes, utf-8 nul-padded)

  - `uint16_t unicode` (2)
  - `uint16_t width` (advance, 2)
  - `uint8_t bitmapW` (1)
  - `uint8_t bitmapH` (1)
  - `int8_t x_offset` (1)
  - `int8_t y_offset` (1)
  - `uint32_t bitmap_offset` (4)
  - `uint32_t bitmap_size` (4)
  - （C 结构中保留 `cached_bitmap` 字段，用于内存缓存或占位）

  - 有两类实现/版本：
    - 简单 1-bit 打包（Webapp/py 脚本生成）：每行按 MSB-first 打包；文件写入前 `pack1Bit` 会对最后一个字节做掩码并对行字节取反（JS 实现与 Python 实现一致）；C 端可以通过 `FontDecoder::decode_bitmap_1bit` 或 `unpack_1bit_bitmap` 解包得到 0/255 灰度像素。
    - 复杂编码（V3/Huffman）：支持更高位深或 2-bit/Huffman 编码以表达灰度，C 端解码器实现为 `FontDecoder::decode_bitmap`/`decode_bitmap_v3`（见 `src/text/font_decoder.cpp`），用于 V3 或未来复杂编码格式。

**三、固件端渲染流程（调用链与关键函数）**

**当前实现（V1.6+）：多级缓存架构**

固件端采用分层缓存策略以平衡内存占用与 I/O 性能：

1. **字体索引层**：
   - 检测格式（`detect_font_format`），读取并缓存 134 字节头部到 PSRAM（`g_font_header_cache`）。
   - 流式模式（`g_font_stream_mode = true`，PROGMEM 字体强制启用）：只加载轻量索引（`GlyphIndex`），并建立 `indexMap`（hash map）用于 O(1) 查找字形度量与文件偏移。位图按需读取。
   - 非流式/完整缓存模式（`g_font_stream_mode = false`，已基本废弃）：将整个字符表解析到 `g_bin_font.chars`（`BinFontChar` vector）并通过 `ChunkedFontCache` 缓存位图。**注意：此模式在 V1.6+ 已很少使用**。

2. **位图读取层（SD 卡字体）**：
   - **不再主要依赖** `GlyphReadWindow` 预读窗口（虽然代码中仍保留该实现）。
   - 当前主流方案是 **各级 PageFontCache**：在渲染前为页面/场景预先构建字形缓存（见下节"四、字体缓存机制"），命中则直接从 PSRAM 读取，未命中才回退到 SD 卡单次读取。
   - PROGMEM 字体（内置）：直接从 Flash 读取，无需 SD I/O。



**四、字体缓存机制（当前 V1.6+ 架构）**

**核心理念**：通过预构建多级字形缓存池，在 PSRAM 中存储完整位图索引，渲染时优先命中缓存避免 SD 卡随机读。

**1. 头部缓存（Header Cache）**
   - 在 `load_bin_font` 时将 134 字节头部缓存到 PSRAM（`g_font_header_cache`），避免频繁小 I/O。

**2. 流式索引（轻量索引）**
   - `g_bin_font.index` 与 `indexMap`（`GlyphIndex`）仅包含每字形的度量与位图偏移/长度（内存占用小），用于按需读取位图。
   - 适合内存受限或大字符集场景（如 PROGMEM 字体）。

**3. 页面级缓存系统（主流方案，替代原窗口预读）**

   实现于 [`src/text/font_buffer.h`](../src/text/font_buffer.h) 和 [`src/text/font_buffer.cpp`](../src/text/font_buffer.cpp)。

   **3.1 核心组件**：
   
   - **`PageFontCache`**：单个页面/场景的字体缓存池，包含：
     - 头部（`PageFontCacheHeader`）：字符数、总大小、索引/位图偏移等元数据。
     - 索引区（`CharGlyphInfo[]`）：每字符的度量、位图偏移、尺寸等。
     - 位图区：连续的位图字节流（1-bit 或灰度）。
     - 分配在 PSRAM（优先）或内部 RAM，构建时从 SD 卡读取或复用其他缓存。
   
   - **`FontBufferManager`**（`g_font_buffer_manager`）：管理 5 页滑动窗口缓存：
     - 当前页（center）+ 前后各 2 页（共 5 个 `PageFontCache`）。
     - 翻页时通过 `scrollUpdate()` 滚动更新：复用已有缓存，只构建新页面。
     - 查询接口：`hasChar()`, `getCharBitmap()` 等，支持按页面偏移（-2 ~ +2）查询。
     - 优先级查找：先查通用缓存，再查 TOC/书名缓存，最后查 5 页窗口。

   **3.2 全局专用缓存（用于特定场景）**：
   
   - **`g_common_char_cache`**（通用字符缓存）：
     - 存储 UI 常用字符（数字、标点、常用汉字等，约 300 字符）。
     - 在加载字体时调用 `buildCommonCharCache()` 构建。
     - 所有场景均可复用，减少重复读取。
   
   - **`g_bookname_char_cache`**（书籍文件名缓存）：
     - 为文件列表显示预构建所有书名中的字符。
     - 调用 `addBookNamesToCache(const std::vector<std::string>& bookNames)` 构建。
     - 加速文件浏览器渲染。
   
   - **`g_toc_char_cache`**（目录/TOC 缓存）：
     - 为书籍目录显示预构建所有章节标题中的字符。
     - 调用 `buildTocCharCache(const char* toc_file_path)` 从 TOC 文件构建。
     - 加速目录浏览。
   
   - **`g_common_recycle_pool`**（通用回收池）：
     - 当 `PageFontCache` 被释放时，将其中的字符回收到此池（上限 1000 字符）。
     - 构建新缓存时优先从回收池复用，减少 SD 读取。

   **3.3 缓存构建流程**（以 `PageFontCache::build()` 为例）：
   
   1. 提取页面文本中的唯一字符（`extractUniqueChars()`）。
   2. 计算缓冲区大小（`calculateBufferSize()`），并分配 PSRAM/RAM。
   3. 填充头部、索引区。
   4. 加载位图（**关键优化**）：
      - 优先从 `FontBufferManager::getCharBitmapAny()` 复用已有缓存（通用缓存、TOC 缓存、其他页面缓存、回收池）。
      - 仅在未命中时才从 SD 卡读取（互斥锁保护文件访问）。
   5. 统计构建耗时、SD 读取次数、复用率。

   **3.4 使用场景**：
   
   - **阅读场景**：`FontBufferManager` 自动为当前页 ±2 页构建缓存，翻页时滚动更新。
   - **菜单/界面**：`g_common_char_cache` 提供 UI 常用字符。
   - **文件列表**：`g_bookname_char_cache` 加速书名渲染。
   - **目录浏览**：`g_toc_char_cache` 加速章节标题渲染。

**4. 遗留组件（V1.6+ 已较少使用）**

   - **`GlyphReadWindow`**（字形预读窗口）：
     - 流式模式下的 PSRAM 预读缓冲（默认 256KB）。
     - 当前已被 `PageFontCache` 体系取代，但代码中仍保留作为回退方案。
   
   - **`ChunkedFontCache`**（分块缓存）：
     - 非流式模式下将整个字体文件分块加载到 PSRAM，支持压缩。
     - V1.6+ 已基本废弃，仅在特定配置下使用。

**5. 性能优化要点**

   - **缓存复用**：构建新缓存时，`getCharBitmapAny()` 会按优先级查找已有缓存（通用→TOC→书名→回收池→5 页窗口），显著减少 SD 读取。
   - **回收池机制**：`g_common_recycle_pool` 收集释放的字符，最多保留 1000 个，供后续页面复用。
   - **PSRAM 分配**：所有缓存优先使用 PSRAM（`heap_caps_malloc(MALLOC_CAP_SPIRAM)`），失败才回退到内部 RAM。
   - **互斥锁保护**：SD 卡文件访问通过 `bin_font_get_file_mutex()` 保护，避免多任务竞态。
   - **统计信息**：每个缓存记录构建耗时、SD 读取次数、复用率（见 `PageFontCacheStats`），便于性能分析。

**五、注意事项与优化建议**

  + From Ext V1.6.2, all charset is handled by JS (and then python is not useful anymore but just for archive)
  + **缓存策略选择**：
    - 阅读场景：启用 `FontBufferManager` 的 5 页滑动窗口（自动管理）。
    - UI/菜单：使用 `g_common_char_cache`（启动时构建一次）。
    - 文件浏览：调用 `addBookNamesToCache()` 为当前文件夹构建书名缓存。
    - 目录浏览：调用 `buildTocCharCache()` 为当前书籍构建 TOC 缓存。
  + **内存管理**：
    - 所有缓存优先分配 PSRAM（ESP32-S3 通常有 2MB~8MB PSRAM），失败才用内部 RAM。
    - 定期清理不需要的缓存（如切换书籍时调用 `g_font_buffer_manager.clearAll()`）。
    - 回收池上限 1000 字符，避免无限增长。
  + **性能监控**：
    - 使用 `PageFontCacheStats` 查看构建耗时、SD 读取次数、复用率。
    - 调用 `FontBufferManager::logStats()` 查看命中率统计。
  + **PROGMEM 字体优先**：
    - 若设备有足够 Flash 且为常用字体，优先烧录到 PROGMEM（`load_bin_font_from_progmem`），完全避免 SD I/O。
  + **调试开关**：
    - `#define DBG_FONT_BUFFER 1` 在 `font_buffer.cpp` 中启用详细日志（构建过程、复用统计等）。

**六、快速参考（关键文件）**

  - **字体生成**：
    - `webapp/extension/pages/readpaper_renderer.js`：字体渲染与 1-bit 打包。
    - `tools/generate_1bit_font_bin.py`：批量生成 `.bin` 字体文件（已较少使用，V1.6.2+ 推荐 JS）。
  
  - **固件端核心**：
    - **索引与解码**：
      - `src/text/bin_font_print.cpp` / `.h`：字体加载、索引构建、字形查找。
      - `src/text/font_decoder.cpp` / `.h`：位图解码（1-bit、V3、Huffman）。
    - **多级缓存系统**（**当前主流**）：
      - `src/text/font_buffer.cpp` / `.h`：`PageFontCache`、`FontBufferManager`、全局缓存（通用/书名/TOC/回收池）。
    - **遗留组件**（V1.6+ 较少使用）：
      - `src/device/chunked_font_cache.cpp` / `.h`：分块缓存（非流式模式）。
      - `GlyphReadWindow`（定义在 `bin_font_print.cpp`）：预读窗口（已被 `PageFontCache` 取代）。
  
  - **工具与映射表**：
    - `tools/generate_gbk_table.py`：生成 GBK→Unicode 查找表（`src/text/gbk_unicode_data.cpp`）。
    - `tools/gen_zh_table.py`：生成繁简转换表（`src/text/zh_conv_table_generated.cpp`）。



**附录：GBK → Unicode 映射表（generate_gbk_table.py）**

- **脚本位置与作用**：`tools/generate_gbk_table.py`。用于生成固件可直接包含的 GBK→Unicode 查找表（C++ 源文件），脚本支持两种模式：`--full` 生成完整映射，`--simple` 生成常用简化表。
  - 生成完整表并写入源码文件：
    - `python tools/generate_gbk_table.py --full > src/text/gbk_unicode_data.cpp`
    - `python tools/generate_gbk_table.py --simple > src/text/gbk_unicode_data.cpp`

  - 同时会定义 `const size_t GBK_TABLE_SIZE` 表示条目数。脚本会在 stderr 输出生成时的日志与估算的 Flash 占用（条目数 * 4 字节）。

- **在固件中的结构与访问**：
  - 头文件 [`src/text/gbk_unicode_table.h`](src/text/gbk_unicode_table.h) 中声明了：
    - `struct GBKToUnicodeEntry { uint16_t gbk_code; uint16_t unicode; };`
    - `extern const GBKToUnicodeEntry gbk_to_unicode_table[];`
    - `extern const size_t GBK_TABLE_SIZE;`
    - 查表/转换函数声明：`uint16_t gbk_to_unicode_lookup(uint16_t)`, `int utf8_encode(uint16_t, uint8_t*)`, `std::string convert_gbk_to_utf8_lookup(const std::string&)` 等。
  - 实现文件 [`src/text/gbk_unicode_table.cpp`](src/text/gbk_unicode_table.cpp) 使用 `pgm_read_word` 从 PROGMEM 读取表项并用二分查找（`gbk_to_unicode_lookup`）进行映射查找；字符串转换函数 `convert_gbk_to_utf8_lookup` 演示了如何把 GBK 字节流按双字节切分并查表再转成 UTF-8。

- **示例：在 C++ 中使用生成的表**：

  - 直接把脚本生成的 `gbk_unicode_data.cpp` 放到 `src/text/`（或其他源码目录）并加入到工程编译，随后在代码中：

    #include "gbk_unicode_table.h"

    // 把 GBK 原始字节串转换为 UTF-8
    std::string gbk_input = ...; // 包含 GBK 编码的字节
    std::string utf8 = convert_gbk_to_utf8_lookup(gbk_input);

    // 或者按单个 GBK 码查表并编码为 UTF-8
    uint16_t gbk_code = (high_byte << 8) | low_byte;
    uint16_t unicode = gbk_to_unicode_lookup(gbk_code);
    if (unicode) {
        uint8_t buf[4];
        int len = utf8_encode(unicode, buf);
        std::string s((char*)buf, len);
        // 将 s 追加到输出
    }

- **体积与性能注意**：
  - 完整表条目数较多（脚本会在 stderr 打印实际条目数），每项 4 字节，完整表通常会消耗若干十 KB 到上百 KB 的 Flash（脚本会给出估算）。
  - 表以只读 Flash（PROGMEM）方式存储，查表使用二分查找（O(log N)）读取 `pgm_read_word`，对实时性要求高的场景请注意查表开销；可考虑：
    - 仅生成并包含常用子集（`--simple` 或自定义裁剪）；
    - 在运行期缓存常用映射到 RAM（若 RAM 足够）。

- **集成建议**：
  - 将 `python tools/generate_gbk_table.py --full > src/text/gbk_unicode_data.cpp` 加入到构建前的生成步骤（例如 Makefile / pre-build 脚本），避免把生成后的大文件直接提交到版本库；或将其加入 `.gitignore` 并在 CI/构建环境中生成。 
  - 若需要减小 Flash 占用，可只生成常用字符子集或在固件中使用更轻量的替代（例如运行时使用 icon/替代符号）。

（结束）

**附录：繁简转换（gen_zh_table.py）**

- **脚本位置与作用**：`tools/gen_zh_table.py`。读取 `tools/zh_conv_table.csv`（4 列：Source,Target,SourceType,TargetType），提取繁体↔简体对并生成 C++ 源文件 `src/text/zh_conv_table_generated.cpp`，包含：
  - 一个字符串对数组 `zh_conv_pairs`（每项 `trad` / `simp`），
  - 两个按繁体与简体排序的索引数组 `zh_conv_indices_trad` / `zh_conv_indices_simp`，
  - 一个 BMP 范围内单字快速索引表 `zh_single_index`（O(1) 单字符查找），
  - 一个 C API `zh_conv_embedded_lookup(const char* key, uint8_t mode)`，mode=1 表示 繁->简，mode=2 表示 简->繁。

- **如何生成**：在仓库根目录下运行：

  ```bash
  # 生成并写入目标源码（覆盖/生成文件）
  python tools/gen_zh_table.py
  # 或显式指定输出文件（脚本默认输出到 src/text/zh_conv_table_generated.cpp）
  ```

  - 脚本会读取 `tools/zh_conv_table.csv` 并在 stdout/stderr 输出处理统计；最终在 `src/text/` 写入 `zh_conv_table_generated.cpp`。
  - 建议把该命令加入 pre-build 或 CI，以便在 CSV 更新后自动重建生成文件，避免把大文件直接手动维护。

- **输出文件要点**：
  - `zh_conv_pairs` 中每个字符串字面量为 UTF-8 编码的 C 字符串（`const char*`），通过索引数组按需二分查找以支持多字节串匹配。
  - `zh_single_index` 长度为 65536（BMP），对单个 Unicode codepoint 提供 O(1) 的 pair_index+1（0 表示无映射）。
  - `zh_conv_embedded_lookup` 接受 UTF-8 字符串 `key`，会先尝试单字符快查（若输入为单个 codepoint 且位于 BMP），否则在对应索引数组上做二分查找并返回匹配的 C 字符串指针（返回值为 `const char*`，指向生成文件中的常量字符串，调用方无需 free）。

- **在 C++ 中的使用示例**：

  - 简单的单字符转换示例（繁->简）：

    ```cpp
    #include "text/zh_conv_table_generated.cpp" // 或包含对应头/生成后的文件路径

    const char* out = zh_conv_embedded_lookup("漢", 1); // mode=1: trad->simp
    if (out) {
        Serial.printf("简体: %s\n", out);
    }
    ```

  - 将整段 UTF-8 文本逐字（或逐可能的短串）转换为目标（示例为繁->简）：

    ```cpp
    std::string convert_trad_to_simp(const std::string &in) {
        std::string out;
        // 简单策略：按 UTF-8 解码单个 codepoint，然后查表
        for (size_t i = 0; i < in.size();) {
            unsigned char c = (unsigned char)in[i];
            size_t clen = 1;
            if ((c & 0x80) == 0) clen = 1;
            else if ((c & 0xE0) == 0xC0) clen = 2;
            else if ((c & 0xF0) == 0xE0) clen = 3;
            else if ((c & 0xF8) == 0xF0) clen = 4;

            std::string key = in.substr(i, clen);
            const char* mapped = zh_conv_embedded_lookup(key.c_str(), 1);
            if (mapped) out.append(mapped);
            else out.append(key);

            i += clen;
        }
        return out;
    }
    ```

  - 若需要支持短语级别的替换（多个字组合成映射），可在 `zh_conv_pairs` 上做更复杂的前缀匹配或维护额外的短语索引，脚本当前以单字符串对为主，且对多字符键支持二分查找（通过 `strcmp` 比较完整字符串）。

- **性能与体积考虑**：
  - 完整表可能较大，`zh_single_index` 本身占用 256KB（65536 * 4 bytes）；脚本在生成时确实会将整个数组写入源码 —— 这会显著增加编译后的代码/Flash 占用。请务必检查生成文件大小并在必要时采取以下措施：
    - 只生成 `zh_single_index` 的子集或移除单字快速表（修改脚本以生成更小的快速表），
    - 仅包含常用映射（通过过滤 CSV 或使用 `--simple`/自定义策略），
    - 在运行时采用哈希表或基于 trie 的压缩表以减小存储（但会增加运行时开销）。

- **集成建议**：
  - 把 `tools/gen_zh_table.py` 的运行加入到构建前任务（或 CI），并把生成文件 `src/text/zh_conv_table_generated.cpp` 添加到构建系统。
  - 如果希望避免把长数组直接放入源码，可修改脚本改为生成二进制 lookup 文件并在运行时以只读方式从 SPIFFS/PROGMEM 加载，或在编译时转换为 `.rodata` 段外的二进制资源。

  - **字符集范围**
  


**字符集范围总结**
- **源码文件**: generateCharset.js — 导出 `buildCharset({includeGBK, includeTraditional, includeJapanese})`，返回排序且去重的 `Uint32Array`。
- **ASCII（基础）**: 包含可打印 ASCII：0x20–0x7E（空格到 ~）。
- **GBK（简体/通用）**:
  - 优先：若环境支持 `TextDecoder('gbk')`，按字节对枚举并解码（lead 0x81–0xFE，trail 两段 0x40–0x7E 和 0x80–0xFE），将解码出的非空白字符加入集合（分块解码以控制内存）。
  - 回退：无 `gbk` 支持时，近似地包含 CJK 基本块：0x4E00–0x9FFF 和 扩展A 0x3400–0x4DBF。
- **Big5（繁体）**:
  - 优先：若支持 `TextDecoder('big5')`，按 Big5 编码枚举并解码（lead 0xA1–0xFE，trail 0x40–0x7E 和 0xA1–0xFE），加入解码得到的字符。
  - 回退：包含 0x4E00–0x9FFF、0x3400–0x4DBF 以及 0xF900–0xFAFF（兼容/繁体相关区块）。
- **日文（可选）**: 若 `includeJapanese` 为真，显式加入常用日文/相关块：
  - 平假名 0x3040–0x309F
  - 片假名 0x30A0–0x30FF
  - 半角片假名 0xFF65–0xFF9F
  - Katakana 扩展 0x31F0–0x31FF
  - CJK 汉字 0x4E00–0x9FAF 与 扩展A 0x3400–0x4DBF
  - 其它兼容/圈字符区 0x3200–0x32FF、0x3300–0x33FF
- **特殊字符**: 明确加入 U+2022、U+25A1、U+FEFF（BOM）三项以保证必要符号存在。
- **过滤与去重**:
  - 解码时会忽略空白字符（用 `/\s/` 过滤）和解码错误。
  - 最后以 Set 去重并排序，返回 `Uint32Array`。
- **参数控制**: 三个布尔参数控制是否包含 GBK、Big5（繁体）、日文分块，便于在不同固件/渲染策略下调整。

