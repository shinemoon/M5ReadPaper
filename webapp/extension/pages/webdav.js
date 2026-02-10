(function(){
  const API_BASE = 'http://192.168.4.1';
  const urlEl = document.getElementById('webdavUrl');
  const userEl = document.getElementById('webdavUser');
  const passEl = document.getElementById('webdavPassword');
  const wifiSsidEl = document.getElementById('wifiSsid');
  const wifiPassEl = document.getElementById('wifiPassword');
  const btnReload = document.getElementById('btnReload');
  const btnSave = document.getElementById('btnSave');
  const btnWifiReload = document.getElementById('btnWifiReload');
  const btnWifiSave = document.getElementById('btnWifiSave');
  const btnToggle = document.getElementById('togglePassword');
  const btnToggleWifi = document.getElementById('toggleWifiPassword');
  const tabButtons = document.querySelectorAll('.config-tabs .tab-btn');

  const getNotePanel = (tabKey) => {
    if (tabKey) {
      const scoped = document.querySelector(`.tab-panel[data-tab="${tabKey}"] .webdav-note`);
      if (scoped) return scoped;
    }
    return document.querySelector('.tab-panel.is-active .webdav-note') || document.querySelector('.webdav-note');
  };

  // 统一的右侧说明区状态显示（单一元素，按需移动到目标 panel）
  let statusMsgEl = null;
  const setStatus = (text, type = 'info', tabKey = null) => {
    try {
      const panel = getNotePanel(tabKey);
      if (!panel) return;
      if (!statusMsgEl) {
        statusMsgEl = document.createElement('div');
        statusMsgEl.id = 'webdavNoteMsg';
        statusMsgEl.style.marginTop = '8px';
      }
      if (statusMsgEl.parentElement !== panel) {
        panel.appendChild(statusMsgEl);
      }
      statusMsgEl.textContent = text || '';
      statusMsgEl.className = '';
      if (type === 'success') statusMsgEl.classList.add('text-success');
      if (type === 'error') statusMsgEl.classList.add('text-error');
    } catch (e) {
      // ignore
    }
  };

  // 别名，保持兼容性
  const setNote = setStatus;

  const setWebdavDisabled = (disabled) => {
    if (btnReload) btnReload.disabled = disabled;
    if (btnSave) btnSave.disabled = disabled;
  };

  const setWifiDisabled = (disabled) => {
    if (btnWifiReload) btnWifiReload.disabled = disabled;
    if (btnWifiSave) btnWifiSave.disabled = disabled;
  };

  const b64EncodeUtf8 = (text) => {
    try {
      return btoa(unescape(encodeURIComponent(text)));
    } catch (e) {
      return null;
    }
  };

  const normalizeField = (value) => (value == null ? '' : String(value)).trim();

  async function loadConfig(){
    setWebdavDisabled(true);
    setStatus('读取中...', 'info', 'webdav');

    // 先从扩展本地存储读取配置（离线回退），立即填充表单以便在设备不可达时可见
    try {
      if (chrome && chrome.storage && chrome.storage.local) {
        await new Promise((resolve) => {
          chrome.storage.local.get('webdav_config', (res) => {
            const c = res && res.webdav_config ? res.webdav_config : null;
            if (c) {
              if (urlEl) urlEl.value = normalizeField(c.url);
              if (userEl) userEl.value = normalizeField(c.username);
              if (passEl) passEl.value = normalizeField(c.password);
              setStatus('已加载扩展本地配置', 'success', 'webdav');
            }
            resolve();
          });
        });
      }
    } catch (e) {
      // ignore
    }
    // 尝试基于本地配置检查/创建 /readpaper（不阻塞页面其余加载）
    try {
      const localUrl = urlEl ? urlEl.value.trim() : '';
      const localUser = userEl ? userEl.value.trim() : '';
      const localPass = passEl ? passEl.value : '';
      if (localUrl) {
        // 不用 await 阻塞后续设备同步，但保留 await 以便状态显示明确
        await ensureReadpaperDir(localUrl, localUser, localPass);
      }
    } catch (e) {
      // ignore
    }
    try {
      const r = await fetch(`${API_BASE}/api/webdav_config`, { method: 'GET' });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const j = await r.json();
      if (!j || j.ok !== true) throw new Error(j && j.message ? j.message : '读取失败');
      const cfg = j.config || {};
      if (urlEl) urlEl.value = normalizeField(cfg.url);
      if (userEl) userEl.value = normalizeField(cfg.username);
      if (passEl) passEl.value = normalizeField(cfg.password);
      setStatus('已同步设备配置', 'success', 'webdav');

      // 保存一份到扩展本地存储以便离线/回退使用
      try {
        if (chrome && chrome.storage && chrome.storage.local) {
          chrome.storage.local.set({ webdav_config: { url: normalizeField(cfg.url), username: normalizeField(cfg.username), password: normalizeField(cfg.password) } });
        }
      } catch (e) {
        // ignore
      }
    } catch (e) {
      // 读取设备配置失败，尝试从扩展本地存储回退
      try {
        if (chrome && chrome.storage && chrome.storage.local) {
          chrome.storage.local.get('webdav_config', (res) => {
            const c = res && res.webdav_config ? res.webdav_config : null;
            if (c) {
              if (urlEl) urlEl.value = normalizeField(c.url);
              if (userEl) userEl.value = normalizeField(c.username);
              if (passEl) passEl.value = normalizeField(c.password);
              setStatus('使用扩展本地配置（离线回退）', 'success', 'webdav');
              setWebdavDisabled(false);
              return;
            }
            setStatus(`读取失败: ${e.message}`, 'error', 'webdav');
          });
          // 已安排异步回退处理，直接 return
          return;
        }
      } catch (err) {
        // ignore and fallthrough
      }
      setStatus(`读取失败: ${e.message}`, 'error', 'webdav');
    } finally {
      setWebdavDisabled(false);
    }
  }

  // 检查并确保目标 WebDAV 上存在 /readpaper/ 目录，若缺失则尝试创建
  async function ensureReadpaperDir(url, username, password) {
    if (!url) return;
    let base = url.trim();
    if (!(base.startsWith('http://') || base.startsWith('https://'))) {
      setNote('WebDAV 地址无效（需 http/https）', 'error');
      return false;
    }
    if (!base.endsWith('/')) base += '/';
    const target = base + 'readpaper/';

    const authUser = (username || '').trim();
    const authPass = (password || '').trim();

    const makeHeaders = () => {
      const h = {};
      if (authUser || authPass) {
        const raw = authUser + ':' + authPass;
        const b64 = b64EncodeUtf8(raw);
        if (b64) {
          h['Authorization'] = 'Basic ' + b64;
        }
      }
      h['Depth'] = '0';
      h['Content-Type'] = 'text/xml';
      return h;
    };

    try {
      setStatus('检测 WebDAV /readpaper...', 'info', 'webdav');
      const headers = makeHeaders();
      const hasAuth = !!(headers && headers['Authorization']);
      // 在右侧说明区显示当前操作
      setNote('正在检测 WebDAV 可用性...', 'info', 'webdav');

      if (!authUser && !authPass) {
        // 未保存凭据，直接提示并返回（不弹窗）
        setNote('未检测到本地保存的 WebDAV 凭据，已跳过自动登录。', 'error', 'webdav');
        return false;
      }

      if (!headers || !headers['Authorization']) {
        setNote('本地凭据编码失败，已跳过自动登录。', 'error', 'webdav');
        return false;
      }
      // 先尝试 OPTIONS/PROPFIND 基址（参考设备侧逻辑）
      const opt = await fetch(base, {
        method: 'OPTIONS',
        headers: headers,
        redirect: 'manual',
        credentials: 'omit',
        cache: 'no-store'
      });
      const optAuth = opt.headers ? opt.headers.get('www-authenticate') : '';
      if (opt.status === 401) {
        setStatus('WebDAV 鉴权失败（401）', 'error', 'webdav');
        setNote(`WebDAV 鉴权失败（401）。${optAuth ? 'Auth: ' + optAuth : ''}`, 'error', 'webdav');
        return false;
      }

      const propBase = await fetch(base, {
        method: 'PROPFIND',
        headers: headers,
        redirect: 'manual',
        credentials: 'omit',
        cache: 'no-store'
      });
      const propBaseAuth = propBase.headers ? propBase.headers.get('www-authenticate') : '';
      if (propBase.status === 401) {
        setStatus('WebDAV 鉴权失败（401）', 'error', 'webdav');
        setNote(`WebDAV 鉴权失败（401）。${propBaseAuth ? 'Auth: ' + propBaseAuth : ''}`, 'error', 'webdav');
        return false;
      }

      // 再检查 /readpaper/
      const prop = await fetch(target, {
        method: 'PROPFIND',
        headers: headers,
        redirect: 'manual',
        credentials: 'omit',
        cache: 'no-store'
      });
      const propAuth = prop.headers ? prop.headers.get('www-authenticate') : '';
      if (prop.status === 207 || prop.status === 200) {
        setStatus('/readpaper 已存在', 'success', 'webdav');
        setNote('/readpaper 已存在', 'success', 'webdav');
        return true;
      }
      if (prop.status === 401) {
        setStatus('WebDAV 鉴权失败（401）', 'error', 'webdav');
        setNote(`WebDAV 鉴权失败（401）。${propAuth ? 'Auth: ' + propAuth : ''}`, 'error', 'webdav');
        return false;
      }
      // 若返回 404 或 405 等，尝试创建
      if (prop.status === 404 || prop.status === 405 || prop.status === 0) {
        setStatus('尝试创建 /readpaper...', 'info', 'webdav');
        const mk = await fetch(target, {
          method: 'MKCOL',
          headers: headers,
          redirect: 'manual',
          credentials: 'omit',
          cache: 'no-store'
        });
        if (mk.status === 201 || mk.status === 200) {
          setStatus('/readpaper 创建成功', 'success', 'webdav');
          setNote('/readpaper 创建成功', 'success', 'webdav');
          return true;
        }
        if (mk.status === 401) {
          setStatus('创建失败：鉴权失败（401）', 'error', 'webdav');
          setNote('创建 /readpaper 失败：鉴权失败（401）。请确认扩展本地保存的用户名/应用密码是否正确。', 'error', 'webdav');
          return false;
        }
        setStatus(`创建失败（HTTP ${mk.status}）`, 'error', 'webdav');
        setNote(`创建 /readpaper 失败（HTTP ${mk.status}）`, 'error', 'webdav');
        return false;
      }

      // 其他情况，报告状态码
      setStatus(`检查失败（HTTP ${prop.status}）`, 'error', 'webdav');
      return false;
    } catch (err) {
      setStatus(`访问 WebDAV 失败: ${err.message}`, 'error', 'webdav');
      return false;
    }
  }

  async function saveConfig(){
    setWebdavDisabled(true);
    setStatus('保存中...', 'info', 'webdav');
    try {
      const payload = {
        url: normalizeField(urlEl ? urlEl.value : ''),
        username: normalizeField(userEl ? userEl.value : ''),
        password: normalizeField(passEl ? passEl.value : '')
      };
      const r = await fetch(`${API_BASE}/api/webdav_config`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      const j = await r.json();
      if (!r.ok || !j || j.ok !== true) {
        throw new Error(j && j.message ? j.message : `HTTP ${r.status}`);
      }
      setStatus('已保存到设备', 'success', 'webdav');

      // 同步保存到扩展本地存储
      try {
        if (chrome && chrome.storage && chrome.storage.local) {
          chrome.storage.local.set({ webdav_config: { url: normalizeField(payload.url), username: normalizeField(payload.username), password: normalizeField(payload.password) } });
        }
      } catch (e) {
        // ignore
      }
    } catch (e) {
      setStatus(`保存失败: ${e.message}`, 'error', 'webdav');
    } finally {
      setWebdavDisabled(false);
    }
  }

  async function loadWifiConfig(){
    setWifiDisabled(true);
    setStatus('读取中...', 'info', 'wifi');

    try {
      if (chrome && chrome.storage && chrome.storage.local) {
        await new Promise((resolve) => {
          chrome.storage.local.get('wifi_config', (res) => {
            const c = res && res.wifi_config ? res.wifi_config : null;
            if (c) {
              if (wifiSsidEl) wifiSsidEl.value = normalizeField(c.ssid);
              if (wifiPassEl) wifiPassEl.value = normalizeField(c.password);
              setStatus('已加载扩展本地配置', 'success', 'wifi');
            }
            resolve();
          });
        });
      }
    } catch (e) {
      // ignore
    }

    try {
      const r = await fetch(`${API_BASE}/api/wifi_config`, { method: 'GET' });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const j = await r.json();
      if (!j || j.ok !== true) throw new Error(j && j.message ? j.message : '读取失败');
      const cfg = j.config || {};
      if (wifiSsidEl) wifiSsidEl.value = normalizeField(cfg.ssid);
      if (wifiPassEl) wifiPassEl.value = normalizeField(cfg.password);
      setStatus('已同步设备配置', 'success', 'wifi');

      try {
        if (chrome && chrome.storage && chrome.storage.local) {
          chrome.storage.local.set({ wifi_config: { ssid: normalizeField(cfg.ssid), password: normalizeField(cfg.password) } });
        }
      } catch (e) {
        // ignore
      }
    } catch (e) {
      try {
        if (chrome && chrome.storage && chrome.storage.local) {
          chrome.storage.local.get('wifi_config', (res) => {
            const c = res && res.wifi_config ? res.wifi_config : null;
            if (c) {
              if (wifiSsidEl) wifiSsidEl.value = normalizeField(c.ssid);
              if (wifiPassEl) wifiPassEl.value = normalizeField(c.password);
              setStatus('使用扩展本地配置（离线回退）', 'success', 'wifi');
              setWifiDisabled(false);
              return;
            }
            setStatus(`读取失败: ${e.message}`, 'error', 'wifi');
          });
          return;
        }
      } catch (err) {
        // ignore
      }
      setStatus(`读取失败: ${e.message}`, 'error', 'wifi');
    } finally {
      setWifiDisabled(false);
    }
  }

  async function saveWifiConfig(){
    setWifiDisabled(true);
    setStatus('保存中...', 'info', 'wifi');
    try {
      const payload = {
        ssid: normalizeField(wifiSsidEl ? wifiSsidEl.value : ''),
        password: normalizeField(wifiPassEl ? wifiPassEl.value : '')
      };
      const r = await fetch(`${API_BASE}/api/wifi_config`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      const j = await r.json();
      if (!r.ok || !j || j.ok !== true) {
        throw new Error(j && j.message ? j.message : `HTTP ${r.status}`);
      }
      setStatus('已保存到设备', 'success', 'wifi');

      try {
        if (chrome && chrome.storage && chrome.storage.local) {
          chrome.storage.local.set({ wifi_config: { ssid: payload.ssid, password: payload.password } });
        }
      } catch (e) {
        // ignore
      }
    } catch (e) {
      setStatus(`保存失败: ${e.message}`, 'error', 'wifi');
    } finally {
      setWifiDisabled(false);
    }
  }

  const activateTab = (tabKey) => {
    const panels = document.querySelectorAll('.tab-panel');
    panels.forEach((panel) => {
      const isMatch = panel.getAttribute('data-tab') === tabKey;
      panel.classList.toggle('is-active', isMatch);
      panel.classList.toggle('hidden', !isMatch);
    });
    tabButtons.forEach((btn) => {
      const isMatch = btn.getAttribute('data-tab') === tabKey;
      btn.classList.toggle('is-active', isMatch);
    });
  };

  if (btnReload) btnReload.addEventListener('click', loadConfig);
  if (btnSave) btnSave.addEventListener('click', saveConfig);
  if (btnWifiReload) btnWifiReload.addEventListener('click', loadWifiConfig);
  if (btnWifiSave) btnWifiSave.addEventListener('click', saveWifiConfig);
  if (btnToggle && passEl) {
    btnToggle.addEventListener('click', () => {
      const next = passEl.type === 'password' ? 'text' : 'password';
      passEl.type = next;
      btnToggle.setAttribute('aria-label', next === 'password' ? '显示密码' : '隐藏密码');
    });
  }
  if (btnToggleWifi && wifiPassEl) {
    btnToggleWifi.addEventListener('click', () => {
      const next = wifiPassEl.type === 'password' ? 'text' : 'password';
      wifiPassEl.type = next;
      btnToggleWifi.setAttribute('aria-label', next === 'password' ? '显示密码' : '隐藏密码');
    });
  }
  tabButtons.forEach((btn) => {
    btn.addEventListener('click', () => {
      const key = btn.getAttribute('data-tab');
      if (key) activateTab(key);
    });
  });

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', () => {
      activateTab('wifi');
      loadWifiConfig();
      loadConfig();
    });
  } else {
    activateTab('wifi');
    loadWifiConfig();
    loadConfig();
  }
})();
