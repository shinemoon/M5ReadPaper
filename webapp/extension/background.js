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

// Keep service worker alive briefly when needed (optional small heartbeat)
// Not strictly necessary for this minimal example.
self.addEventListener('install', (event) => {
  // Skip waiting so the service worker activates immediately during development
  self.skipWaiting();
});
