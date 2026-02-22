// MV3 service worker: listens for toolbar icon clicks and opens the extension blank page
chrome.action.onClicked.addListener((tab) => {
  // Open a new tab pointing to the bundled blank page
  chrome.tabs.create({
    url: chrome.runtime.getURL('pages/welcome.html')
  });
});

// Version update detection and notification
const STORAGE_KEY = 'readpaper_last_version';

/**
 * 检测版本更新
 */
async function checkVersionUpdate() {
  try {
    const manifest = chrome.runtime.getManifest();
    const currentVersion = manifest.version;
    
    // 从 storage 获取上次记录的版本
    const result = await chrome.storage.local.get(STORAGE_KEY);
    const lastVersion = result[STORAGE_KEY];
    
    console.log('[Background] 版本检测 - 当前:', currentVersion, '上次:', lastVersion);
    
    // 如果是新版本或首次安装
    if (isVersionUpdated(currentVersion, lastVersion)) {
      console.log('[Background] 检测到版本更新，打开更新页面');
      
      // 打开版本更新通知页面
      chrome.tabs.create({
        url: chrome.runtime.getURL('pages/version-update.html')
      });
      
      // 保存当前版本
      await chrome.storage.local.set({ [STORAGE_KEY]: currentVersion });
    }
  } catch (error) {
    console.error('[Background] 版本检测失败:', error);
  }
}

/**
 * 比较版本号
 */
function isVersionUpdated(current, last) {
  if (!last) return true; // 首次安装仍然视为更新

  // 仅比较版本号的前 3 段（major.minor.patch）
  const cur = getMajorMinorPatch(current);
  const prev = getMajorMinorPatch(last);

  for (let i = 0; i < 3; i++) {
    if (cur[i] !== prev[i]) return true;
  }
  return false;
}

/**
 * 提取版本号的前三段为整数数组，缺失或非数字部分按 0 处理
 * 例如: "5.0.3.1" -> [5,0,3]
 */
function getMajorMinorPatch(version) {
  if (!version) return [0, 0, 0];
  const parts = String(version).split('.');
  const out = [0, 0, 0];
  for (let i = 0; i < 3; i++) {
    const p = parts[i];
    if (!p) { out[i] = 0; continue; }
    const m = p.match(/^(\d+)/);
    out[i] = m ? parseInt(m[1], 10) : 0;
  }
  return out;
}

// 扩展安装或更新时检测
chrome.runtime.onInstalled.addListener((details) => {
  console.log('[Background] onInstalled:', details.reason);
  
  if (details.reason === 'install') {
    console.log('[Background] 首次安装');
    checkVersionUpdate();
  } else if (details.reason === 'update') {
    console.log('[Background] 扩展已更新');
    checkVersionUpdate();
  }
});

// 扩展启动时也检测一次（浏览器重启等场景）
chrome.runtime.onStartup.addListener(() => {
  console.log('[Background] 浏览器启动，检测版本更新');
  checkVersionUpdate();
});

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  // WebDAV 请求代理（绕过 CORS）
  if (message && message.type === 'webdav_fetch') {
    handleWebDAVFetch(message.url, message.options)
      .then(response => sendResponse({ ok: true, response }))
      .catch(error => sendResponse({ 
        ok: false, 
        error: error.message || String(error) 
      }));
    return true; // 保持消息通道开启
  }
  
  if (message && message.type === 'forceVersionCheck') {
    checkVersionUpdate()
      .then(() => sendResponse({ ok: true }))
      .catch((error) => {
        console.error('[Background] 手动检测失败:', error);
        sendResponse({ ok: false, error: error.message || String(error) });
      });
    return true;
  }
  return false;
});

/**
 * 在 background 中执行 fetch（不受 CORS 限制）
 * @param {string} url - 请求 URL
 * @param {object} options - fetch 选项
 * @returns {Promise<object>} 包含响应数据的对象
 */
