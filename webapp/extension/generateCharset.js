// generateCharset.js
// 独立的 JS 字符集生成器，目标与 tools/generate_1bit_font_bin.py:build_charset 行为一致。
// 导出: buildCharset({includeGBK=true, includeTraditional=true, includeJapanese=false}) -> Uint32Array(codepoints)

function supportsEncoding(name) {
  try {
    new TextDecoder(name);
    return true;
  } catch (e) {
    return false;
  }
}

function pushRange(set, start, end) {
  for (let cp = start; cp <= end; cp++) set.add(cp);
}

export async function buildCharset({ includeGBK = true, includeTraditional = true, includeJapanese = false } = {}) {
  const set = new Set();

  // ASCII 可打印字符 0x20-0x7E
  pushRange(set, 0x20, 0x7E);

  // Helper: try to decode bytes with given encoding (gbk/big5)
  async function decodeBytes(leadRange, trailRanges, encoding) {
    if (!supportsEncoding(encoding)) return;
    const decoder = new TextDecoder(encoding);
    const bytes = [];
    for (let lead = leadRange[0]; lead <= leadRange[1]; lead++) {
      for (const tr of trailRanges) {
        for (let trail = tr[0]; trail <= tr[1]; trail++) {
          bytes.push(lead & 0xFF, trail & 0xFF);
        }
      }
    }
    // Decode in chunks to avoid huge arrays
    const CHUNK = 32 * 1024; // bytes per chunk (pairs -> count/2)
    for (let i = 0; i < bytes.length; i += CHUNK) {
      const slice = new Uint8Array(bytes.slice(i, i + CHUNK));
      try {
        const s = decoder.decode(slice);
        for (let ch of s) {
          if (ch && !ch.match(/\s/)) set.add(ch.codePointAt(0));
        }
      } catch (e) {
        // ignore decode errors
      }
    }
  }

  if (includeGBK) {
    // 尝试使用 TextDecoder('gbk') 恢复与 Python 相同的 GBK 解码
    if (supportsEncoding('gbk') || supportsEncoding('GBK')) {
      // lead: 0x81-0xFE, trail: 0x40-0x7E and 0x80-0xFE
      await decodeBytes([0x81, 0xFE], [[0x40, 0x7E], [0x80, 0xFE]], supportsEncoding('gbk') ? 'gbk' : 'GBK');
    } else {
      // 回退：包含常用 CJK 统一汉字范围作为近似
      pushRange(set, 0x4E00, 0x9FFF);
      pushRange(set, 0x3400, 0x4DBF);
    }
  }

  if (includeTraditional) {
    if (supportsEncoding('big5') || supportsEncoding('Big5')) {
      // Big5 lead: 0xA1-0xFE, trail: 0x40-0x7E and 0xA1-0xFE
      await decodeBytes([0xA1, 0xFE], [[0x40, 0x7E], [0xA1, 0xFE]], supportsEncoding('big5') ? 'big5' : 'Big5');
    } else {
      // 回退：常见繁体汉字 Unicode 范围
      pushRange(set, 0x4E00, 0x9FFF);
      pushRange(set, 0x3400, 0x4DBF);
      pushRange(set, 0xF900, 0xFAFF);
    }
  }

  if (includeJapanese) {
    // 日文相关 Unicode 范围
    pushRange(set, 0x3040, 0x309F); // 平假名
    pushRange(set, 0x30A0, 0x30FF); // 片假名
    pushRange(set, 0x4E00, 0x9FAF); // CJK 汉字
    pushRange(set, 0x3400, 0x4DBF); // 扩展A
    pushRange(set, 0xFF65, 0xFF9F); // 半角片假名
    pushRange(set, 0x31F0, 0x31FF);
    pushRange(set, 0x3200, 0x32FF);
    pushRange(set, 0x3300, 0x33FF);
  }

  // 特殊字符
  set.add(0x2022);
  set.add(0x25A1);
  set.add(0xFEFF);

  // 转为排序数组
  const arr = Array.from(set);
  arr.sort((a, b) => a - b);
  
  // 过滤掉BMP之外的字符 (U+10000及以上)
  // 因为.bin文件使用uint16存储码点，补充平面字符会被截断导致冲突
  const bmpOnly = arr.filter(cp => cp <= 0xFFFF);
  
  return new Uint32Array(bmpOnly);
}

export default buildCharset;
