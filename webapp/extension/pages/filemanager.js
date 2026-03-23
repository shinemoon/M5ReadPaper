(function(){
  const API_BASE = 'http://192.168.4.1'; // 若在同域热点内访问，可留空改为 ''
  const GUIDE_ENDPOINT = `${API_BASE}/api/device_guide`;
  const PAGE_SIZE = 10;
  const DEBUG_FLAG_KEY = 'device_mgmt_debug';

  function isTruthyFlag(v){
    if(v == null) return false;
    const s = String(v).trim().toLowerCase();
    return s === '1' || s === 'true' || s === 'yes' || s === 'on';
  }

  function computeDebugMode(){
    let fromQuery = null;
    try {
      const q = new URLSearchParams(window.location.search || '');
      if(q.has('debug')) fromQuery = q.get('debug');
      if(q.has('dm_debug')) fromQuery = q.get('dm_debug');
      if(q.has('device_debug')) fromQuery = q.get('device_debug');
      if(q.has('debug_off') && isTruthyFlag(q.get('debug_off'))) fromQuery = '0';
    } catch(_){ }

    if(fromQuery != null){
      const on = isTruthyFlag(fromQuery);
      try { localStorage.setItem(DEBUG_FLAG_KEY, on ? '1' : '0'); } catch(_){ }
      return on;
    }

    try {
      return isTruthyFlag(localStorage.getItem(DEBUG_FLAG_KEY));
    } catch(_){
      return false;
    }
  }

  const DEBUG_MODE = computeDebugMode();
  let currentCat = '';
  let currentPage = 1;
  let currentBookSubdir = ''; // current subdirectory path within /book (e.g. '' or 'manga' or 'manga/sub')
  let cache = {};
  let selectedFiles = [];
  let selectedScbackFile = null;
  let selectedForDelete = new Set();
  let moduleMode = 'file';
  let deviceGuide = null;
  let tabConfigs = [];

  const debugGuide = {
    ok: true,
    schema_version: 1,
    sections: {
      file_management: true,
      time_management: true,
      settings_management: true,
    },
    fileManagement: {
      required: true,
      tabs: [
        { id:'book', apiTab:'book', title:'书籍', hint:'[DEBUG] 书籍测试向量：目录、长文件名、当前阅读标记。', supportsHierarchy:true, allowUpload:true, allowDelete:true, allowRename:true, allowMkdir:true, allowReadingRecords:true, showIdxBadge:true },
        { id:'font', apiTab:'font', title:'字体', hint:'[DEBUG] 字体测试向量：当前字体与多文件状态。', supportsHierarchy:false, allowUpload:true, allowDelete:true, allowRename:false, allowMkdir:false, allowReadingRecords:false, showIdxBadge:false },
        { id:'image', apiTab:'image', title:'锁屏', hint:'[DEBUG] 锁屏图片测试向量。', supportsHierarchy:false, allowUpload:true, allowDelete:true, allowRename:false, allowMkdir:false, allowReadingRecords:false, showIdxBadge:false },
        { id:'screenshot', apiTab:'screenshot', title:'截图', hint:'[DEBUG] 截图测试向量，可验证批量选择与删除。', supportsHierarchy:false, allowUpload:false, allowDelete:true, allowRename:false, allowMkdir:false, allowReadingRecords:false, showIdxBadge:false, supportsScback:true },
      ]
    },
    timeManagement: { enabled:true, allowSyncTime:true },
    settingsManagement: { enabled:true, allowWifiConfig:true, allowWebdavConfig:true },
  };

  const debugState = {
    files: {
      book: [
        { path:'/book/科幻', type:'dir' },
        { path:'/book/技术', type:'dir' },
        { path:'/book/今日速读-用于验证超长文件名截断和布局稳定性的测试样本_2026_03_23_版本A.txt', type:'file', size: 38124, isCurrent:false, isIdxed:false },
        { path:'/book/正在阅读样例.txt', type:'file', size: 94812, isCurrent:true, isIdxed:true },
        { path:'/book/科幻/基地.txt', type:'file', size: 820331, isCurrent:false, isIdxed:true },
        { path:'/book/科幻/沙丘.txt', type:'file', size: 1219033, isCurrent:false, isIdxed:true },
        { path:'/book/技术/嵌入式UI调试实践.txt', type:'file', size: 232910, isCurrent:false, isIdxed:false },
      ],
      font: [
        { path:'/font/font.bin', type:'file', size: 1048576, isCurrent:true },
        { path:'/font/font_cn_16.bin', type:'file', size: 780123, isCurrent:false },
      ],
      image: [
        { path:'/image/default.png', type:'file', size: 238001, isCurrent:true },
        { path:'/image/mountain.png', type:'file', size: 532100, isCurrent:false },
      ],
      screenshot: [
        { path:'/screenshot/screen_001.png', type:'file', size: 201212, isCurrent:false },
        { path:'/screenshot/screen_002.png', type:'file', size: 199002, isCurrent:false },
        { path:'/screenshot/screen_003.png', type:'file', size: 211405, isCurrent:false },
      ],
    },
    wifiConfigs: [
      { ssid:'Office-WiFi', password:'12345678' },
      { ssid:'Home-WiFi-5G', password:'password-2' },
      { ssid:'MobileHotspot', password:'password-3' },
    ],
    webdavConfig: {
      url:'https://debug.example.com/dav/',
      username:'debug_user',
      password:'debug_pass',
    },
    readingRecords: [
      { bookname:'正在阅读样例.txt', total_hours:12, total_minutes:44, hourly_records:{'2026032209':32,'2026032310':18} },
      { bookname:'基地.txt', total_hours:5, total_minutes:20, hourly_records:{'2026032215':20,'2026032311':40} },
    ],
  };

  function debugNormalizePath(path){
    if(!path) return '/';
    let p = String(path).replace(/\\/g, '/').trim();
    if(!p.startsWith('/')) p = '/' + p;
    p = p.replace(/\/+/g, '/');
    if(p.length > 1 && p.endsWith('/')) p = p.slice(0, -1);
    return p;
  }

  function debugBasename(path){
    const p = debugNormalizePath(path);
    const idx = p.lastIndexOf('/');
    return idx >= 0 ? p.slice(idx + 1) : p;
  }

  function debugGetList(tab){
    if(!debugState.files[tab]) debugState.files[tab] = [];
    return debugState.files[tab];
  }

  function debugBuildListPayload(apiTab, subdir, page, perPage){
    const list = debugGetList(apiTab).map(x=>({ ...x, path: debugNormalizePath(x.path) }));
    const root = '/' + apiTab;
    const parent = (apiTab === 'book' && subdir) ? debugNormalizePath(root + '/' + subdir) : root;
    const seenDirs = new Set();
    const out = [];

    for(const item of list){
      const p = debugNormalizePath(item.path);
      if(!p.startsWith(parent + '/') && p !== parent) continue;
      if(p === parent) continue;
      const rest = p.slice(parent.length + 1);
      if(!rest) continue;
      const slash = rest.indexOf('/');
      if(slash === -1){
        if(item.type === 'dir'){
          if(!seenDirs.has(p)){
            seenDirs.add(p);
            out.push({ name: debugBasename(p), type:'dir', path:p, isCurrent:false, isIdxed:false, size:0 });
          }
        } else {
          out.push({
            name: debugBasename(p),
            type:'file',
            path:p,
            size:Number(item.size || 0),
            isCurrent:!!item.isCurrent,
            isIdxed:!!item.isIdxed,
          });
        }
      } else if(apiTab === 'book'){
        const childDir = debugNormalizePath(parent + '/' + rest.slice(0, slash));
        if(!seenDirs.has(childDir)){
          seenDirs.add(childDir);
          out.push({ name: debugBasename(childDir), type:'dir', path:childDir, isCurrent:false, isIdxed:false, size:0 });
        }
      }
    }

    out.sort((a,b)=>{
      if(a.type !== b.type) return a.type === 'dir' ? -1 : 1;
      return String(a.name).localeCompare(String(b.name), 'zh-CN');
    });

    const total = out.length;
    const start = (Math.max(1, page) - 1) * Math.max(1, perPage);
    const files = out.slice(start, start + perPage);
    return { total, page: Math.max(1, page), perPage: Math.max(1, perPage), files };
  }

  function debugDeletePath(path){
    const p = debugNormalizePath(path);
    Object.keys(debugState.files).forEach(tab=>{
      const list = debugGetList(tab);
      debugState.files[tab] = list.filter(item=>{
        const ip = debugNormalizePath(item.path);
        if(ip === p) return false;
        if(ip.startsWith(p + '/')) return false;
        return true;
      });
    });
  }

  function debugRenamePath(oldPath, newName){
    const oldP = debugNormalizePath(oldPath);
    const oldBase = debugBasename(oldP);
    const parent = oldP.slice(0, oldP.length - oldBase.length);
    const newBase = String(newName || '').trim();
    if(!newBase) throw new Error('new_name empty');
    const newP = debugNormalizePath(parent + newBase);

    Object.keys(debugState.files).forEach(tab=>{
      const list = debugGetList(tab);
      list.forEach(item=>{
        const ip = debugNormalizePath(item.path);
        if(ip === oldP) item.path = newP;
        else if(ip.startsWith(oldP + '/')) item.path = newP + ip.slice(oldP.length);
      });
    });
  }

  function debugMkdir(path){
    const p = debugNormalizePath(path);
    const tab = p.split('/')[1];
    const list = debugGetList(tab);
    if(list.some(x=> debugNormalizePath(x.path) === p)) return;
    list.push({ path:p, type:'dir' });
  }

  function debugAddUploadedFile(file, tab, subdir){
    const safeName = (file && file.name ? file.name : 'debug-upload.txt').replace(/[\\/]/g, '_');
    const root = tab === 'scback' ? '/' : `/${tab}`;
    if(tab === 'scback') return;
    const dir = subdir ? debugNormalizePath(root + '/' + subdir) : root;
    const path = debugNormalizePath(dir + '/' + safeName);
    const list = debugGetList(tab);
    const existing = list.find(x=> debugNormalizePath(x.path) === path);
    if(existing){
      existing.type = 'file';
      existing.size = Number(file && file.size ? file.size : 1024);
      existing.isCurrent = false;
    } else {
      list.push({ path, type:'file', size:Number(file && file.size ? file.size : 1024), isCurrent:false, isIdxed:false });
    }
  }

  function debugCreateResponse(body, status, contentType){
    return new Response(body, {
      status: status || 200,
      headers: { 'Content-Type': contentType || 'application/json' },
    });
  }

  function debugJson(data, status){
    return debugCreateResponse(JSON.stringify(data), status || 200, 'application/json');
  }

  function debugText(data, status){
    return debugCreateResponse(String(data || ''), status || 200, 'text/plain');
  }

  async function debugHandleRequest(urlObj, method, init){
    const path = urlObj.pathname;
    const q = urlObj.searchParams;

    if(path === '/api/device_guide') return debugJson(debugGuide);
    if(path === '/heartbeat') return debugJson({ status:'ok', hw:'PaperS3-Debug', firmware:'ReadPaper', version:'DEBUG-UI' });

    if(path === '/api/wifi_config'){
      if(method === 'GET') return debugJson({ ok:true, configs: debugState.wifiConfigs, last_success_idx: 0 });
      if(method === 'POST'){
        let body = {};
        try { body = JSON.parse((init && init.body) || '{}'); } catch(_){ }
        if(Array.isArray(body.configs)) debugState.wifiConfigs = body.configs.slice(0,3);
        return debugJson({ ok:true, configs: debugState.wifiConfigs, last_success_idx: 0 });
      }
    }

    if(path === '/api/webdav_config'){
      if(method === 'GET') return debugJson({ ok:true, config: debugState.webdavConfig });
      if(method === 'POST'){
        let body = {};
        try { body = JSON.parse((init && init.body) || '{}'); } catch(_){ }
        const cfg = body.config || body;
        debugState.webdavConfig = {
          url: cfg.url || '',
          username: cfg.username || '',
          password: cfg.password || '',
        };
        return debugJson({ ok:true, config: debugState.webdavConfig });
      }
    }

    if(path === '/sync_time' && method === 'POST') return debugText('Time synced (debug mock)', 200);

    if(path === '/api/reading_records' && method === 'GET'){
      const book = q.get('book');
      const books = q.get('books');
      let records = debugState.readingRecords;
      if(book){
        const n = debugBasename(book);
        records = records.filter(r=> String(r.bookname || '').includes(n));
      } else if(books){
        const names = books.split(',').map(x=> debugBasename(x));
        records = records.filter(r=> names.includes(String(r.bookname || '')));
      }
      return debugJson({ ok:true, records });
    }

    if(path.startsWith('/list') && method === 'GET'){
      const seg = path.split('/').filter(Boolean);
      const apiTab = seg[1] || 'book';
      const page = Number(q.get('page') || 1);
      const perPage = Number(q.get('perPage') || PAGE_SIZE);
      const subdir = q.get('subdir') || '';
      return debugJson(debugBuildListPayload(apiTab, subdir, page, perPage));
    }

    if(path === '/delete' && method === 'GET'){
      const p = q.get('path');
      if(!p) return debugJson({ ok:false, message:'missing path' }, 400);
      debugDeletePath(p);
      return debugJson({ ok:true });
    }

    if(path === '/rename' && method === 'GET'){
      const oldPath = q.get('old_path');
      const newName = q.get('new_name');
      if(!oldPath || !newName) return debugJson({ ok:false, message:'missing params' }, 400);
      debugRenamePath(oldPath, newName);
      return debugJson({ ok:true });
    }

    if(path === '/mkdir' && method === 'GET'){
      const p = q.get('path');
      if(!p) return debugJson({ ok:false, message:'missing path' }, 400);
      debugMkdir(p);
      return debugJson({ ok:true });
    }

    if(path === '/download' && method === 'GET'){
      const p = q.get('path') || '/debug.txt';
      return debugText('DEBUG DOWNLOAD\n' + p + '\n', 200);
    }

    return debugJson({ ok:false, message:`debug api not mocked: ${path}` }, 404);
  }

  (function installDebugFetchInterceptor(){
    if(!DEBUG_MODE) return;
    const realFetch = window.fetch.bind(window);
    const apiOrigin = new URL(API_BASE).origin;
    window.fetch = async function(input, init){
      const reqUrl = typeof input === 'string' ? input : (input && input.url);
      if(!reqUrl) return realFetch(input, init);
      let urlObj;
      try {
        urlObj = new URL(reqUrl, window.location.origin);
      } catch(_){
        return realFetch(input, init);
      }
      const method = String((init && init.method) || (typeof input !== 'string' && input && input.method) || 'GET').toUpperCase();
      if(urlObj.origin === apiOrigin){
        return debugHandleRequest(urlObj, method, init);
      }
      return realFetch(input, init);
    };
  })();
  
  // 后端分页支持检测（首次请求时自动检测）
  let paginationSupported = null; // null=未检测, true=支持, false=不支持

  // IndexedDB configuration for local reading records storage
  const DB_NAME = 'readpaper_data_center';
  const DB_VERSION = 1;
  const STORE = 'reading_records';

  const el = id=>document.getElementById(id);
  const fileBody = el('fileBody');
  const pager = el('pager');
  const pageInfo = el('pageInfo');
  const prevBtn = el('prevBtn');
  const nextBtn = el('nextBtn');
  const btnUpload = el('btnUpload');
  const btnSelect = el('btnSelect');
  const fileInput = el('fileInput');
  const uploadStatus = el('uploadStatus');
  const uploadTitle = el('uploadTitle');
  const hint = el('hint');
  const btnSyncTime = el('btnSyncTime');
  const uploadBox = el('uploadBox');
  const scbackBox = el('scbackBox');
  const btnScbackSelect = el('btnScbackSelect');
  const btnScbackUpload = el('btnScbackUpload');
  const btnScbackDelete = el('btnScbackDelete');
  const scbackFileInput = el('scbackFileInput');
  const scbackInfo = el('scbackInfo');
  const btnMkdir = el('btnMkdir');
  const catTabItems = el('catTabItems');
  const fileSection = el('fileSection');
  const timeSection = el('timeSection');
  const settingsSection = el('settingsSection');
  const tabFileManagement = el('tabFileManagement');
  const tabTimeManagement = el('tabTimeManagement');
  const tabSettingsManagement = el('tabSettingsManagement');
  const wifiSettingsCard = el('wifiSettingsCard');
  const webdavSettingsCard = el('webdavSettingsCard');
  const settingsStatus = el('settingsStatus');
  const wifiSsid0 = el('wifiSsid0');
  const wifiPass0 = el('wifiPass0');
  const wifiSsid1 = el('wifiSsid1');
  const wifiPass1 = el('wifiPass1');
  const wifiSsid2 = el('wifiSsid2');
  const wifiPass2 = el('wifiPass2');
  const btnLoadWifiSettings = el('btnLoadWifiSettings');
  const btnSaveWifiSettings = el('btnSaveWifiSettings');
  const webdavUrl = el('webdavUrl');
  const webdavUsername = el('webdavUsername');
  const webdavPassword = el('webdavPassword');
  const btnLoadWebdavSettings = el('btnLoadWebdavSettings');
  const btnSaveWebdavSettings = el('btnSaveWebdavSettings');
  const recBox = el('recBox');

  const defaultGuide = {
    sections: {
      file_management: true,
      time_management: true,
      settings_management: true,
    },
    fileManagement: {
      required: true,
      tabs: [
        { id:'book', apiTab:'book', title:'书籍', hint:'支持 unicode/GBK 编码的 txt 文件。', supportsHierarchy:true, allowUpload:true, allowDelete:true, allowRename:true, allowMkdir:true, allowReadingRecords:true, showIdxBadge:true },
        { id:'font', apiTab:'font', title:'字体', hint:'请上传工具生成的 font.bin。', supportsHierarchy:false, allowUpload:true, allowDelete:true, allowRename:false, allowMkdir:false, allowReadingRecords:false, showIdxBadge:false },
        { id:'image', apiTab:'image', title:'锁屏', hint:'锁屏图片建议 540x960，支持透明 png。', supportsHierarchy:false, allowUpload:true, allowDelete:true, allowRename:false, allowMkdir:false, allowReadingRecords:false, showIdxBadge:false },
        { id:'screenshot', apiTab:'screenshot', title:'截图', hint:'设备截图存储目录。', supportsHierarchy:false, allowUpload:false, allowDelete:true, allowRename:false, allowMkdir:false, allowReadingRecords:false, showIdxBadge:false, supportsScback:true },
      ]
    },
    timeManagement: { enabled:true, allowSyncTime:true },
    settingsManagement: { enabled:true, allowWifiConfig:true, allowWebdavConfig:true },
  };

  function toast(msg, type='info', ms=3000){
    const box = document.getElementById('toasts');
    if(!box) return; 
    const t = document.createElement('div');
    t.className='toast';
    t.style.background = type==='error'? '#b30000' : type==='success'? '#2f6e2f' : '#222';
    t.textContent = msg;
    box.appendChild(t);
    setTimeout(()=>{ if(t.parentNode===box) box.removeChild(t); }, ms);
  }

  // 统一使用全局 window.niceConfirm (modal_confirm.js 提供)，这里提供轻量包装以兼容旧调用
  const showConfirm = (message, title='请确认') => {
    if(typeof window.niceConfirm === 'function') return window.niceConfirm(message,{title});
    return Promise.resolve(window.confirm ? window.confirm(message) : false);
  };

  function currentTabConfig(){
    return tabConfigs.find(t => t.id === currentCat) || null;
  }

  function currentApiTab(){
    const cfg = currentTabConfig();
    return (cfg && cfg.apiTab) ? cfg.apiTab : currentCat;
  }

  function supportsHierarchy(){
    const cfg = currentTabConfig();
    return !!(cfg && cfg.supportsHierarchy);
  }

  function canReadRecords(){
    const cfg = currentTabConfig();
    return !!(cfg && cfg.allowReadingRecords);
  }

  function canRename(){
    const cfg = currentTabConfig();
    return !!(cfg && cfg.allowRename);
  }

  function canMkdir(){
    const cfg = currentTabConfig();
    return !!(cfg && cfg.allowMkdir);
  }

  function canUpload(){
    const cfg = currentTabConfig();
    return !!(cfg && cfg.allowUpload);
  }

  function supportsScback(){
    const cfg = currentTabConfig();
    return !!(cfg && cfg.supportsScback);
  }

  function hasIdxBadge(){
    const cfg = currentTabConfig();
    return !!(cfg && cfg.showIdxBadge);
  }

  function setModuleMode(mode){
    moduleMode = mode;
    if(fileSection) fileSection.style.display = mode === 'file' ? '' : 'none';
    if(timeSection) timeSection.style.display = mode === 'time' ? '' : 'none';
    if(settingsSection) settingsSection.style.display = mode === 'settings' ? '' : 'none';

    if(tabFileManagement) tabFileManagement.classList.toggle('active', mode === 'file');
    if(tabTimeManagement) tabTimeManagement.classList.toggle('active', mode === 'time');
    if(tabSettingsManagement) tabSettingsManagement.classList.toggle('active', mode === 'settings');
  }

  function setSettingsStatus(text, isError=false){
    if(!settingsStatus) return;
    settingsStatus.textContent = text || '';
    settingsStatus.style.color = isError ? '#b30000' : '';
  }

  function setWifiForm(configs){
    const list = Array.isArray(configs) ? configs : [];
    const c0 = list[0] || {};
    const c1 = list[1] || {};
    const c2 = list[2] || {};
    if(wifiSsid0) wifiSsid0.value = c0.ssid || '';
    if(wifiPass0) wifiPass0.value = c0.password || '';
    if(wifiSsid1) wifiSsid1.value = c1.ssid || '';
    if(wifiPass1) wifiPass1.value = c1.password || '';
    if(wifiSsid2) wifiSsid2.value = c2.ssid || '';
    if(wifiPass2) wifiPass2.value = c2.password || '';
  }

  function getWifiFormConfigs(){
    return [
      { ssid: (wifiSsid0 && wifiSsid0.value || '').trim(), password: (wifiPass0 && wifiPass0.value || '').trim() },
      { ssid: (wifiSsid1 && wifiSsid1.value || '').trim(), password: (wifiPass1 && wifiPass1.value || '').trim() },
      { ssid: (wifiSsid2 && wifiSsid2.value || '').trim(), password: (wifiPass2 && wifiPass2.value || '').trim() },
    ];
  }

  function setWebdavForm(config){
    const c = config || {};
    if(webdavUrl) webdavUrl.value = c.url || '';
    if(webdavUsername) webdavUsername.value = c.username || '';
    if(webdavPassword) webdavPassword.value = c.password || '';
  }

  async function loadWifiSettings(){
    const r = await fetch(`${API_BASE}/api/wifi_config`, { cache: 'no-store' });
    if(!r.ok) throw new Error(`HTTP ${r.status}`);
    const j = await r.json();
    if(!j || j.ok === false) throw new Error((j && j.message) || '加载 WiFi 配置失败');
    setWifiForm(j.configs || []);
  }

  async function saveWifiSettings(){
    const payload = { configs: getWifiFormConfigs() };
    const r = await fetch(`${API_BASE}/api/wifi_config`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    const j = await r.json();
    if(!r.ok || !j || j.ok === false) throw new Error((j && j.message) || `HTTP ${r.status}`);
    setWifiForm(j.configs || payload.configs);
  }

  async function loadWebdavSettings(){
    const r = await fetch(`${API_BASE}/api/webdav_config`, { cache: 'no-store' });
    if(!r.ok) throw new Error(`HTTP ${r.status}`);
    const j = await r.json();
    if(!j || j.ok === false) throw new Error((j && j.message) || '加载 WebDAV 配置失败');
    setWebdavForm(j.config || {});
  }

  async function saveWebdavSettings(){
    const payload = {
      config: {
        url: (webdavUrl && webdavUrl.value || '').trim(),
        username: (webdavUsername && webdavUsername.value || '').trim(),
        password: (webdavPassword && webdavPassword.value || '').trim(),
      }
    };
    const r = await fetch(`${API_BASE}/api/webdav_config`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    const j = await r.json();
    if(!r.ok || !j || j.ok === false) throw new Error((j && j.message) || `HTTP ${r.status}`);
    setWebdavForm(j.config || payload.config);
  }

  async function initializeSettingsData(){
    try {
      await Promise.all([loadWifiSettings(), loadWebdavSettings()]);
      setSettingsStatus('设置已同步。');
    } catch(e){
      setSettingsStatus('设置加载失败: ' + (e.message || e), true);
    }
  }

  function applyModuleAvailability(){
    const sec = (deviceGuide && deviceGuide.sections) ? deviceGuide.sections : defaultGuide.sections;
    const canFile = !!sec.file_management;
    const canTime = !!sec.time_management;
    const canSettings = !!sec.settings_management;

    if(tabFileManagement) tabFileManagement.style.display = canFile ? '' : 'none';
    if(tabTimeManagement) tabTimeManagement.style.display = canTime ? '' : 'none';
    if(tabSettingsManagement) tabSettingsManagement.style.display = canSettings ? '' : 'none';

    if(!canFile && moduleMode === 'file') moduleMode = canTime ? 'time' : (canSettings ? 'settings' : 'file');
    if(!canTime && moduleMode === 'time') moduleMode = canFile ? 'file' : (canSettings ? 'settings' : 'time');
    if(!canSettings && moduleMode === 'settings') moduleMode = canFile ? 'file' : (canTime ? 'time' : 'settings');
    setModuleMode(moduleMode);

    const sm = (deviceGuide && deviceGuide.settingsManagement) ? deviceGuide.settingsManagement : defaultGuide.settingsManagement;
    if(wifiSettingsCard) wifiSettingsCard.style.display = sm.allowWifiConfig ? '' : 'none';
    if(webdavSettingsCard) webdavSettingsCard.style.display = sm.allowWebdavConfig ? '' : 'none';

    const tm = (deviceGuide && deviceGuide.timeManagement) ? deviceGuide.timeManagement : defaultGuide.timeManagement;
    if(btnSyncTime) btnSyncTime.style.display = tm.allowSyncTime ? '' : 'none';
  }

  function renderFileTabs(){
    if(!catTabItems) return;
    catTabItems.innerHTML = tabConfigs.map((tab, idx)=>{
      const active = idx === 0 ? 'active' : '';
      return `<a data-cat="${tab.id}" class="${active}">${tab.title || tab.id}</a>`;
    }).join('');
    document.querySelectorAll('#catTabItems a[data-cat]').forEach(a=>{
      a.onclick = ()=> switchCat(a.dataset.cat);
    });
  }

  function applyDebugBadge(){
    if(!DEBUG_MODE) return;
    try {
      document.body.classList.add('dm-debug-mode');
      const devEl = document.getElementById('device-info');
      if(devEl){
        const tip = 'DEBUG MODE: 使用本地测试向量（离线可调试）';
        if(!devEl.textContent || !devEl.textContent.includes('DEBUG MODE')){
          devEl.textContent = tip;
        }
        devEl.style.color = '#b45309';
        devEl.style.fontWeight = '700';
      }
    } catch(_){ }
  }

  async function fetchDeviceGuide(){
    try {
      if(DEBUG_MODE){
        return debugGuide;
      }
      if(window.deviceGuideCache && window.deviceGuideCache.guide){
        return window.deviceGuideCache.guide;
      }
      const r = await fetch(GUIDE_ENDPOINT, { cache: 'no-store' });
      if(!r.ok) throw new Error(`HTTP ${r.status}`);
      const j = await r.json();
      if(!j || j.ok === false) throw new Error('invalid guide payload');
      return j;
    } catch(e){
      console.warn('[device_guide] fallback to default guide:', e.message || e);
      return defaultGuide;
    }
  }

  async function initializeDeviceGuide(){
    deviceGuide = await fetchDeviceGuide();
    const fm = (deviceGuide && deviceGuide.fileManagement) ? deviceGuide.fileManagement : defaultGuide.fileManagement;
    tabConfigs = (fm.tabs && fm.tabs.length) ? fm.tabs : defaultGuide.fileManagement.tabs;
    if(DEBUG_MODE){
      window.deviceGuideCache = window.deviceGuideCache || { guide:null, fetchedAt:0 };
      window.deviceGuideCache.guide = deviceGuide;
      window.deviceGuideCache.fetchedAt = Date.now();
    }
    renderFileTabs();
    applyModuleAvailability();
  }

  // IndexedDB helper functions
  function openDB() {
    return new Promise((resolve, reject) => {
      const req = indexedDB.open(DB_NAME, DB_VERSION);
      req.onerror = (e) => reject(e.target.error || e);
      req.onupgradeneeded = (e) => {
        const db = e.target.result;
        if (!db.objectStoreNames.contains(STORE)) {
          const os = db.createObjectStore(STORE, { keyPath: 'id', autoIncrement: true });
          os.createIndex('bookname', 'bookname', { unique: false });
          os.createIndex('timestamp', 'timestamp', { unique: false });
        }
      };
      req.onsuccess = (e) => resolve(e.target.result);
    });
  }

  async function mergeRecordToDB(record) {
    const db = await openDB();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, 'readwrite');
      const store = tx.objectStore(STORE);
      const index = store.index('bookname');
      
      // Normalize record
      const bookname = record.bookname || record.book_name || (record.book_path ? record.book_path.split('/').pop() : '');
      if (!bookname) {
        console.log('[mergeRecordToDB] Skipping record with no bookname:', record);
        resolve(); // Skip if no valid bookname
        return;
      }
      
      const newRecord = {
        book_path: record.book_path || `/sd/book/${bookname}`,
        book_name: bookname,
        bookname: bookname,
        total_hours: record.total_hours || 0,
        total_minutes: record.total_minutes || 0,
        hourly_records: record.hourly_records || {},
        daily_summary: record.daily_summary || {},
        monthly_summary: record.monthly_summary || {},
        timestamp: record.timestamp || Date.now()
      };
      
      console.log('[mergeRecordToDB] Processing:', bookname);
      
      // Check if record exists
      const getReq = index.get(bookname);
      getReq.onsuccess = () => {
        const existing = getReq.result;
        if (existing) {
          console.log('[mergeRecordToDB] Found existing record, merging data for:', bookname);
          
          // Merge hourly_records: for each hour, keep the maximum value (capped at 60)
          const mergedHourly = {...(existing.hourly_records || {})};
          for (const [hour, minutes] of Object.entries(newRecord.hourly_records || {})) {
            const existingMin = mergedHourly[hour] || 0;
            const newMin = Math.min(Number(minutes) || 0, 60); // Cap at 60
            mergedHourly[hour] = Math.max(existingMin, newMin);
          }
          
          // Merge daily_summary: sum up all hours for each day
          const mergedDaily = {};
          const allDays = new Set([
            ...Object.keys(existing.daily_summary || {}),
            ...Object.keys(newRecord.daily_summary || {})
          ]);
          
          for (const day of allDays) {
            // Calculate from hourly records for this day
            let dayTotal = 0;
            for (const [hour, minutes] of Object.entries(mergedHourly)) {
              if (hour.startsWith(day)) {
                dayTotal += Number(minutes) || 0;
              }
            }
            if (dayTotal > 0) {
              mergedDaily[day] = dayTotal;
            }
          }
          
          // Merge monthly_summary: sum up all days for each month
          const mergedMonthly = {};
          for (const [day, minutes] of Object.entries(mergedDaily)) {
            const month = day.substring(0, 6); // YYYYMM
            mergedMonthly[month] = (mergedMonthly[month] || 0) + minutes;
          }
          
          // Calculate total time from merged data
          const totalMinutes = Object.values(mergedHourly).reduce((sum, min) => sum + (Number(min) || 0), 0);
          const totalHours = Math.floor(totalMinutes / 60);
          const remainingMinutes = totalMinutes % 60;
          
          const mergedRecord = {
            id: existing.id,
            book_path: existing.book_path || newRecord.book_path,
            book_name: bookname,
            bookname: bookname,
            total_hours: totalHours,
            total_minutes: remainingMinutes,
            hourly_records: mergedHourly,
            daily_summary: mergedDaily,
            monthly_summary: mergedMonthly,
            timestamp: Date.now()
          };
          
          console.log('[mergeRecordToDB] Merged totals:', bookname, 
            'hours:', totalHours, 'minutes:', remainingMinutes,
            'hourly entries:', Object.keys(mergedHourly).length);
          
          const putReq = store.put(mergedRecord);
          putReq.onsuccess = () => {
            console.log('[mergeRecordToDB] Successfully merged and updated:', bookname);
            resolve();
          };
          putReq.onerror = (e) => {
            console.error('[mergeRecordToDB] Put error:', bookname, e.target.error || e);
            reject(e.target.error || e);
          };
        } else {
          // Add new record - ensure hourly records don't exceed 60 minutes
          const cappedHourly = {};
          for (const [hour, minutes] of Object.entries(newRecord.hourly_records || {})) {
            cappedHourly[hour] = Math.min(Number(minutes) || 0, 60);
          }
          newRecord.hourly_records = cappedHourly;
          
          console.log('[mergeRecordToDB] Adding new record:', bookname);
          const addReq = store.add(newRecord);
          addReq.onsuccess = () => {
            console.log('[mergeRecordToDB] Successfully added:', bookname);
            resolve();
          };
          addReq.onerror = (e) => {
            console.error('[mergeRecordToDB] Add error:', bookname, e.target.error || e);
            reject(e.target.error || e);
          };
        }
      };
      getReq.onerror = (e) => {
        console.error('[mergeRecordToDB] Get error:', bookname, e.target.error || e);
        reject(e.target.error || e);
      };
      tx.onerror = (e) => {
        console.error('[mergeRecordToDB] Transaction error:', bookname, e.target.error || e);
        reject(e.target.error || e);
      };
    });
  }

  async function fetchAndStoreReadingRecords(bookPath = null, books = null, all = false) {
    try {
      let apiUrl = `${API_BASE}/api/reading_records`;
      if (bookPath) {
        apiUrl += `?book=${encodeURIComponent(bookPath)}`;
      } else if (books) {
        apiUrl += `?books=${encodeURIComponent(books)}`;
      }
      // else: all books (default)
      
      console.log('[fetchAndStoreReadingRecords] API URL:', apiUrl);
      toast('正在获取阅读记录...', 'info', 2000);
      const response = await fetch(apiUrl);
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
      }
      
      const data = await response.json();
      console.log('[fetchAndStoreReadingRecords] Fetched data:', data);
      
      if (data.records && data.records.length > 0) {
        // Store each record to local DB with merge logic
        console.log('[fetchAndStoreReadingRecords] Storing', data.records.length, 'records to DB...');
        for (let i = 0; i < data.records.length; i++) {
          const record = data.records[i];
          console.log(`[fetchAndStoreReadingRecords] Storing record ${i+1}/${data.records.length}:`, record.book_name || record.bookname);
          await mergeRecordToDB(record);
        }
        console.log('[fetchAndStoreReadingRecords] All records stored successfully');
        toast(`已同步 ${data.records.length} 条记录到本地数据库`, 'success', 2000);
      } else {
        console.log('[fetchAndStoreReadingRecords] No records found');
        toast('未获取到阅读记录', 'info', 2000);
      }
    } catch (e) {
      console.error('[fetchAndStoreReadingRecords] Error:', e);
      toast('获取阅读记录失败: ' + e.message, 'error', 3000);
      throw e; // Re-throw to let caller know there was an error
    }
  }

  function formatSize(bytes){
    if(bytes===0) return '0B';
    const units=['B','KB','MB','GB'];
    const i=Math.floor(Math.log(bytes)/Math.log(1024));
    return (bytes/Math.pow(1024,i)).toFixed(1)+units[i];
  }

  function switchCat(cat){
    currentCat = cat; 
    currentPage = 1; // 重置为第一页
    cache[cat] = null; // 清除缓存，强制重新加载
    if (!supportsHierarchy()) currentBookSubdir = ''; // non-hierarchical tab does not keep subdir
    const cfg = currentTabConfig();
    const tabTitle = (cfg && cfg.title) ? cfg.title : cat;
    uploadTitle.textContent = tabTitle + '-文件上传';
    hint.textContent = (cfg && cfg.hint) ? cfg.hint : '';
    // 更新 tab 激活
    document.querySelectorAll('#catTabItems a').forEach(a=>{
      if(a.dataset.cat===cat) a.classList.add('active'); else a.classList.remove('active');
    });
    // clear any selected-for-delete state and selectAll checkbox
    selectedForDelete.clear();
    const delBtn = document.getElementById('btnDeleteSelected'); if(delBtn) delBtn.disabled = true;
    const selAll = document.getElementById('selectAll'); if(selAll){ selAll.checked=false; selAll.indeterminate=false; }
    
    // 对于 screenshot tab，隐藏常规上传区域，显示截图背景设置盒子
    if(uploadBox){ uploadBox.style.display = canUpload() ? 'block' : 'none'; }
    if(scbackBox){ scbackBox.style.display = supportsScback() ? 'block' : 'none'; }
    if(recBox){ recBox.style.display = canReadRecords() ? 'block' : 'none'; }
    if(btnMkdir) btnMkdir.style.display = canMkdir() ? '' : 'none';
    updateBookPathBar();
    
    loadList();
  }

  function updateBookPathBar() {
    const bar = el('bookPathBar');
    const barRow = el('bookPathBarRow');
    if (!bar) return;
    if (!supportsHierarchy() || !currentBookSubdir) {
      if (bar) bar.style.display = 'none';
      if (barRow) barRow.style.display = 'none';
      return;
    }
    if (barRow) barRow.style.display = '';
    bar.style.display = '';
    const baseDir = '/' + currentApiTab();
    // Build breadcrumb: /base > part1 > part2 (each part clickable except the last)
    const parts = currentBookSubdir.split('/');
    let html = `<span class="crumb" data-crumb-idx="-1">${baseDir}</span>`;
    parts.forEach((p, i) => {
      html += `<span class="crumb-sep"> &rsaquo; </span>`;
      if (i < parts.length - 1) {
        html += `<span class="crumb" data-crumb-idx="${i}">${p}</span>`;
      } else {
        html += `<span>${p}</span>`;
      }
    });
    bar.innerHTML = html;
    // Wire clicks: idx=-1 means /book root; idx=i means navigate to parts[0..i]
    bar.querySelectorAll('[data-crumb-idx]').forEach(el => {
      el.onclick = () => {
        const idx = parseInt(el.getAttribute('data-crumb-idx'));
        if (idx === -1) {
          currentBookSubdir = '';
        } else {
          currentBookSubdir = parts.slice(0, idx + 1).join('/');
        }
        cache[currentCat] = null; currentPage = 1;
        updateBookPathBar();
        loadList();
      };
    });
  }

  function navigateBookInto(dirName) {
    if (!supportsHierarchy()) return;
    currentBookSubdir = currentBookSubdir ? currentBookSubdir + '/' + dirName : dirName;
    cache[currentCat] = null;
    currentPage = 1;
    updateBookPathBar();
    loadList();
  }

  function navigateBookUp() {
    if (!supportsHierarchy()) return;
    const parts = currentBookSubdir.split('/');
    parts.pop();
    currentBookSubdir = parts.join('/');
    cache[currentCat] = null;
    currentPage = 1;
    updateBookPathBar();
    loadList();
  }

  async function loadList(){
    fileBody.innerHTML = '<tr><td colspan="5" class="text-center">加载中...</td></tr>';
    pager.classList.add('hidden');
    
    // 使用服务端分页（如果已有缓存则直接渲染，否则请求）
    if(cache[currentCat] && cache[currentCat].page === currentPage){ 
      render(); 
      return; 
    }
    
    try {
      let data;
      const apiTab = currentApiTab();
      const useSubdir = supportsHierarchy() && currentBookSubdir;
      
      // 首次请求时检测后端是否支持分页
      if(paginationSupported === null){
        // 尝试分页请求
        let testUrl = `${API_BASE}/list/${apiTab}?page=1&perPage=${PAGE_SIZE}`;
        if(useSubdir) testUrl += `&subdir=${encodeURIComponent(currentBookSubdir)}`;
        const r = await fetch(testUrl);
        if(!r.ok) throw new Error('HTTP '+r.status);
        data = await r.json();
        
        // 检测返回格式判断是否支持分页
        if(Array.isArray(data)){
          // 旧后端：忽略参数，返回数组
          paginationSupported = false;
          console.log('[FileManager] 后端不支持分页，使用客户端分页模式');
        } else if(data.total !== undefined && data.files !== undefined){
          // 新后端：返回分页对象
          paginationSupported = true;
          console.log('[FileManager] 后端支持分页，使用服务端分页模式');
        } else {
          // 未知格式，保守处理
          paginationSupported = false;
          console.warn('[FileManager] 后端返回未知格式，回退到客户端分页');
        }
      } else if(paginationSupported){
        // 已知支持分页，直接请求
        let pUrl = `${API_BASE}/list/${apiTab}?page=${currentPage}&perPage=${PAGE_SIZE}`;
        if(useSubdir) pUrl += `&subdir=${encodeURIComponent(currentBookSubdir)}`;
        const r = await fetch(pUrl);
        if(!r.ok) throw new Error('HTTP '+r.status);
        data = await r.json();
      } else {
        // 已知不支持分页，请求全部数据
        let npUrl = `${API_BASE}/list/${apiTab}`;
        if(useSubdir) npUrl += `?subdir=${encodeURIComponent(currentBookSubdir)}`;
        const r = await fetch(npUrl);
        if(!r.ok) throw new Error('HTTP '+r.status);
        data = await r.json();
      }
      
      // 统一数据格式（兼容新旧后端）
      if(Array.isArray(data)){
        // 旧格式：纯数组，客户端分页
        cache[currentCat] = {
          total: data.length, 
          page: currentPage, 
          perPage: PAGE_SIZE, 
          files: data.slice((currentPage-1)*PAGE_SIZE, currentPage*PAGE_SIZE),
          _allFiles: data, // 保存全部数据用于客户端分页
          _clientSide: true // 标记为客户端分页
        };
      } else {
        // 新格式：服务端分页
        cache[currentCat] = data;
        cache[currentCat]._clientSide = false;
      }
      
      render();
    } catch(e){
      fileBody.innerHTML = `<tr><td colspan="5" class="text-center">加载失败: ${e.message} <button class='button is-small outline' id='retryBtn'>重试</button></td></tr>`;
      const rb = document.getElementById('retryBtn');
      if(rb) rb.onclick=()=>{ cache[currentCat]=null; loadList(); };
    }
  }

  function render(){
    const data = cache[currentCat];
    if(!data || !data.files || data.files.length===0){
      const baseDir = '/' + currentApiTab();
      const goUpRowEmpty = (supportsHierarchy() && currentBookSubdir)
        ? `<tr><td></td><td class="file-name-cell" style="cursor:pointer" id="goUpRow">&lt;&lt; 返回上级 <span class="muted" style="font-size:0.8em">${baseDir}${currentBookSubdir ? '/'+currentBookSubdir : ''}</span></td><td></td><td></td><td></td></tr>`
        : '';
      fileBody.innerHTML = goUpRowEmpty + '<tr><td colspan="5" class="text-center">暂无文件</td></tr>';
      pager.classList.add('hidden');
      updateUploadState(0);
      if (goUpRowEmpty) {
        const goUpEl = document.getElementById('goUpRow');
        if (goUpEl) goUpEl.onclick = () => navigateBookUp();
      }
      return;
    }
    
    const list = data.files; // 当前页的文件列表
    const total = data.total || list.length; // 文件总数
    updateUploadState(total);
    
    // 计算总页数（基于服务端返回的 total）
    const perPage = data.perPage || PAGE_SIZE;
    const maxPage = Math.ceil(total / perPage);
    currentPage = data.page || currentPage; // 使用服务端返回的页码
    
    // 渲染当前页文件（不再需要 slice，服务端已分页）
    // For book category in a subdirectory, prepend a ".." go-up row
    const baseDir = '/' + currentApiTab();
    const goUpRow = (supportsHierarchy() && currentBookSubdir)
      ? `<tr><td></td><td class="file-name-cell" style="cursor:pointer" id="goUpRow"><< 返回上级 <span class="muted" style="font-size:0.8em">${baseDir}${currentBookSubdir ? '/'+currentBookSubdir : ''}</span></td><td></td><td></td><td></td></tr>`
      : '';

    fileBody.innerHTML = goUpRow + list.map(f=>{
      // Prefer server-supplied full path (untruncated). Fallback to constructed path.
      const fullPath = f.path ? f.path : `/${currentCat}/${f.name}`;
      const disableDelete = !!f.isCurrent;

      if (f.type === 'dir') {
        // Directory row: navigate on name click, rename + delete buttons (no download/record)
        const dirActions = `<button class='button is-small outline' data-dir-rename='${fullPath}' title='重命名目录'>重命名</button>
            <button class='button is-small outline' data-dir-del='${fullPath}'>删除</button>`;
        return `<tr>
          <td></td>
          <td class="file-name-cell" style="cursor:pointer" data-navigate="${encodeURIComponent(f.name)}" title="进入目录 ${f.name}">[${f.name}]<span class="dir-badge">文件夹</span></td>
          <td></td>
          <td></td>
          <td class='file-actions nowrap'>${dirActions}</td>
        </tr>
        <tr class='file-actions-row'><td colspan='5' class='file-actions nowrap'>${dirActions}</td></tr>`;
      }

      // show small badge when book has .idx (isIdxed)
        const idxBadge = (hasIdxBadge() && f.isIdxed) ? ` <span class="badge idx-badge" title="存在目录 (.idx)">目录</span>` : '';
        const apiTab = currentApiTab();
        const currentBadge = f.isCurrent ? ` <span class='badge-current'>${apiTab==='book'?'正在阅读':apiTab==='font'?'当前字体':'当前'}</span>` : '';
      const fileActions = `${f.type==='file'?`<a href='${API_BASE}/download?path=${encodeURIComponent(fullPath)}' data-path='${encodeURIComponent(fullPath)}' class='download-link button is-small outline' title='下载'>下载</a>`:''}
          <button class='button is-small outline' data-del='${fullPath}' ${disableDelete?'disabled':''}>删除</button>
          ${canRename() && f.type==='file'?`<button class='button is-small outline' data-rename='${fullPath}' title='重命名'>重命名</button>`:''}
          ${canReadRecords() && f.type==='file'?`<button class='button is-small outline' data-record='${fullPath}' title='查看阅读记录'>记录</button>`:''}`.trim();
      return `<tr>
        <td><input type="checkbox" class="file-select-checkbox" data-path="${encodeURIComponent(fullPath)}" ${disableDelete? 'disabled' : ''}></td>
        <td class="file-name-cell">${f.name}${currentBadge}${idxBadge}</td>
        <td>${f.type==='file'?formatSize(f.size):''}</td>
        <td>${f.isCurrent? '✔':''}</td>
        <td class='file-actions nowrap'>${fileActions}</td>
      </tr>
      <tr class='file-actions-row'><td colspan='5' class='file-actions nowrap'>${fileActions}</td></tr>`;
    }).join('');

    // 绑定删除按钮
    // bind go-up row
    const goUpEl = document.getElementById('goUpRow');
    if (goUpEl) goUpEl.onclick = () => navigateBookUp();

    // bind directory name cells (navigate into)
    fileBody.querySelectorAll('[data-navigate]').forEach(cell => {
      cell.onclick = () => navigateBookInto(decodeURIComponent(cell.getAttribute('data-navigate')));
    });

    // bind directory delete buttons
    fileBody.querySelectorAll('button[data-dir-del]').forEach(btn => {
      btn.onclick = async () => {
        const dirPath = btn.getAttribute('data-dir-del');
        const dirName = dirPath.split('/').pop();
        const ok = await showConfirm(
          `确认递归删除目录 "${dirName}" 及其所有书籍和关联数据（书签、阅读进度、索引）？此操作不可恢复。`,
          '删除目录'
        );
        if (!ok) return;
        try {
          const r = await fetch(`${API_BASE}/delete?path=${encodeURIComponent(dirPath)}`);
          const j = await r.json();
          if (j.ok) {
            toast('目录已删除：' + dirName, 'success');
            cache[currentCat] = null;
            await new Promise(r => setTimeout(r, 600));
            loadList();
          } else {
            toast(j.message || '删除失败', 'error', 5000);
          }
        } catch(e) { toast('删除失败: ' + e.message, 'error', 5000); }
      };
    });

    // bind directory rename buttons
    fileBody.querySelectorAll('button[data-dir-rename]').forEach(btn => {
      btn.onclick = async () => {
        const dirPath = btn.getAttribute('data-dir-rename');
        const currentName = dirPath.split('/').pop();
        const promptFn = typeof window.nicePrompt === 'function'
          ? (msg, def) => window.nicePrompt(msg, def, {title:'重命名目录'})
          : (msg, def) => Promise.resolve(window.prompt(msg, def));
        const newName = await promptFn('请输入新目录名：', currentName);
        if (!newName || newName === currentName) return;
        if (newName.includes('/') || newName.includes('\\') || newName.includes('..')) {
          toast('目录名无效', 'error'); return;
        }
        const confirmed = await showConfirm(
          `重命名目录将同步更新所有子书籍的阅读进度、书签及历史记录路径。\n\n将 "${currentName}" 重命名为 "${newName}"？`,
          '重命名确认'
        );
        if (!confirmed) return;
        try {
          const r = await fetch(`${API_BASE}/rename?old_path=${encodeURIComponent(dirPath)}&new_name=${encodeURIComponent(newName)}`);
          const j = await r.json();
          if (j.ok) {
            toast('目录已重命名：' + newName, 'success');
            // If we're currently inside the renamed directory, update currentBookSubdir
            const parts = currentBookSubdir.split('/');
            if (parts[parts.length - 1] === currentName) {
              parts[parts.length - 1] = newName;
              currentBookSubdir = parts.join('/');
              updateBookPathBar();
            }
            cache[currentCat] = null;
            await new Promise(r => setTimeout(r, 600));
            loadList();
          } else {
            toast(j.message || '重命名失败', 'error', 5000);
          }
        } catch(e) { toast('重命名失败: ' + e.message, 'error', 5000); }
      };
    });

    // bind delete buttons (single or batch for screenshots)
    fileBody.querySelectorAll('button[data-del]').forEach(btn=>{
      btn.onclick = async ()=>{
        const path = btn.getAttribute('data-del');

        // If current category is screenshot and multiple items are selected,
        // treat this as a batch delete for all selected files.
        if(supportsScback() && selectedForDelete.size > 1){
          const paths = Array.from(selectedForDelete);
          const ok = await showConfirm(`确认删除 ${paths.length} 个截图？`);
          if(!ok) return;
          let successCount = 0;
          for(const p of paths){
            try{
              const r = await fetch(`${API_BASE}/delete?path=${encodeURIComponent(p)}`);
              const j = await r.json();
              if(j.ok) successCount++;
            }catch(e){ /* ignore per-file errors */ }
          }
          toast(`已删除 ${successCount} 个文件`,'success');
          selectedForDelete.clear();
          cache[currentCat]=null;
          // 延迟刷新，确保后端文件系统操作完全同步
          await new Promise(r=>setTimeout(r, 600));
          loadList();
          return;
        }

        // Fallback: single-file delete (existing behavior)
        const ok = await showConfirm('确认删除 '+path+' ?');
        if(!ok) return;
        try {
          const r = await fetch(`${API_BASE}/delete?path=${encodeURIComponent(path)}`);
          const j = await r.json();
          if(j.ok){ 
            toast('删除成功','success'); 
            cache[currentCat]=null; 
            // 延迟刷新，确保后端文件系统操作完全同步
            await new Promise(r=>setTimeout(r, 500)); // 500ms 延迟
            loadList(); 
          }
          else toast(j.message||'删除失败','error',5000);
        } catch(e){ toast('删除失败: '+e.message,'error',5000); }
      };
    });

    // bind reading records buttons (for book category)
    if(canReadRecords()){
      fileBody.querySelectorAll('button[data-record]').forEach(btn=>{
        btn.onclick = async ()=>{
          const bookPath = btn.getAttribute('data-record');
          await fetchAndStoreReadingRecords(bookPath);
          window.open(`readingRecord.html?book=${encodeURIComponent(bookPath)}&src=local`, '_blank');
        };
      });
    }

    // 绑定重命名按钮（仅书籍分类）
    if(canRename()){
      fileBody.querySelectorAll('button[data-rename]').forEach(btn=>{
        btn.onclick = async ()=>{
          const path = btn.getAttribute('data-rename');
          const parts = path.split('/');
          const currentName = parts[parts.length - 1] || '';

          // 弹出输入框获取新文件名
          const promptFn = typeof window.nicePrompt === 'function'
            ? (msg, def) => window.nicePrompt(msg, def, {title:'重命名书籍'})
            : (msg, def) => Promise.resolve(window.prompt(msg, def));
          const rawNew = await promptFn('请输入新文件名（保留 .txt 后缀）：', currentName);
          if(!rawNew || rawNew === currentName) return;

          // 自动补全 .txt 后缀
          const newName = rawNew.toLowerCase().endsWith('.txt') ? rawNew : rawNew + '.txt';
          if(newName === currentName) return;

          // 弹框告知用户重命名影响
          const confirmed = await showConfirm(
            '重命名书籍后，设备端按文件名存储的阅读进度（书签、标签、索引等）将会同步迁移。\n\n' +
            '但网页端本地保存的阅读时间记录（IndexedDB）按书名索引，' +
            '旧记录不会自动迁移，新旧文件名下的阅读时长记录将独立存储。\n\n' +
            '将 "' + currentName + '" 重命名为 "' + newName + '"？',
            '重命名确认'
          );
          if(!confirmed) return;

          try{
            const r = await fetch(`${API_BASE}/rename?old_path=${encodeURIComponent(path)}&new_name=${encodeURIComponent(newName)}`);
            const j = await r.json();
            if(j.ok){
              toast('重命名成功：' + newName, 'success');
              cache[currentCat] = null;
              await new Promise(r => setTimeout(r, 500));
              loadList();
            } else {
              toast(j.message || '重命名失败', 'error', 5000);
            }
          } catch(e){
            toast('重命名失败: ' + e.message, 'error', 5000);
          }
        };
      });
    }

    // bind per-row checkbox events
    fileBody.querySelectorAll('.file-select-checkbox').forEach(cb=>{
      cb.onchange = ()=>{
        const p = decodeURIComponent(cb.getAttribute('data-path'));
        if(cb.checked) selectedForDelete.add(p); else selectedForDelete.delete(p);
        // enable delete selected button if any selected
        const delBtn = document.getElementById('btnDeleteSelected');
        if(delBtn) delBtn.disabled = selectedForDelete.size===0;
        // update selectAll checkbox state
        const all = document.querySelectorAll('.file-select-checkbox:not(:disabled)');
        const checked = document.querySelectorAll('.file-select-checkbox:checked:not(:disabled)');
        const selAll = document.getElementById('selectAll');
        if(selAll){ selAll.indeterminate = checked.length>0 && checked.length<all.length; selAll.checked = checked.length===all.length && all.length>0; }
      };
    });

    // bind download links: for screenshots, if multiple selected, clicking any download will package selected files into zip
    fileBody.querySelectorAll('.download-link').forEach(a=>{
      a.onclick = async (e)=>{
        try{
          if(supportsScback() && selectedForDelete.size > 1 && window.JSZip){
            e.preventDefault();
            const zip = new JSZip();
            const paths = Array.from(selectedForDelete);
            // Fetch each file as blob and add to zip
            for(const p of paths){
              try{
                const url = `${API_BASE}/download?path=${encodeURIComponent(p)}`;
                const r = await fetch(url);
                if(!r.ok) throw new Error('HTTP '+r.status);
                const blob = await r.blob();
                // derive basename
                const parts = p.split('/');
                const name = parts[parts.length-1] || 'file';
                zip.file(name, blob);
              }catch(fe){
                toast(`获取 ${p} 失败: ${fe.message}`,'error',5000);
              }
            }
            // generate zip and trigger download
            const baseName = 'screenshots';
            const outName = `${baseName}.zip`;
            const zblob = await zip.generateAsync({type:'blob'}, (meta)=>{
              // optional progress feedback
              const pct = Math.floor(meta.percent);
              // update uploadStatus as progress indicator
              const status = el('uploadStatus'); if(status) status.textContent = `打包中... ${pct}%`;
            });
            const url = URL.createObjectURL(zblob);
            const dl = document.createElement('a'); dl.href = url; dl.download = outName; document.body.appendChild(dl); dl.click(); setTimeout(()=>{ URL.revokeObjectURL(url); if(dl.parentNode) dl.parentNode.removeChild(dl); const status = el('uploadStatus'); if(status) status.textContent=''; }, 1000);
            return;
          }
          // else let default action proceed (browser download via link)
        }catch(err){
          toast('打包下载失败: '+err.message,'error',5000);
          e.preventDefault();
        }
      };
    });

    // 分页
    if(maxPage>1){
      pager.classList.remove('hidden');
      pageInfo.textContent = `${currentPage}/${maxPage}`;
      // For cyclic pagination, keep buttons enabled when there are multiple pages
      prevBtn.disabled = false;
      nextBtn.disabled = false;
    } else {
      pager.classList.add('hidden');
      // disable when only a single page exists
      prevBtn.disabled = true;
      nextBtn.disabled = true;
    }
  }

  function updateUploadState(count){
    const info = el('uploadInfo');
    if(count===0){ info.textContent = ""; btnUpload.disabled = selectedFiles.length===0; info.style.color='var(--grey)' }
    //if(count===0){ info.textContent = "何处见那'遁去的一'?"; btnUpload.disabled = selectedFiles.length===0; info.style.color='var(--grey)' }
    else if(count>=99){ info.textContent = ''; btnUpload.disabled = true; info.style.color='#b30000'; }
    //else if(count>=99){ info.textContent = '宁缺毋滥，九九归一'; btnUpload.disabled = true; info.style.color='#b30000'; }
    else { info.textContent=''; btnUpload.disabled = selectedFiles.length===0; }
  }

  // Cyclic pagination: go to previous, wrapping to last page; next wraps to first page
  prevBtn.onclick = ()=>{
    const data = cache[currentCat];
    if(!data) return;
    const total = data.total || 0;
    const perPage = data.perPage || PAGE_SIZE;
    const maxPage = Math.max(1, Math.ceil(total / perPage));
    if(maxPage <= 1) return; // nothing to do
    
    if(currentPage>1) currentPage--; else currentPage = maxPage;
    
    // 客户端分页模式：直接从缓存切片
    if(data._clientSide && data._allFiles){
      cache[currentCat].page = currentPage;
      cache[currentCat].files = data._allFiles.slice((currentPage-1)*PAGE_SIZE, currentPage*PAGE_SIZE);
      render();
    } else {
      // 服务端分页模式：清除缓存，触发请求
      cache[currentCat] = null;
      loadList();
    }
  };
  nextBtn.onclick = ()=>{
    const data = cache[currentCat];
    if(!data) return;
    const total = data.total || 0;
    const perPage = data.perPage || PAGE_SIZE;
    const maxPage = Math.max(1, Math.ceil(total / perPage));
    if(maxPage <= 1) return;
    
    if(currentPage<maxPage) currentPage++; else currentPage = 1;
    
    // 客户端分页模式：直接从缓存切片
    if(data._clientSide && data._allFiles){
      cache[currentCat].page = currentPage;
      cache[currentCat].files = data._allFiles.slice((currentPage-1)*PAGE_SIZE, currentPage*PAGE_SIZE);
      render();
    } else {
      // 服务端分页模式：清除缓存，触发请求
      cache[currentCat] = null;
      loadList();
    }
  };

  btnSelect.onclick = ()=> fileInput.click();
  fileInput.onchange = ()=>{
    selectedFiles = Array.from(fileInput.files||[]);
    if(selectedFiles.length){ btnUpload.textContent = '上传 '+selectedFiles.length+' 个文件'; btnUpload.disabled=false; }
    else { btnUpload.textContent='开始上传'; btnUpload.disabled=true; }
    updateUploadState(cache[currentCat]?cache[currentCat].length:0);
  };

  // 截图背景设置控件逻辑（仅对 screenshot tab 可见）
  if(btnScbackSelect) btnScbackSelect.onclick = ()=> scbackFileInput.click();
  if(scbackFileInput) scbackFileInput.onchange = ()=>{
    const files = Array.from(scbackFileInput.files||[]);
    if(files.length===0){ selectedScbackFile = null; if(btnScbackUpload) btnScbackUpload.disabled = true; return; }
    const f = files[0];
    // Accept any image file; it will be uploaded as scback.png and overwrite the SD root file
    selectedScbackFile = f;
    if(btnScbackUpload){ btnScbackUpload.disabled = false; }
    if(scbackInfo) scbackInfo.textContent = `已选择: ${f.name}`;
  };

  if(btnScbackUpload) btnScbackUpload.onclick = async ()=>{
    if(!selectedScbackFile){ toast('请先选择要上传的图片','error'); return; }
    btnScbackUpload.disabled = true; if(scbackInfo) scbackInfo.textContent = '上传中...'; if(uploadStatus) uploadStatus.textContent = '上传背景: 0%';
    try{
      await performScbackUpload(selectedScbackFile, p=>{ if(uploadStatus) uploadStatus.textContent = `上传背景: ${p.toFixed(1)}%`; if(scbackInfo) scbackInfo.textContent = `上传 ${p.toFixed(1)}%`; });
      toast('背景上传成功','success');
      // small delay then refresh list
      await new Promise(r=>setTimeout(r, 400));
      cache[currentCat]=null; loadList();
    }catch(e){ toast('上传失败: '+e.message,'error',5000); }
    finally{ if(btnScbackUpload) btnScbackUpload.disabled = false; if(scbackInfo) scbackInfo.textContent = '上传的图片会保存为 scback.png，并覆盖 SD 根目录的同名文件。'; if(uploadStatus) uploadStatus.textContent = ''; }
  };

  if(btnScbackDelete) btnScbackDelete.onclick = async ()=>{
    const ok = await showConfirm('确认删除 SD 根目录下的 scback.png 吗？');
    if(!ok) return;
    try{
      const r = await fetch(`${API_BASE}/delete?path=${encodeURIComponent('/scback.png')}`);
      const j = await r.json();
      if(j.ok){ toast('已删除 scback.png','success'); cache[currentCat]=null; await new Promise(r=>setTimeout(r,400)); loadList(); }
      else toast(j.message||'删除失败','error',5000);
    }catch(e){ toast('删除失败: '+e.message,'error',5000); }
  };

  // 拖拽
  ['dragenter','dragover','dragleave','drop'].forEach(ev=>{
    uploadBox.addEventListener(ev,e=>{ e.preventDefault(); e.stopPropagation(); });
  });
  ['dragenter','dragover'].forEach(ev=>uploadBox.addEventListener(ev,()=>uploadBox.classList.add('dragover')));
  ['dragleave','drop'].forEach(ev=>uploadBox.addEventListener(ev,()=>uploadBox.classList.remove('dragover')));
  uploadBox.addEventListener('drop', e=>{ selectedFiles = Array.from(e.dataTransfer.files); fileInput.value=''; btnUpload.textContent='上传 '+selectedFiles.length+' 个文件'; btnUpload.disabled = selectedFiles.length===0; updateUploadState(cache[currentCat]?cache[currentCat].length:0); });

  // 新建目录
  if (btnMkdir) {
    btnMkdir.onclick = async () => {
      if (!canMkdir()) return;
      const promptFn = typeof window.nicePrompt === 'function'
        ? (msg, def) => window.nicePrompt(msg, def, {title:'新建目录'})
        : (msg, def) => Promise.resolve(window.prompt(msg, def));
      const dirName = await promptFn('请输入新目录名：', '');
      if (!dirName) return;
      if (dirName.includes('/') || dirName.includes('\\') || dirName.includes('..')) {
        toast('目录名无效', 'error'); return;
      }
      const baseDir = '/' + currentApiTab();
      const parentPath = (supportsHierarchy() && currentBookSubdir) ? (baseDir + '/' + currentBookSubdir) : baseDir;
      const fullPath = parentPath + '/' + dirName;
      try {
        const r = await fetch(`${API_BASE}/mkdir?path=${encodeURIComponent(fullPath)}`);
        const j = await r.json();
        if (j.ok) {
          toast('目录已创建：' + dirName, 'success');
          cache[currentCat] = null;
          await new Promise(r => setTimeout(r, 400));
          loadList();
        } else {
          toast(j.message || '创建失败', 'error', 5000);
        }
      } catch(e) { toast('创建失败: ' + e.message, 'error', 5000); }
    };
  }

  btnUpload.onclick = ()=>{
    if(selectedFiles.length===0) return;
    btnUpload.disabled = true; btnUpload.textContent='上传中...';
    uploadStatus.textContent='';
    uploadSequential(0);
  };

  async function uploadSequential(i){
    if(i>=selectedFiles.length){
      // 所有文件上传完成
      toast('所有文件上传完成！','success');
      btnUpload.disabled=false;
      btnUpload.textContent='开始上传';
      selectedFiles=[];
      fileInput.value='';
      cache[currentCat]=null; // 清除缓存
      currentPage = 1; // 重置到第一页查看新上传的文件
      // 通知并刷新列表：先显示正在刷新，再等待 loadList 完成后清理状态
      uploadStatus.textContent = '正在刷新列表...';
      // 给设备一点时间让文件系统稳定（短暂等待），然后等待 loadList 完成
      await new Promise(r=>setTimeout(r, 400)); // 400ms 延迟
      await loadList();
      uploadStatus.textContent = '';
      return;
    }
    const f = selectedFiles[i];
    uploadStatus.textContent = `正在上传 ${f.name} (${formatSize(f.size)}) ${i+1}/${selectedFiles.length}`;
    try {
      await performUpload(f, p=>{ uploadStatus.textContent = `上传 ${f.name}: ${p.toFixed(1)}% (${i+1}/${selectedFiles.length})`; });
      // 单个文件上传成功，不显示 toast，避免干扰
      // 增加短延迟以给服务器/文件系统留够时间完成后处理（rename/刷新缓存等）
      await new Promise(r=>setTimeout(r, 300));
      await uploadSequential(i+1); // 继续下一个文件
    } catch(e){
      const retry = await showConfirm(`上传 ${f.name} 失败: ${e.message}\n是否重试?`);
      if(retry){ setTimeout(()=>uploadSequential(i),500); }
      else { setTimeout(()=>uploadSequential(i+1),300); }
    }
  }

  function performUpload(file,onProgress){
    if(DEBUG_MODE){
      return new Promise((resolve)=>{
        let p = 0;
        const timer = setInterval(()=>{
          p += 25;
          if(onProgress) onProgress(Math.min(100, p));
          if(p >= 100){
            clearInterval(timer);
            const currentTab = currentApiTab();
            const currentSubdir = supportsHierarchy() ? currentBookSubdir : '';
            debugAddUploadedFile(file, currentTab, currentSubdir);
            resolve('OK (debug upload)');
          }
        }, 60);
      });
    }

    // Implement automatic retry with exponential backoff to match original template.html behaviour.
    const maxRetries = 2; // same as template.html
    const baseDelay = 500; // ms
    const currentTab = currentApiTab();
    const currentSubdir = supportsHierarchy() ? currentBookSubdir : '';

    return (async function(){
      for(let attempt=0; attempt<=maxRetries; attempt++){
        try{
          const res = await new Promise((resolve,reject)=>{
            const xhr = new XMLHttpRequest();
            let uploadUrl = `${API_BASE}/upload?tab=${encodeURIComponent(currentTab)}`;
            if(currentSubdir) uploadUrl += `&subdir=${encodeURIComponent(currentSubdir)}`;
            xhr.open('POST', uploadUrl);
            xhr.upload.onprogress = e=>{ if(e.lengthComputable && onProgress){ onProgress((e.loaded/e.total)*100); } };
            xhr.onerror=()=>reject(new Error('network'));
            xhr.ontimeout=()=>reject(new Error('timeout'));
            xhr.onload=()=>{
              if(xhr.status===200){
                try{ const j=JSON.parse(xhr.responseText||'{}'); if(typeof j.ok!=='undefined'){ if(j.ok) return resolve(j.message||'OK'); else return reject(new Error(j.message||'上传失败')); } }
                catch(_){ /* non-json, treat as success */ return resolve(xhr.responseText||'OK'); }
                // if JSON didn't have ok, treat as success
                return resolve(xhr.responseText||'OK');
              }
              if(xhr.status===413) return reject(new Error('文件过大'));
              return reject(new Error('HTTP '+xhr.status));
            };
            xhr.timeout = 300000; // 5 minutes
            const fd = new FormData(); fd.append('data', file, file.name); xhr.send(fd);
          });
          return res;
        } catch(err){
          // On network-like errors, try to verify via /list whether server accepted file
          const msg = (err && err.message) ? err.message.toString() : '';
          if(msg === 'network' || msg.toLowerCase().includes('http')){
            // small wait to allow server flush
            await new Promise(r=>setTimeout(r,400));
            try{
              const r = await fetch(`${API_BASE}/list/${encodeURIComponent(currentTab)}`);
              if(r.ok){
                const files = await r.json();
                const exists = files.some(f=>f.name===file.name);
                if(exists) return 'OK (verified via listing)';
              }
            }catch(e){ /* ignore verification errors */ }
          }

          if(attempt < maxRetries){
            const delay = baseDelay * Math.pow(2, attempt);
            toast(`上传失败，正在重试（第 ${attempt+1} 次）...`, 'info', 2000);
            await new Promise(r=>setTimeout(r, delay));
            continue; // retry
          }
          // exhausted retries
          throw err;
        }
      }
    })();
  }

    function performScbackUpload(file,onProgress){
      if(DEBUG_MODE){
        return new Promise((resolve)=>{
          let p = 0;
          const timer = setInterval(()=>{
            p += 33;
            if(onProgress) onProgress(Math.min(100, p));
            if(p >= 100){
              clearInterval(timer);
              resolve('OK (debug scback upload)');
            }
          }, 70);
        });
      }

      const maxRetries = 2;
      const baseDelay = 500;
      return (async function(){
        for(let attempt=0; attempt<=maxRetries; attempt++){
          try{
            const res = await new Promise((resolve,reject)=>{
              const xhr = new XMLHttpRequest();
              xhr.open('POST', `${API_BASE}/upload?tab=scback`);
              xhr.upload.onprogress = e=>{ if(e.lengthComputable && onProgress){ onProgress((e.loaded/e.total)*100); } };
              xhr.onerror=()=>reject(new Error('network'));
              xhr.ontimeout=()=>reject(new Error('timeout'));
              xhr.onload=()=>{
                if(xhr.status===200){
                  try{ const j=JSON.parse(xhr.responseText||'{}'); if(typeof j.ok!=='undefined'){ if(j.ok) return resolve(j.message||'OK'); else return reject(new Error(j.message||'上传失败')); } }
                  catch(_){ return resolve(xhr.responseText||'OK'); }
                  return resolve(xhr.responseText||'OK');
                }
                if(xhr.status===413) return reject(new Error('文件过大'));
                return reject(new Error('HTTP '+xhr.status));
              };
              xhr.timeout = 300000;
              const fd = new FormData(); fd.append('data', file, 'scback.png'); xhr.send(fd);
            });
            return res;
          } catch(err){
            if(attempt < maxRetries){
              const delay = baseDelay * Math.pow(2, attempt);
              toast(`上传失败，正在重试（第 ${attempt+1} 次）...`, 'info', 2000);
              await new Promise(r=>setTimeout(r, delay));
              continue;
            }
            throw err;
          }
        }
      })();
    }

  btnSyncTime.onclick = async ()=>{
    btnSyncTime.disabled=true; btnSyncTime.textContent='同步中...';
    const now = new Date();
    const payload = { timestamp: Math.floor(now.getTime()/1000), iso: now.toISOString(), tzOffsetMinutes: now.getTimezoneOffset() };
    try {
      const r = await fetch(`${API_BASE}/sync_time`, { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(payload)});
      const txt = await r.text();
      if(r.ok) toast('时间同步成功','success'); else toast('时间同步失败: '+txt,'error',5000);
    } catch(e){ toast('时间同步失败: '+e.message,'error',5000); }
    finally { btnSyncTime.disabled=false; btnSyncTime.textContent='同步时间'; }
  };

  if(tabFileManagement) tabFileManagement.onclick = ()=> setModuleMode('file');
  if(tabTimeManagement) tabTimeManagement.onclick = ()=> setModuleMode('time');
  if(tabSettingsManagement) tabSettingsManagement.onclick = ()=> setModuleMode('settings');

  if(btnLoadWifiSettings){
    btnLoadWifiSettings.onclick = async ()=>{
      try {
        await loadWifiSettings();
        setSettingsStatus('WiFi 配置已刷新。');
      } catch(e){
        setSettingsStatus('WiFi 配置刷新失败: ' + (e.message || e), true);
      }
    };
  }

  if(btnSaveWifiSettings){
    btnSaveWifiSettings.onclick = async ()=>{
      btnSaveWifiSettings.disabled = true;
      try {
        await saveWifiSettings();
        setSettingsStatus('WiFi 配置保存成功。');
        toast('WiFi 配置已保存', 'success');
      } catch(e){
        setSettingsStatus('WiFi 配置保存失败: ' + (e.message || e), true);
        toast('WiFi 配置保存失败: ' + (e.message || e), 'error', 4000);
      } finally {
        btnSaveWifiSettings.disabled = false;
      }
    };
  }

  if(btnLoadWebdavSettings){
    btnLoadWebdavSettings.onclick = async ()=>{
      try {
        await loadWebdavSettings();
        setSettingsStatus('WebDAV 配置已刷新。');
      } catch(e){
        setSettingsStatus('WebDAV 配置刷新失败: ' + (e.message || e), true);
      }
    };
  }

  if(btnSaveWebdavSettings){
    btnSaveWebdavSettings.onclick = async ()=>{
      btnSaveWebdavSettings.disabled = true;
      try {
        await saveWebdavSettings();
        setSettingsStatus('WebDAV 配置保存成功。');
        toast('WebDAV 配置已保存', 'success');
      } catch(e){
        setSettingsStatus('WebDAV 配置保存失败: ' + (e.message || e), true);
        toast('WebDAV 配置保存失败: ' + (e.message || e), 'error', 4000);
      } finally {
        btnSaveWebdavSettings.disabled = false;
      }
    };
  }

  // selectAll checkbox handler
  const selectAllEl = document.getElementById('selectAll');
  const btnDeleteSelected = document.getElementById('btnDeleteSelected');
  if(selectAllEl){
    // helper to recompute selectedForDelete and buttons from checkboxes
    function updateSelectionFromCheckboxes(){
      selectedForDelete.clear();
      const all = Array.from(document.querySelectorAll('.file-select-checkbox:not(:disabled)'));
      const checked = all.filter(cb=>cb.checked);
      checked.forEach(cb=> selectedForDelete.add(decodeURIComponent(cb.getAttribute('data-path'))));
      const delBtn = document.getElementById('btnDeleteSelected'); if(delBtn) delBtn.disabled = selectedForDelete.size===0;
      const selAll = document.getElementById('selectAll'); if(selAll){ selAll.indeterminate = checked.length>0 && checked.length<all.length; selAll.checked = checked.length===all.length && all.length>0; }
    }

    selectAllEl.onchange = ()=>{
      const all = document.querySelectorAll('.file-select-checkbox:not(:disabled)');
      all.forEach(cb=>{ cb.checked = selectAllEl.checked; });
      // run a single unified update in next tick to avoid inconsistent partial updates
      setTimeout(updateSelectionFromCheckboxes, 0);
    };
  }
  if(btnDeleteSelected){
    btnDeleteSelected.onclick = async ()=>{
      if(selectedForDelete.size===0) return;
      const ok = await showConfirm(`确认删除 ${selectedForDelete.size} 个文件？`);
      if(!ok) return;
      // perform deletes sequentially
      const paths = Array.from(selectedForDelete);
      let successCount = 0;
      for(const p of paths){
        try{
          const r = await fetch(`${API_BASE}/delete?path=${encodeURIComponent(p)}`);
          const j = await r.json();
          if(j.ok){ successCount++; }
        }catch(e){ /* ignore per-file errors */ }
      }
      toast(`已删除 ${successCount} 个文件`,'success');
      selectedForDelete.clear();
      cache[currentCat]=null;
      // 延迟刷新，确保后端文件系统操作完全同步
      await new Promise(r=>setTimeout(r, 600)); // 600ms 延迟
      loadList();
    };
  }

  // Batch reading records button
  const btnBatchRecords = document.getElementById('btnBatchRecords');
  if(btnBatchRecords){
    btnBatchRecords.onclick = async ()=>{
      if(selectedForDelete.size === 0){
        toast('请先选择要查看阅读记录的书籍', 'error', 3000);
        return;
      }
      const books = Array.from(selectedForDelete).join(',');
      console.log('[btnBatchRecords] Selected books:', books);
      try {
        await fetchAndStoreReadingRecords(null, books);
        console.log('[btnBatchRecords] Records fetched and stored, opening window...');
        window.open(`readingRecord.html?books=${encodeURIComponent(books)}&src=local`, '_blank');
      } catch (e) {
        console.error('[btnBatchRecords] Failed:', e);
        // Error already shown by fetchAndStoreReadingRecords
      }
    };
  }

  // All reading records button
  const btnAllRecords = document.getElementById('btnAllRecords');
  if(btnAllRecords){
    btnAllRecords.onclick = async ()=>{
      console.log('[btnAllRecords] Fetching all records...');
      try {
        await fetchAndStoreReadingRecords(null, null, true);
        console.log('[btnAllRecords] Records fetched and stored, opening window...');
        window.open(`readingRecord.html?all=true&src=local`, '_blank');
      } catch (e) {
        console.error('[btnAllRecords] Failed:', e);
        // Error already shown by fetchAndStoreReadingRecords
      }
    };
  }

  // 初始化：先加载 guide，再切到首个可用文件页签
  (async function initDeviceManagement(){
    applyDebugBadge();
    await initializeDeviceGuide();
    await initializeSettingsData();
    const firstTab = tabConfigs && tabConfigs.length ? tabConfigs[0].id : 'book';
    switchCat(firstTab);
    setModuleMode('file');
    if(DEBUG_MODE){
      toast('已进入 DEBUG MODE：本地测试向量已启用', 'success', 2600);
    }
  })();

  // Ensure the top nav's "设备管理" tab appears active/text-dark on this page
  // and clicking it does nothing (no-op) while the user is already on filemanager.html.
  (function ensureFileTabActive(){
    const start = Date.now();
    const iv = setInterval(()=>{
      const tabs = document.querySelectorAll('.tabs a');
      if(tabs && tabs.length){
        // find by visible text (中文), fallback to href matching
        let fileTab = Array.from(tabs).find(a=> (a.textContent||'').trim()==='设备管理');
        if(!fileTab) fileTab = document.querySelector('.tabs a[href="filemanager.html"]') || document.querySelector('.tabs a[href="welcome.html"]');
        if(fileTab){
          // make it look active and dark
          fileTab.classList.add('active');
          fileTab.classList.add('text-dark');
          fileTab.classList.remove('text-light');
          // prevent click navigation
          fileTab.addEventListener('click', function(e){ e.preventDefault(); e.stopPropagation(); }, true);
          clearInterval(iv);
        }
      }
      if(Date.now() - start > 5000) clearInterval(iv);
    }, 200);
  })();
})();