async function handleWebDAVFetch(url, options = {}) {
  try {
    console.log('[Background WebDAV]', options.method || 'GET', url);
    
    // 确保有权限访问目标 origin
    try {
      const urlObj = new URL(url);
      const origin = urlObj.origin + "/*";
      
      const hasPermission = await new Promise((resolve) => {
        chrome.permissions.contains({ origins: [origin] }, (result) => {
          resolve(result);
        });
      });
      
      if (!hasPermission) {
        console.warn('[Background WebDAV] 缺少权限:', origin);
        throw new Error(`需要访问权限: ${origin}。请在页面中先授予权限。`);
      }
      
      console.log('[Background WebDAV] 权限检查通过:', origin);
    } catch (permError) {
      console.error('[Background WebDAV] 权限检查失败:', permError);
      throw permError;
    }
    
    // 处理 Blob body（从 base64 还原）
    let fetchOptions = { ...options };
    if (options._bodyIsBlob && options.body) {
      const base64 = options.body;
      const match = base64.match(/^data:([^;]+);base64,(.+)$/);
      if (match) {
        const mimeType = match[1];
        const base64Data = match[2];
        const byteCharacters = atob(base64Data);
        const byteNumbers = new Array(byteCharacters.length);
        for (let i = 0; i < byteCharacters.length; i++) {
          byteNumbers[i] = byteCharacters.charCodeAt(i);
        }
        const byteArray = new Uint8Array(byteNumbers);
        fetchOptions.body = new Blob([byteArray], { type: mimeType });
      }
      delete fetchOptions._bodyIsBlob;
    }
    
    const response = await fetch(url, fetchOptions);
    
    console.log('[Background WebDAV] 响应状态:', response.status, response.statusText);
    
    const contentType = response.headers.get('content-type') || '';
    console.log('[Background WebDAV] Content-Type:', contentType);
    console.log('[Background WebDAV] URL:', url);
    
    // 读取响应体
    let body = null;
    let bodyType = 'text';
    
    // 优先尝试以文本形式读取（适用于 JSON、文本和未知类型）
    // 只对明确的二进制类型使用 blob
    const isBinaryType = contentType.includes('image/') || 
                         contentType.includes('video/') || 
                         contentType.includes('audio/') ||
                         contentType.match(/application\/(pdf|zip|x-rar|x-tar|x-7z-compressed)/);
    
    console.log('[Background WebDAV] 是否二进制类型:', isBinaryType);
    
    if (isBinaryType) {
      // 明确的二进制类型
      bodyType = 'blob';
      const blob = await response.blob();
      console.log('[Background WebDAV] Blob 响应体大小:', blob.size);
      body = await blobToBase64(blob);
    } else {
      // 所有其他情况（包括 JSON、文本、application/octet-stream 等）都先尝试读取为文本
      body = await response.text();
      console.log('[Background WebDAV] 文本响应体大小:', body.length);
      console.log('[Background WebDAV] 文本响应预览:', body.substring(0, 100));
      
      // 如果 Content-Type 是 JSON 或内容看起来像 JSON，尝试解析
      if (contentType.includes('application/json') || body.trim().startsWith('{') || body.trim().startsWith('[')) {
        try {
          const parsed = JSON.parse(body);
          bodyType = 'json';
          body = parsed;
          console.log('[Background WebDAV] 成功解析为 JSON');
        } catch (e) {
          // 保持为文本
          bodyType = 'text';
          console.log('[Background WebDAV] 保持为文本（JSON解析失败或不是JSON）');
        }
      } else {
        bodyType = 'text';
      }
    }
    
    // 提取响应头
    const headers = {};
    response.headers.forEach((value, key) => {
      headers[key] = value;
    });
    
    const result = {
      ok: response.ok,
      status: response.status,
      statusText: response.statusText,
      headers: headers,
      body: body,
      bodyType: bodyType,
      url: response.url
    };
    
    console.log('[Background WebDAV] 返回结果: ok=%s, status=%d, bodyType=%s, bodyLength=%d', 
      result.ok, result.status, result.bodyType, 
      typeof body === 'string' ? body.length : JSON.stringify(body).length
    );
    
    return result;
  } catch (error) {
    console.error('[Background WebDAV] Fetch 失败:', error);
    throw error;
  }
}

/**
 * 将 Blob 转换为 Base64
 */
function blobToBase64(blob) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onloadend = () => resolve(reader.result);
    reader.onerror = reject;
    reader.readAsDataURL(blob);
  });
}

// Keep service worker alive briefly when needed (optional small heartbeat)
// Not strictly necessary for this minimal example.
self.addEventListener('install', (event) => {
  // Skip waiting so the service worker activates immediately during development
  self.skipWaiting();
});
