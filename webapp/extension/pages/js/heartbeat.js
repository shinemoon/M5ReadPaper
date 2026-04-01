(function(){
  const DEBUG_FLAG_KEY = 'device_mgmt_debug';
  function isTruthyFlag(v){
    if(v == null) return false;
    const s = String(v).trim().toLowerCase();
    return s === '1' || s === 'true' || s === 'yes' || s === 'on';
  }
  function isDeviceMgmtDebugMode(){
    try {
      const q = new URLSearchParams(window.location.search || '');
      let fromQuery = null;
      if(q.has('debug')) fromQuery = q.get('debug');
      if(q.has('dm_debug')) fromQuery = q.get('dm_debug');
      if(q.has('device_debug')) fromQuery = q.get('device_debug');
      if(q.has('debug_off') && isTruthyFlag(q.get('debug_off'))) fromQuery = '0';
      if(fromQuery != null){
        const on = isTruthyFlag(fromQuery);
        try { localStorage.setItem(DEBUG_FLAG_KEY, on ? '1' : '0'); } catch(_){ }
        return on;
      }
      try { return isTruthyFlag(localStorage.getItem(DEBUG_FLAG_KEY)); } catch(_){ return false; }
    } catch(_){
      return false;
    }
  }

  // Use the heartbeat endpoint implemented on the device
  const DEVICE_URL = 'http://192.168.4.1/heartbeat';
  const GUIDE_URL = 'http://192.168.4.1/api/device_guide';
  const INTERVAL_MS = 1000;
  const GUIDE_CACHE_TTL_MS = 15000;
  // start pessimistic: assume offline until heartbeat proves otherwise
  let online = false;
  let fileTabApplied = false;
  let guideFetchInFlight = false;
  const DEBUG_MODE = isDeviceMgmtDebugMode();

  // Global device info for webapp. Defaults as requested.
  window.deviceInfo = window.deviceInfo || { hw: 'M5Stack PaperS3', firmware: 'ReadPaper', version: 'V1.3' };
  window.deviceGuideCache = window.deviceGuideCache || { guide: null, fetchedAt: 0 };

  function log(){ if(window.console) console.debug.apply(console, ['[heartbeat]'].concat(Array.from(arguments))); }

  function canUseBackgroundHeartbeat(){
    try{
      return !!(window.chrome && chrome.runtime && chrome.runtime.id && typeof chrome.runtime.sendMessage === 'function');
    }catch(_){
      return false;
    }
  }

  async function fetchViaBackground(url, timeoutMs){
    return await new Promise((resolve, reject) => {
      try{
        chrome.runtime.sendMessage({ type: 'heartbeat_fetch', url, timeoutMs }, (resp) => {
          if(chrome.runtime.lastError){
            reject(new Error(chrome.runtime.lastError.message || 'runtime_error'));
            return;
          }
          if(!resp || !resp.ok){
            reject(new Error(resp && resp.error ? resp.error : 'heartbeat_fetch_failed'));
            return;
          }
          resolve(resp.response || null);
        });
      }catch(e){
        reject(e);
      }
    });
  }

  async function fetchJson(url, timeoutMs){
    if(canUseBackgroundHeartbeat()){
      const resp = await fetchViaBackground(url, timeoutMs);
      if(!resp || !resp.ok) return { ok: false, status: resp && resp.status ? resp.status : 0, json: null };
      if(resp.bodyType === 'json') return { ok: true, status: resp.status || 200, json: resp.body || null };
      if(typeof resp.body === 'string'){
        const txt = resp.body.trim();
        if(!txt) return { ok: true, status: resp.status || 200, json: null };
        try{
          return { ok: true, status: resp.status || 200, json: JSON.parse(txt) };
        }catch(_){
          return { ok: true, status: resp.status || 200, json: null };
        }
      }
      return { ok: true, status: resp.status || 200, json: null };
    }

    const controller = new AbortController();
    const timer = setTimeout(()=>controller.abort(), timeoutMs || 2500);
    try{
      const r = await fetch(url, {mode:'cors', cache:'no-store', signal: controller.signal});
      if(!r || !r.ok) return { ok: false, status: r ? r.status : 0, json: null };
      try{
        return { ok: true, status: r.status, json: await r.json() };
      }catch(_){
        return { ok: true, status: r.status, json: null };
      }
    }finally{
      clearTimeout(timer);
    }
  }

  async function refreshDeviceGuideCache(force){
    if(!online) return;
    const now = Date.now();
    const cache = window.deviceGuideCache || (window.deviceGuideCache = { guide: null, fetchedAt: 0 });
    if(!force && cache.guide && (now - (cache.fetchedAt || 0) < GUIDE_CACHE_TTL_MS)) return;
    if(guideFetchInFlight) return;
    guideFetchInFlight = true;
    try{
      const r = await fetchJson(GUIDE_URL, 2500);
      if(!r || !r.ok || !r.json) return;
      const j = r.json;
      if(j && j.ok){
        cache.guide = j;
        cache.fetchedAt = Date.now();
      }
    }catch(e){
      log('device_guide cache refresh failed', e && e.message ? e.message : e);
    }finally{
      guideFetchInFlight = false;
    }
  }

  async function checkOnce(){
    if(DEBUG_MODE){
      setOnline(true);
      if(window.deviceInfo){
        window.deviceInfo.hw = window.deviceInfo.hw || 'PaperS3-Debug';
        window.deviceInfo.firmware = window.deviceInfo.firmware || 'ReadPaper';
        window.deviceInfo.version = 'DEBUG-UI';
      }
      return;
    }
    try{
      // Use background proxy in extension pages so offline heartbeats are handled as normal state.
      const r = await fetchJson(DEVICE_URL, 2500);
      if (r && r.ok) {
        // Only treat device as online when the heartbeat returns a JSON
        // payload containing an explicit status === 'OK' (case-insensitive).
        // Examples that should be considered OFFLINE:
        // - HTTP 204 (No Content)
        // - HTTP 200 with JSON missing `status` or with status != 'OK'
        try {
          const j = r.json;
          const statusVal = j && j.status ? String(j.status).toLowerCase() : null;
          if (statusVal === 'ok') {
            // Accept and update deviceInfo only when status === 'OK'
            if (j.hw) window.deviceInfo.hw = j.hw;
            if (j.firmware) window.deviceInfo.firmware = j.firmware;
            if (j.version) window.deviceInfo.version = j.version;
            const devEl = document.getElementById('device-info');
            if (devEl) devEl.textContent = (window.deviceInfo && window.deviceInfo.firmware ? window.deviceInfo.firmware : 'ReadPaper') + ' ' +
                                         (window.deviceInfo && window.deviceInfo.version ? window.deviceInfo.version : 'V1.3') + ' @ ' +
                                         (window.deviceInfo && window.deviceInfo.hw ? window.deviceInfo.hw : 'M5Stack PaperS3');
            setOnline(true);
            refreshDeviceGuideCache(false);
          } else {
            // JSON present but not reporting OK — keep offline
            log('heartbeat: JSON status not OK, treating as offline', j);
            setOnline(false);
          }
        } catch (e) {
          // parse failure (e.g. 204 No Content) — treat as offline
          log('heartbeat: failed to parse JSON, treating as offline', e && e.message ? e.message : e);
          setOnline(false);
        }
      } else {
        setOnline(false);
      }
    }catch(e){
      setOnline(false);
    }
  }

  function applyStateToFileTab(state){
    const fileTab = document.querySelector('.tabs a[href="filemanager.html"]');
    // Because header may initially point filemanager->welcome, try either href
    let fileTabEl = fileTab || document.querySelector('.tabs a.disabled-until-heartbeat') || document.querySelector('.tabs a[href="welcome.html"]');
    if(!fileTabEl) return false;
    const statusEl = document.getElementById('connection-status');
    if(!state){
      fileTabEl.classList.remove('text-dark');
      fileTabEl.classList.add('text-light');
      fileTabEl.classList.add('disabled-until-heartbeat');
      fileTabEl.setAttribute('href', 'welcome.html');
      fileTabEl.setAttribute('aria-disabled', 'true');
      if(statusEl) statusEl.textContent = DEBUG_MODE ? 'DEBUG MODE：已放行设备管理入口，可离线调试 UI。' : '设备不在线，请确认设备已处于热点模式，而且本机已经连接。';
      log('device offline - UI applied');
    } else {
      fileTabEl.classList.remove('text-light');
      fileTabEl.classList.remove('disabled-until-heartbeat');
      fileTabEl.classList.add('text-dark');
      fileTabEl.setAttribute('href', 'filemanager.html');
      fileTabEl.setAttribute('aria-disabled', 'false');
      if(statusEl) statusEl.textContent = DEBUG_MODE ? 'DEBUG MODE：设备管理使用本地测试向量。' : '';
      log('device online - UI applied');
    }
    return true;
  }

  function setOnline(state){
    if(online === state) return;
    online = state;
    // show/hide device info element depending on online state
    try{
      const devEl = document.getElementById('device-info');
      if(devEl){
        if(state){
          // populate from window.deviceInfo if available
          try{
            devEl.textContent = (window.deviceInfo && window.deviceInfo.firmware ? window.deviceInfo.firmware : 'ReadPaper') + ' ' +
                                (window.deviceInfo && window.deviceInfo.version ? window.deviceInfo.version : 'V1.3') + ' @ ' +
                                (window.deviceInfo && window.deviceInfo.hw ? window.deviceInfo.hw : 'M5Stack PaperS3');
          }catch(_){ /* ignore */ }
          devEl.classList.add('show');
        } else {
          devEl.classList.remove('show');
        }
      }
    }catch(_){ }

    // try to apply immediately; if header not yet loaded, ensureApply will pick it up
    if(applyStateToFileTab(state)) fileTabApplied = true;

    // clear guide cache on offline transition to avoid stale capability maps
    if(!state && window.deviceGuideCache){
      window.deviceGuideCache.guide = null;
      window.deviceGuideCache.fetchedAt = 0;
    }
  }

  function offlineClickHandler(e){
    // redirect to welcome and show message
    e.preventDefault();
    try{ window.location.href = 'welcome.html'; }catch(_){ /* ignore */ }
    const statusEl = document.getElementById('connection-status');
    if(statusEl) statusEl.textContent = '设备不在线：已跳转到首页，请确认 Wi‑Fi 连接并重试。';
  }

  // delegated click guard: capture clicks on filemanager links even before header exists
  document.addEventListener('click', function(e){
    try{
      const a = e.target.closest && e.target.closest('.tabs a[href="filemanager.html"]');
      if(!a) return;
      if(!online && !DEBUG_MODE){
        e.preventDefault();
        // behave like offline click handler
        try{ window.location.href = 'welcome.html'; }catch(_){ }
        const statusEl = document.getElementById('connection-status');
        if(statusEl) statusEl.textContent = '设备不在线：已跳转到首页，请确认 Wi‑Fi 连接并重试。';
      }
    }catch(err){ /* ignore */ }
  }, true);

  // Start heartbeat: run immediately if DOM already ready, otherwise wait for DOMContentLoaded.
  function startHeartbeat(){
    // initial check — do one immediately, then periodic checks
    checkOnce();
    setInterval(checkOnce, INTERVAL_MS);

    // ensure that the fileTab's visual state is applied as soon as header partial is inserted
    const ensureInterval = setInterval(()=>{
      if(applyStateToFileTab(online)){
        fileTabApplied = true;
        clearInterval(ensureInterval);
      }
    }, 250);
    // stop trying after 10s to avoid infinite polling
    setTimeout(()=>{ if(!fileTabApplied) clearInterval(ensureInterval); }, 10000);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', startHeartbeat);
  } else {
    // DOM already ready (likely because script was injected after DOMContentLoaded)
    startHeartbeat();
  }

})();
