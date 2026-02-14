(function(){
  const API_BASE = 'http://192.168.4.1';
  const urlEl = document.getElementById('webdavUrl');
  const userEl = document.getElementById('webdavUser');
  const passEl = document.getElementById('webdavPassword');

  // ========== Fetch 代理（通过 Background 绕过 CORS） ==========
  /**
   * 代理 fetch 请求到 background service worker
   * 用于 WebDAV 请求以绕过 CORS 限制
   * @param {string} url - 请求 URL
   * @param {object} options - fetch 选项
   * @param {boolean} skipPermissionRequest - 是否跳过权限请求（用于非用户交互的检测）
   * @returns {Promise<Response>} 模拟的 Response 对象
   */
  async function backgroundFetch(url, options = {}, skipPermissionRequest = false) {
    // 对于设备 API (192.168.4.1) 使用原生 fetch
    if (url.includes('192.168.4.1')) {
      return fetch(url, options);
    }
    
    // 检查权限状态
    if (!skipPermissionRequest) {
      try {
        const permissionGranted = await requestWebDAVPermission(url);
        if (!permissionGranted) {
          throw new Error('WebDAV 访问权限被拒绝');
        }
      } catch (permErr) {
        console.error('[backgroundFetch] 权限申请失败:', permErr);
        throw permErr;
      }
    } else {
      // 仅检查权限，不申请
      try {
        const urlObj = new URL(url);
        const origin = urlObj.origin + "/*";
        const hasPermission = await new Promise((resolve) => {
          if (!chrome || !chrome.permissions) {
            resolve(false);
            return;
          }
          chrome.permissions.contains({ origins: [origin] }, (result) => {
            resolve(result);
          });
        });
        
        if (!hasPermission) {
          throw new Error('没有 WebDAV 访问权限，请先点击「保存设置」按钮授权');
        }
      } catch (permErr) {
        console.warn('[backgroundFetch] 权限检查失败:', permErr);
        throw permErr;
      }
    }
    
    // 处理 Blob body：转换为 base64
    let processedOptions = { ...options };
    if (options.body && options.body instanceof Blob) {
      const base64Body = await new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onloadend = () => resolve(reader.result);
        reader.onerror = reject;
        reader.readAsDataURL(options.body);
      });
      processedOptions.body = base64Body;
      processedOptions._bodyIsBlob = true;
    }
    
    // 对于外部 WebDAV，通过 background 代理
    return new Promise((resolve, reject) => {
      chrome.runtime.sendMessage(
        { type: 'webdav_fetch', url, options: processedOptions },
        (response) => {
          if (chrome.runtime.lastError) {
            reject(new Error(chrome.runtime.lastError.message));
            return;
          }
          
          if (!response || !response.ok) {
            const error = new Error(response?.error || 'Background fetch failed');
            // 如果是权限错误，提供更友好的提示
            if (response?.error && response.error.includes('需要访问权限')) {
              error.message = '请先授予 WebDAV 访问权限。刷新页面后请在浏览器弹出的权限请求中点击"允许"。';
            }
            reject(error);
            return;
          }
          
          const bgResp = response.response;
          
          console.log('[backgroundFetch] 收到 background 响应:', {
            ok: bgResp.ok,
            status: bgResp.status,
            bodyType: bgResp.bodyType,
            bodyLength: typeof bgResp.body === 'string' ? bgResp.body.length : 
                        (bgResp.body ? JSON.stringify(bgResp.body).length : 0)
          });
          
          // 创建模拟的 Response 对象
          const mockResponse = {
            ok: bgResp.ok,
            status: bgResp.status,
            statusText: bgResp.statusText,
            url: bgResp.url,
            headers: {
              get: (name) => bgResp.headers[name.toLowerCase()] || null,
              has: (name) => name.toLowerCase() in bgResp.headers,
              forEach: (callback) => {
                Object.entries(bgResp.headers).forEach(([key, value]) => {
                  callback(value, key);
                });
              }
            },
            // 根据 bodyType 提供相应的方法
            text: async () => {
              console.log('[backgroundFetch] text() 被调用, bodyType:', bgResp.bodyType);
              if (bgResp.bodyType === 'text' || bgResp.bodyType === 'json') {
                const result = typeof bgResp.body === 'string' ? bgResp.body : JSON.stringify(bgResp.body);
                console.log('[backgroundFetch] text() 返回长度:', result.length, '预览:', result.substring(0, 100));
                return result;
              }
              if (bgResp.bodyType === 'blob') {
                // 尝试将 base64 解码为文本
                console.log('[backgroundFetch] text() 尝试解码 blob 为文本');
                try {
                  const base64 = bgResp.body;
                  const match = base64.match(/^data:([^;]+);base64,(.+)$/);
                  if (match) {
                    const mimeType = match[1];
                    const base64Data = match[2];
                    const byteCharacters = atob(base64Data);
                    console.log('[backgroundFetch] 成功解码 blob，长度:', byteCharacters.length, 'MIME:', mimeType);
                    return byteCharacters;
                  }
                } catch (e) {
                  console.error('[backgroundFetch] blob 解码失败:', e);
                }
              }
              return '';
            },
            json: async () => {
              if (bgResp.bodyType === 'json') {
                const result = typeof bgResp.body === 'string' ? JSON.parse(bgResp.body) : bgResp.body;
                console.log('[backgroundFetch] json() 返回对象，类型:', typeof result);
                return result;
              }
              if (bgResp.bodyType === 'text') {
                if (!bgResp.body || (typeof bgResp.body === 'string' && bgResp.body.trim() === '')) {
                  console.error('[backgroundFetch] json() 收到空文本');
                  throw new Error('Response body is empty');
                }
                console.log('[backgroundFetch] json() 解析文本:', bgResp.body.substring(0, 100));
                return JSON.parse(bgResp.body);
              }
              throw new Error('Response is not JSON');
            },
            blob: async () => {
              if (bgResp.bodyType === 'blob') {
                // 从 base64 还原 blob
                const base64 = bgResp.body;
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
                  return new Blob([byteArray], { type: mimeType });
                }
              }
              // 回退：从文本创建 blob
              const text = await mockResponse.text();
              return new Blob([text], { type: 'application/octet-stream' });
            }
          };
          
          resolve(mockResponse);
        }
      );
    });
  }

  // ========== 动态权限管理 ==========
  /**
   * 动态申请 WebDAV 访问权限
   * @param {string} url - WebDAV URL
   * @returns {Promise<boolean>} 是否成功获得权限
   */
  async function requestWebDAVPermission(url) {
    if (!url || !chrome || !chrome.permissions) {
      console.warn('无效的 URL 或浏览器不支持动态权限');
      return false;
    }

    try {
      // 解析 URL 获取 origin
      const urlObj = new URL(url);
      const origin = urlObj.origin + "/*";

      // 先检查是否已有权限
      const hasPermission = await new Promise((resolve) => {
        chrome.permissions.contains({ origins: [origin] }, (result) => {
          resolve(result);
        });
      });

      if (hasPermission) {
        console.log('已有访问权限:', origin);
        return true;
      }

      // 申请新权限（会弹出浏览器确认对话框）
      console.log('请求访问权限:', origin);
      const granted = await new Promise((resolve) => {
        chrome.permissions.request({ origins: [origin] }, (result) => {
          if (chrome.runtime.lastError) {
            console.error('权限申请错误:', chrome.runtime.lastError.message);
            resolve(false);
          } else {
            resolve(result);
          }
        });
      });

      if (granted) {
        console.log('权限已授予:', origin);
      } else {
        console.warn('权限被拒绝:', origin);
      }

      return granted;
    } catch (e) {
      console.error('权限申请失败:', e && e.message ? e.message : e);
      return false;
    }
  }
  
  // WiFi���置元素（3组）
  const wifiSsidEls = [
    document.getElementById('wifiSsid0'),
    document.getElementById('wifiSsid1'),
    document.getElementById('wifiSsid2')
  ];
  const wifiPassEls = [
    document.getElementById('wifiPassword0'),
    document.getElementById('wifiPassword1'),
    document.getElementById('wifiPassword2')
  ];
  
  const btnReload = document.getElementById('btnReload');
  const btnSave = document.getElementById('btnSave');
  const btnWifiReload = document.getElementById('btnWifiReload');
  const btnWifiSave = document.getElementById('btnWifiSave');
  const btnToggle = document.getElementById('togglePassword');
  const btnToggleWifiAll = document.querySelectorAll('.toggle-wifi-password');
  const tabButtons = document.querySelectorAll('.config-tabs .tab-btn');
  const refreshPeriodEl = document.getElementById('refreshPeriod');

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
        // 跳过权限申请，用于页面加载时的自动检查
        await ensureReadpaperDir(localUrl, localUser, localPass, true);
      }
    } catch (e) {
      // 如果权限不足，安静忽略，用户需点击「保存设置」按钮授权
      console.log('[页面加载] WebDAV 自动检查跳过:', e.message);
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
              updateLoadCurrentButtonState();
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
      updateLoadCurrentButtonState();
    }
  }

  // 检查并确保目标 WebDAV 上存在 /readpaper/ 目录，若缺失则尝试创建
  async function ensureReadpaperDir(url, username, password, skipPermissionRequest = false) {
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
      const opt = await backgroundFetch(base, {
        method: 'OPTIONS',
        headers: headers,
        redirect: 'manual',
        credentials: 'omit',
        cache: 'no-store'
      }, skipPermissionRequest);
      const optAuth = opt.headers ? opt.headers.get('www-authenticate') : '';
      if (opt.status === 401) {
        setStatus('WebDAV 鉴权失败（401）', 'error', 'webdav');
        setNote(`WebDAV 鉴权失败（401）。${optAuth ? 'Auth: ' + optAuth : ''}`, 'error', 'webdav');
        return false;
      }

      const propBase = await backgroundFetch(base, {
        method: 'PROPFIND',
        headers: headers,
        redirect: 'manual',
        credentials: 'omit',
        cache: 'no-store'
      }, skipPermissionRequest);
      const propBaseAuth = propBase.headers ? propBase.headers.get('www-authenticate') : '';
      if (propBase.status === 401) {
        setStatus('WebDAV 鉴权失败（401）', 'error', 'webdav');
        setNote(`WebDAV 鉴权失败（401）。${propBaseAuth ? 'Auth: ' + propBaseAuth : ''}`, 'error', 'webdav');
        return false;
      }

      // 再检查 /readpaper/
      const prop = await backgroundFetch(target, {
        method: 'PROPFIND',
        headers: headers,
        redirect: 'manual',
        credentials: 'omit',
        cache: 'no-store'
      }, skipPermissionRequest);
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
        const mk = await backgroundFetch(target, {
          method: 'MKCOL',
          headers: headers,
          redirect: 'manual',
          credentials: 'omit',
          cache: 'no-store'
        }, skipPermissionRequest);
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
      
      // 更新按钮状态
      updateLoadCurrentButtonState();
      updateUploadButtonState();
    } catch (e) {
      setStatus(`保存失败: ${e.message}`, 'error', 'webdav');
    } finally {
      setWebdavDisabled(false);
    }
  }

  // Helper: read refreshPeriod from input and clamp to allowed range
  function readRefreshPeriod() {
    try {
      const v = refreshPeriodEl ? parseInt(refreshPeriodEl.value, 10) : NaN;
      if (isNaN(v)) return 30;
      return Math.min(1440, Math.max(10, v));
    } catch (e) {
      return 30;
    }
  }

  async function loadWifiConfig(){
    setWifiDisabled(true);
    setStatus('读取中...', 'info', 'wifi');

    try {
      if (chrome && chrome.storage && chrome.storage.local) {
        await new Promise((resolve) => {
          chrome.storage.local.get('wifi_configs', (res) => {
            const configs = res && res.wifi_configs ? res.wifi_configs : null;
            if (configs && Array.isArray(configs)) {
              for (let i = 0; i < 3 && i < configs.length; i++) {
                if (wifiSsidEls[i]) wifiSsidEls[i].value = normalizeField(configs[i].ssid);
                if (wifiPassEls[i]) wifiPassEls[i].value = normalizeField(configs[i].password);
              }
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
      
      const configs = j.configs || [];
      const lastSuccessIdx = j.last_success_idx || -1;
      
      for (let i = 0; i < 3; i++) {
        if (wifiSsidEls[i]) wifiSsidEls[i].value = i < configs.length ? normalizeField(configs[i].ssid) : '';
        if (wifiPassEls[i]) wifiPassEls[i].value = i < configs.length ? normalizeField(configs[i].password) : '';
        
        // 标记最近成功的配置
        const group = wifiSsidEls[i]?.closest('.wifi-config-group');
        if (group) {
          if (i === lastSuccessIdx) {
            group.classList.add('last-success');
          } else {
            group.classList.remove('last-success');
          }
        }
      }
      
      setStatus('已同步设备配置' + (lastSuccessIdx >= 0 ? ` (最近成功: WiFi ${lastSuccessIdx + 1})` : ''), 'success', 'wifi');

      try {
        if (chrome && chrome.storage && chrome.storage.local) {
          const storageConfigs = [];
          for (let i = 0; i < 3; i++) {
            storageConfigs.push({
              ssid: i < configs.length ? normalizeField(configs[i].ssid) : '',
              password: i < configs.length ? normalizeField(configs[i].password) : ''
            });
          }
          chrome.storage.local.set({ wifi_configs: storageConfigs });
        }
      } catch (e) {
        // ignore
      }
    } catch (e) {
      try {
        if (chrome && chrome.storage && chrome.storage.local) {
          chrome.storage.local.get('wifi_configs', (res) => {
            const configs = res && res.wifi_configs ? res.wifi_configs : null;
            if (configs && Array.isArray(configs)) {
              for (let i = 0; i < 3 && i < configs.length; i++) {
                if (wifiSsidEls[i]) wifiSsidEls[i].value = normalizeField(configs[i].ssid);
                if (wifiPassEls[i]) wifiPassEls[i].value = normalizeField(configs[i].password);
              }
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
      const configs = [];
      for (let i = 0; i < 3; i++) {
        configs.push({
          ssid: normalizeField(wifiSsidEls[i] ? wifiSsidEls[i].value : ''),
          password: normalizeField(wifiPassEls[i] ? wifiPassEls[i].value : '')
        });
      }
      
      const payload = { configs: configs };
      
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
          chrome.storage.local.set({ wifi_configs: configs });
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
    
    // 切换到展示配置时，更新"读取当前"和"更新到WebDAV"按钮状态，以及背景预览
    if (tabKey === 'display') {
      updateLoadCurrentButtonState();
      updateUploadButtonState();
      // 延迟一下确保 DOM 已经显示
      setTimeout(() => {
        const canvas = document.getElementById('bgCanvas');
        if (canvas && (canvas.width === 0 || canvas.height === 0)) {
          const container = document.getElementById('screenPreviewContainer');
          if (container) {
            const rect = container.getBoundingClientRect();
            canvas.width = rect.width || 450;
            canvas.height = rect.height || 800;
          }
        }
        updateBackgroundPreview();
      }, 100);
    }
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
  
  // WiFi密码显示/隐藏（支持3组）
  btnToggleWifiAll.forEach((btn) => {
    btn.addEventListener('click', () => {
      const targetId = btn.getAttribute('data-target');
      const targetEl = document.getElementById(targetId);
      if (targetEl) {
        const next = targetEl.type === 'password' ? 'text' : 'password';
        targetEl.type = next;
        btn.setAttribute('aria-label', next === 'password' ? '显示密码' : '隐藏密码');
      }
    });
  });
  tabButtons.forEach((btn) => {
    btn.addEventListener('click', () => {
      const key = btn.getAttribute('data-tab');
      if (key) activateTab(key);
    });
  });

  // ========== 展示配置相关 ==========
  const SCREEN_COLS = 9;
  const SCREEN_ROWS = 16;
  let selectedCell = null; // 当前选中的单元格（单选模式）
  let components = []; // 已添加的组件列表
  let expandedComponentId = null; // 当前展开的组件 id
  let backgroundImage = null; // 背景图（base64 或 URL）
  let hasBgPic = false; // 是否启用背景图
  let availableFonts = []; // 系统可用字体列表

  // 获取系统字体列表
  async function loadSystemFonts() {
    try {
      // 尝试使用 Font Access API
      if (typeof window.queryLocalFonts === 'function' || typeof navigator.queryLocalFonts === 'function') {
        console.log('使用 Font Access API 获取字体...');

        // 先尝试安全查询权限（部分环境会抛出 DOMException）
        let permAllowed = false;
        try {
          if (navigator.permissions && typeof navigator.permissions.query === 'function') {
            try {
              const status = await navigator.permissions.query({ name: 'local-fonts' });
              permAllowed = (status && (status.state === 'granted' || status.state === 'prompt'));
            } catch (permErr) {
              console.warn('查询 local-fonts 权限失败:', permErr && permErr.message ? permErr.message : permErr);
              // 若权限查询本身失败，仍然尝试调用 API（某些环境不支持 permissions，但支持 queryLocalFonts）
              permAllowed = true;
            }
          } else {
            permAllowed = true;
          }

          if (permAllowed) {
            const q = window.queryLocalFonts || navigator.queryLocalFonts;
            if (typeof q === 'function') {
              const fonts = await q();
              const fontFamilies = new Set();
              fonts.forEach(font => {
                if (font && font.family) fontFamilies.add(font.family);
              });
              availableFonts = Array.from(fontFamilies).sort();
              console.log(`获取到 ${availableFonts.length} 个系统字体`);
              return;
            }
          }
        } catch (e) {
          console.warn('Font Access API 获取字体失败:', e && e.message ? e.message : e);
          // 继续执行回退检测
        }
      }
    } catch (e) {
      console.warn('Font Access API 不可用:', e && e.message ? e.message : e);
    }
    
    // 回退方案：通过 canvas 检测字体是否实际可用
    console.log('使用 Canvas 检测可用字体...');
    const testFonts = [
      // 英文字体
      'Arial', 'Arial Black', 'Arial Narrow', 'Arial Rounded MT Bold',
      'Helvetica', 'Helvetica Neue',
      'Times', 'Times New Roman',
      'Georgia', 'Garamond', 'Palatino', 'Palatino Linotype',
      'Courier', 'Courier New',
      'Verdana', 'Geneva', 'Tahoma',
      'Trebuchet MS',
      'Comic Sans MS', 'Comic Sans',
      'Impact', 'Charcoal',
      'Lucida Console', 'Lucida Sans', 'Lucida Grande', 'Lucida Sans Unicode',
      'Monaco', 'Consolas', 'Menlo',
      'Brush Script MT',
      'Copperplate', 'Papyrus',
      'Century Gothic', 'Century Schoolbook', 'Futura',
      'Gill Sans', 'Optima',
      'Cambria', 'Calibri', 'Candara', 'Constantia', 'Corbel',
      'Segoe UI', 'Segoe Print', 'Segoe Script',
      'Franklin Gothic Medium',
      'Book Antiqua', 'Bookman Old Style',
      'MS Sans Serif', 'MS Serif',
      'System', 'System-ui', 'sans-serif', 'serif', 'monospace',
      'Rockwell', 'Rockwell Extra Bold',
      'Baskerville',
      'Didot',
      'Hoefler Text',
      'American Typewriter',
      'Andale Mono',
      'Courier Prime',
      'Noto Sans', 'Noto Serif',
      // 中文字体（只保留中文名）
      '微软雅黑',
      '微软雅黑 Light',
      '黑体',
      '宋体',
      '新宋体',
      '楷体',
      '仿宋',
      '幼圆',
      '华文仿宋',
      '华文楷体',
      '华文宋体',
      '华文黑体',
      '华文中宋',
      '华文细黑',
      '华文新魏',
      '华文琥珀',
      '华文行楷',
      '华文隶书',
      '苹方-简',
      '苹方-繁',
      '苹方 SC',
      '苹方 TC',
      '苹方 HK',
      '思源黑体',
      '思源宋体',
      '文泉驿微米黑',
      '文泉驿正黑',
      '文泉驿点阵正黑',
      '冬青黑体简体中文',
      '兰亭黑-简',
      '兰亭黑-繁',
      '娃娃体-简',
      '手札体-简',
      '翩翩体-简',
      '圆体-简',
      '方正兰亭黑',
      '方正舒体',
      '方正姚体',
      '方正书宋',
      '方正黑体',
      '方正楷体',
      '隶书',
      '汉仪旗黑',
      '汉仪大宋简',
      '汉仪楷体简',
      '印品雅圆体',
      '造字工房悦黑',
      '造字工房尚黑',
      '站酷高端黑',
      '站酷快乐体',
      'Noto Sans CJK SC',
      'Noto Sans CJK TC',
      'Noto Serif CJK SC',
      'Noto Serif CJK TC'
    ];
    
    availableFonts = detectAvailableFonts(testFonts);
    console.log(`检测到 ${availableFonts.length} 个可用字体`);
  }
  
  // 通过 Canvas 检测字体是否可用
  function detectAvailableFonts(fontList) {
    const canvas = document.createElement('canvas');
    const context = canvas.getContext('2d');
    const testStrings = [
      'mmmmmmmmmmlli',
      '中文测试字体',
      'abcdefghijklmnopqrstuvwxyz',
      '0123456789'
    ];
    const testSize = '72px';
    const baseFonts = ['monospace', 'sans-serif', 'serif'];
    
    // 测量基准字体的宽度
    const baseWidths = {};
    baseFonts.forEach(baseFont => {
      baseWidths[baseFont] = [];
      testStrings.forEach(testString => {
        context.font = testSize + ' ' + baseFont;
        baseWidths[baseFont].push(context.measureText(testString).width);
      });
    });
    
    // 检测每个字体
    const available = [];
    fontList.forEach(font => {
      let detected = false;
      
      for (let baseFont of baseFonts) {
        for (let i = 0; i < testStrings.length; i++) {
          context.font = testSize + ' "' + font + '",' + baseFont;
          const width = context.measureText(testStrings[i]).width;
          
          // 如果宽度与基准字体不同，说明该字体存在
          if (Math.abs(width - baseWidths[baseFont][i]) > 0.1) {
            detected = true;
            break;
          }
        }
        if (detected) break;
      }
      
      if (detected) {
        available.push(font);
      }
    });
    
    return available.sort();
  }

  // 初始化屏幕预览
  function initScreenPreview() {
    const preview = document.getElementById('screenPreview');
    const canvas = document.getElementById('bgCanvas');
    if (!preview) return;

    preview.innerHTML = '';
    preview.style.display = 'grid';
    preview.style.gridTemplateColumns = `repeat(${SCREEN_COLS}, 1fr)`;
    preview.style.gridTemplateRows = `repeat(${SCREEN_ROWS}, 1fr)`;

    // 初始化背景canvas
    if (canvas) {
      // 等待容器渲染后再设置 canvas 尺寸
      setTimeout(() => {
        const container = document.getElementById('screenPreviewContainer');
        if (container) {
          const rect = container.getBoundingClientRect();
          canvas.width = rect.width || 450;
          canvas.height = rect.height || 800;
        }
      }, 100);
    }

    for (let row = 0; row < SCREEN_ROWS; row++) {
      for (let col = 0; col < SCREEN_COLS; col++) {
        const cell = document.createElement('div');
        cell.className = 'screen-cell';
        cell.dataset.row = row;
        cell.dataset.col = col;
        cell.addEventListener('click', () => toggleCellSelection(row, col));
        preview.appendChild(cell);
      }
    }

    updateBackgroundPreview();
  }

  // 更新背景预览
  function updateBackgroundPreview() {
    const canvas = document.getElementById('bgCanvas');
    const preview = document.getElementById('screenPreview');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    
    // 确保 canvas 有正确的尺寸
    if (canvas.width === 0 || canvas.height === 0) {
      const container = document.getElementById('screenPreviewContainer');
      if (container) {
        const rect = container.getBoundingClientRect();
        canvas.width = rect.width || 450;
        canvas.height = rect.height || 800;
        console.log('Canvas 尺寸初始化:', canvas.width, 'x', canvas.height);
      }
    }
    
    // 绘制预览组件（包含高亮和文本）
    const drawPreviewComponents = () => {
      // 计算单元格尺寸（预览画布）
      const cellWidth = canvas.width / SCREEN_COLS;
      const cellHeight = canvas.height / SCREEN_ROWS;
      
      // 计算偏移量的缩放比例（设备实际是540x960，预览画布可能不同）
      const offsetScaleX = canvas.width / 540;
      const offsetScaleY = canvas.height / 960;
      // 计算每个组件的添加序号映射（按 id 升序），以便预览中显示与组件列表一致的添加序号
      const byIdAscPreview = [...components].sort((a, b) => a.id - b.id);
      const idToIndexPreview = new Map();
      byIdAscPreview.forEach((c, i) => idToIndexPreview.set(c.id, i + 1));
      
      components.forEach((comp, idx) => {
        const x = comp.col * cellWidth + (comp.xOffset || 0) * offsetScaleX;
        const y = comp.row * cellHeight + (comp.yOffset || 0) * offsetScaleY;
        const w = comp.width * cellWidth;
        const h = comp.height * cellHeight;
        
        // 对于动态文本（dynamic_text, daily_poem, list, rss, reading_status, weather），绘制半透明高亮+标签，不渲染真实文本
          if (comp.type === 'dynamic_text' || comp.type === 'daily_poem' || comp.type === 'list' || comp.type === 'rss' || comp.type === 'reading_status' || comp.type === 'weather') {
          // 半透明高亮（今日诗词用蓝色，列表用绿色，RSS用橙色，天气用天蓝色，普通文本用黄色）
          if (comp.type === 'daily_poem') {
            ctx.fillStyle = 'rgba(100, 149, 237, 0.3)';  // 蓝色
          } else if (comp.type === 'list') {
            ctx.fillStyle = 'rgba(76, 175, 80, 0.3)';  // 绿色
          } else if (comp.type === 'rss') {
            ctx.fillStyle = 'rgba(255, 152, 0, 0.3)';  // 橙色
          } else if (comp.type === 'weather') {
            ctx.fillStyle = 'rgba(33, 150, 243, 0.3)';  // 天蓝色
          } else {
            ctx.fillStyle = 'rgba(251, 192, 45, 0.3)';  // 黄色
          }
          ctx.fillRect(x, y, w, h);
          
          // 在左上角显示标签（使用添加序号，与组件列表一致）
          const typeLabel = getComponentTypeLabel(comp.type);
          const addIndexPreview = idToIndexPreview.get(comp.id) || (idx + 1);
          ctx.fillStyle = 'rgba(0, 0, 0, 0.7)';
          ctx.font = '14px Arial, sans-serif';
          ctx.textAlign = 'left';
          ctx.textBaseline = 'top';
          ctx.fillText(`组件${addIndexPreview}-${typeLabel}`, x + 4, y + 4);
          return;  // 跳过后续的旋转渲染
        }
        
        // 绘制分割线或预渲染文本组件（支持旋转）
        const cx = x + w / 2;
        const cy = y + h / 2;
        const angle = (comp.rotation || 0) * Math.PI / 180;
        ctx.save();
        ctx.translate(cx, cy);
        ctx.rotate(angle);

        // 分割线预览
        if (comp.type === 'divider') {
          const lineGray = (comp.lineColor || 0) * 17;
          ctx.strokeStyle = `rgb(${lineGray}, ${lineGray}, ${lineGray})`;
          ctx.lineWidth = 2 * (canvas.width / 540);  // 按比例缩放线宽
          
          // 设置线条样式
          const lineStyle = comp.lineStyle || 'solid';
          const dashScale = canvas.width / 540;
          if (lineStyle === 'dashed') {
            ctx.setLineDash([10 * dashScale, 5 * dashScale]);
          } else if (lineStyle === 'dotted') {
            ctx.setLineDash([2 * dashScale, 3 * dashScale]);
          } else if (lineStyle === 'double') {
            ctx.setLineDash([]);
            const offset = 2 * dashScale;
            // 绘制双线
            ctx.beginPath();
            ctx.moveTo(-w/2, -offset);
            ctx.lineTo(w/2, -offset);
            ctx.stroke();
            ctx.beginPath();
            ctx.moveTo(-w/2, offset);
            ctx.lineTo(w/2, offset);
            ctx.stroke();
          } else {
            ctx.setLineDash([]);
          }
          
          // 绘制单线（非double样式）
          if (lineStyle !== 'double') {
            ctx.beginPath();
            ctx.moveTo(-w/2, 0);
            ctx.lineTo(w/2, 0);
            ctx.stroke();
          }
        }
        // 预渲染文本内容背景色
        else if (comp.text && comp.type === 'text') {
          if (comp.bgColor !== 'transparent' && comp.bgColor !== undefined) {
            const bgGray = (comp.bgColor || 0) * 17;
            ctx.fillStyle = `rgb(${bgGray}, ${bgGray}, ${bgGray})`;
            ctx.fillRect(-w/2, -h/2, w, h);
          }

          // 文本
          const textGray = (comp.textColor || 0) * 17;
          ctx.fillStyle = `rgb(${textGray}, ${textGray}, ${textGray})`;
          const fontSize = (comp.fontSize || 24) * (canvas.width / 540);
          const fontFamily = comp.fontFamily || 'Arial';
          ctx.font = `${fontSize}px ${fontFamily}, sans-serif`;
          ctx.textAlign = 'center';
          ctx.textBaseline = 'middle';

          const maxCharsPerLine = Math.floor(w / (fontSize * 0.6));
          const lines = [];
          let currentLine = '';
          for (let i = 0; i < comp.text.length; i++) {
            currentLine += comp.text[i];
            if (currentLine.length >= maxCharsPerLine || i === comp.text.length - 1) {
              lines.push(currentLine);
              currentLine = '';
            }
          }

          const lineHeight = fontSize * 1.2;
          const startYLocal = -(lines.length - 1) * lineHeight / 2;
          lines.forEach((line, idx) => {
            ctx.fillText(line, 0, startYLocal + idx * lineHeight);
          });
        }

        ctx.restore();
      });
    };
    
    ctx.clearRect(0, 0, canvas.width, canvas.height);

    if (backgroundImage && hasBgPic) {
      const img = new Image();
      img.onload = function() {
        ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
        drawPreviewComponents(); // 在背景之上绘制组件
      };
      img.onerror = function() {
        console.error('背景图加载失败');
        drawPreviewComponents(); // 即使背景图失败也绘制组件
      };
      img.src = backgroundImage;
      
      // 添加 has-background class
      if (preview && !preview.classList.contains('has-background')) {
        preview.classList.add('has-background');
      }
    } else {
      drawPreviewComponents(); // 无背景图时也绘制组件
      // 移除 has-background class
      if (preview && preview.classList.contains('has-background')) {
        preview.classList.remove('has-background');
      }
    }
  }

  // 切换单元格选中状态（单选模式）
  function toggleCellSelection(row, col) {
    // 清除之前的选中
    if (selectedCell) {
      const oldCell = document.querySelector(`.screen-cell[data-row="${selectedCell.row}"][data-col="${selectedCell.col}"]`);
      if (oldCell) {
        oldCell.classList.remove('selected');
      }
    }
    
    // 如果点击的是同一个单元格，则取消选中
    if (selectedCell && selectedCell.row === row && selectedCell.col === col) {
      selectedCell = null;
      return;
    }
    
    // 选中新单元格
    selectedCell = { row, col };
    const cell = document.querySelector(`.screen-cell[data-row="${row}"][data-col="${col}"]`);
    if (cell) {
      cell.classList.add('selected');
    }
  }

  // 更新屏幕预览，显示组件占用情况
  function updateScreenPreview() {
    // 清除所有occupied标记
    document.querySelectorAll('.screen-cell').forEach(cell => {
      cell.classList.remove('occupied');
      cell.title = '';
    });

    // 标记被组件占用的单元格
      // 所有组件均为非排他，不再将任何单元格标记为 occupied
      // 如果将来需要显示占用信息，可在此处添加视觉提示（目前保留为空）。
  }

  // 添加组件
  function addComponent() {
    if (!selectedCell) {
      alert('请先在左侧预览区域选择起始位置');
      return;
    }

    const minRow = selectedCell.row;
    const minCol = selectedCell.col;

    const type = document.getElementById('componentType')?.value || 'text';
    const width = parseInt(document.getElementById('componentWidth')?.value || 3);
    const height = parseInt(document.getElementById('componentHeight')?.value || 2);

    // 检查是否超出边界
    if (minCol + width > SCREEN_COLS) {
      alert(`组件宽度超出屏幕范围（${minCol + 1} + ${width} > ${SCREEN_COLS}）`);
      return;
    }
    if (minRow + height > SCREEN_ROWS) {
      alert(`组件高度超出屏幕范围（${minRow + 1} + ${height} > ${SCREEN_ROWS}）`);
      return;
    }

    // 允许组件交叠，按添加顺序渲染（后添加的在上层）

    const component = {
      id: Date.now(),
      type: type,
      row: minRow,
      col: minCol,
      width: width,
      height: height,
      text: (type === 'daily_poem' || type === 'divider' || type === 'reading_status' || type === 'weather') ? '' : (type === 'list' ? '项目1;项目2;项目3' : (type === 'rss' ? 'https://example.com/feed.xml' : '示例文本')),  // 今日诗词、分割线、阅读状态和天气不需要文本输入，列表默认示例，RSS默认URL
      fontSize: (type === 'dynamic_text' || type === 'daily_poem' || type === 'list' || type === 'rss' || type === 'reading_status' || type === 'weather') ? 24 : 24,  // 动态文本默认24
      fontFamily: (type === 'dynamic_text' || type === 'daily_poem' || type === 'list' || type === 'rss' || type === 'reading_status' || type === 'weather') ? '' : 'Arial',  // 动态文本、列表、RSS、阅读状态和天气不支持字体选择
      textColor: 0,  // 0-15 灰度级别，0=黑色，15=白色
      bgColor: (type === 'dynamic_text' || type === 'daily_poem' || type === 'reading_status' || type === 'weather') ? 15 : 'transparent',  // 动态文本、阅读状态和天气默认白色背景(15)，列表和RSS透明
      align: (type === 'dynamic_text' || type === 'daily_poem' || type === 'reading_status' || type === 'weather') ? 'left' : undefined,  // 动态文本、阅读状态与天气支持对齐，列表和RSS不支持
      rotation: (type === 'dynamic_text' || type === 'daily_poem' || type === 'reading_status' || type === 'weather' || type === 'divider') ? 0 : 0,  // 动态文本和分割线支持旋转
      xOffset: 0,  // x偏移量（像素）
      yOffset: 0,  // y偏移量（像素）
      lineColor: type === 'divider' ? 0 : undefined,  // 分割线颜色（0-15灰度）
      lineStyle: type === 'divider' ? 'solid' : undefined,  // 分割线样式
      lineWidth: type === 'divider' ? 2 : undefined,  // 分割线粗细（像素）
      margin: (type === 'list' || type === 'rss') ? 10 : undefined,  // 列表和RSS行间距（像素）
      citycode: type === 'weather' ? '110000' : undefined,  // 天气组件城市代码（默认北京）
      apiKey: type === 'weather' ? '' : undefined,  // 天气组件高德API Key
      dynamic: type !== 'text' && type !== 'divider'  // text和divider默认为false（预渲染），其他类型为true（设备动态渲染）
    };

    // 将新组件添加到数组开头，确保最新组件排在最前面
    components.unshift(component);
    
    // 清除选中状态
    if (selectedCell) {
      const cell = document.querySelector(`.screen-cell[data-row="${selectedCell.row}"][data-col="${selectedCell.col}"]`);
      if (cell) {
        cell.classList.remove('selected');
      }
    }
    selectedCell = null;

    updateScreenPreview();
    updateComponentList();
  }

  // 将组件类型转换为中文标签
  function getComponentTypeLabel(type) {
    switch ((type || '').toLowerCase()) {
      case 'text':
        return '预渲染文本';
      case 'dynamic_text':
        return '普通文本';
      case 'daily_poem':
        return '今日诗词';
      case 'reading_status':
        return '阅读状态';
      case 'weather':
        return '天气查询';
      case 'list':
        return '列表';
      case 'rss':
        return 'RSS订阅';
      case 'divider':
        return '分割线';
      case 'image':
        return '图片';
      case 'video':
        return '视频';
      case 'clock':
        return '时钟';
      case 'barcode':
        return '条码';
      default:
        // 首字母大写作为兜底
        return type ? (type.charAt(0).toUpperCase() + type.slice(1)) : '组件';
    }
  }

  // 更新组件列表显示
  function updateComponentList() {
    console.log('[updateComponentList] 开始执行，components.length =', components.length);
    
    const listEl = document.getElementById('componentList');
    if (!listEl) {
      console.error('[updateComponentList] 找不到 componentList 元素！');
      return;
    }

    console.log('[updateComponentList] 找到 componentList 元素');

    // 保留标题
    listEl.innerHTML = '<h6>已添加组件</h6>';

    if (components.length === 0) {
      console.log('[updateComponentList] 组件列表为空');
      listEl.innerHTML += '<p class="muted">暂无组件</p>';
      return;
    }

    console.log('[updateComponentList] 开始渲染', components.length, '个组件');

    // 计算每个组件的添加序号（按 id 升序），以保持 UI 编号按添加顺序显示
    const byIdAsc = [...components].sort((a, b) => a.id - b.id);
    const idToIndex = new Map();
    byIdAsc.forEach((c, i) => idToIndex.set(c.id, i + 1));

    // 按组件ID从大到小排序，用于展示（最新的在前）
    const sortedComponents = [...components].sort((a, b) => b.id - a.id);

    sortedComponents.forEach((comp, idx) => {
      const item = document.createElement('div');
      item.className = 'component-item';
      item.dataset.componentId = comp.id;
      
      const header = document.createElement('div');
      header.className = 'component-item-header';
      header.style.cursor = 'pointer';
      header.style.userSelect = 'none';
      
      const toggleIcon = document.createElement('span');
      toggleIcon.className = 'toggle-icon';
      toggleIcon.textContent = '▶';
      toggleIcon.style.display = 'inline-block';
      toggleIcon.style.marginRight = '8px';
      toggleIcon.style.transition = 'transform 0.2s';
      
      const title = document.createElement('strong');
      const typeLabel = getComponentTypeLabel(comp.type);
      // 使用添加序号（按创建时间升序分配），使组件标题反映添加顺序
      const addIndex = idToIndex.get(comp.id) || (idx + 1);
      title.textContent = `组件 ${addIndex} - ${typeLabel}`;
      
      const info = document.createElement('span');
      info.className = 'muted';
      info.textContent = `位置: (${comp.col + 1}, ${comp.row + 1}) | 尺寸: ${comp.width}x${comp.height}`;
      
      const deleteBtn = document.createElement('button');
      deleteBtn.className = 'button is-small error';
      deleteBtn.textContent = '删除';
      deleteBtn.dataset.componentId = comp.id;
      deleteBtn.style.marginLeft = 'auto';
      
      header.appendChild(toggleIcon);
      header.appendChild(title);
      header.appendChild(info);
      header.appendChild(deleteBtn);
      
      // 文本内容输入（今日诗词不需要文本输入）
      const field = document.createElement('div');
      field.className = 'field';
      
      const textLabel = document.createElement('label');
      // 根据组件类型设置标签文本
      if (comp.type === 'list') {
        textLabel.textContent = '列表内容（分号分隔）';
      } else if (comp.type === 'rss') {
        textLabel.textContent = 'RSS Feed URL';
      } else {
        textLabel.textContent = '文本内容';
      }
      
      const input = document.createElement('input');
      input.type = 'text';
      input.value = comp.text || '';
      input.dataset.componentId = comp.id;
      input.className = 'component-text-input';
      
      field.appendChild(textLabel);
      field.appendChild(input);
      
      // 字体大小
      const sizeField = document.createElement('div');
      sizeField.className = 'field';
      
      const sizeLabel = document.createElement('label');
      sizeLabel.textContent = '字体大小';
      
      const sizeInput = document.createElement('input');
      sizeInput.type = 'number';
      sizeInput.value = comp.fontSize || 24;
      // 动态文本（dynamic_text, daily_poem, list, rss, reading_status, weather）限制字体大小24-38，预渲染文本12-72
      if (comp.type === 'dynamic_text' || comp.type === 'daily_poem' || comp.type === 'list' || comp.type === 'rss' || comp.type === 'reading_status' || comp.type === 'weather') {
        sizeInput.min = 24;
        sizeInput.max = 38;
      } else {
        sizeInput.min = 12;
        sizeInput.max = 72;
      }
      sizeInput.dataset.componentId = comp.id;
      sizeInput.className = 'component-fontsize-input';
      
      sizeField.appendChild(sizeLabel);
      sizeField.appendChild(sizeInput);
      
      // 列 / 行 位置选择
      const posField = document.createElement('div');
      posField.className = 'field';

      const colLabel = document.createElement('label');
      colLabel.textContent = '列 (1-' + SCREEN_COLS + ')';

      const colSelect = document.createElement('select');
      colSelect.dataset.componentId = comp.id;
      colSelect.className = 'component-col-select';
      for (let c = 0; c < SCREEN_COLS; c++) {
        const opt = document.createElement('option');
        opt.value = c;
        opt.textContent = (c + 1).toString();
        if (c === comp.col) opt.selected = true;
        colSelect.appendChild(opt);
      }

      const rowLabel = document.createElement('label');
      rowLabel.textContent = '行 (1-' + SCREEN_ROWS + ')';

      const rowSelect = document.createElement('select');
      rowSelect.dataset.componentId = comp.id;
      rowSelect.className = 'component-row-select';
      for (let r = 0; r < SCREEN_ROWS; r++) {
        const opt = document.createElement('option');
        opt.value = r;
        opt.textContent = (r + 1).toString();
        if (r === comp.row) opt.selected = true;
        rowSelect.appendChild(opt);
      }

      posField.appendChild(colLabel);
      posField.appendChild(colSelect);
      posField.appendChild(rowLabel);
      posField.appendChild(rowSelect);
      
      // 宽度/高度修改
      const widthHeightField = document.createElement('div');
      widthHeightField.className = 'field';
      
      const widthLabel = document.createElement('label');
      widthLabel.textContent = '宽度 (1-' + SCREEN_COLS + ')';
      
      const widthInput = document.createElement('input');
      widthInput.type = 'number';
      widthInput.min = 1;
      widthInput.max = SCREEN_COLS;
      widthInput.value = comp.width || 3;
      widthInput.dataset.componentId = comp.id;
      widthInput.className = 'component-width-input';
      
      const heightLabel = document.createElement('label');
      heightLabel.textContent = '高度 (1-' + SCREEN_ROWS + ')';
      
      const heightInput = document.createElement('input');
      heightInput.type = 'number';
      heightInput.min = 1;
      heightInput.max = SCREEN_ROWS;
      heightInput.value = comp.height || 2;
      heightInput.dataset.componentId = comp.id;
      heightInput.className = 'component-height-input';
      
      widthHeightField.appendChild(widthLabel);
      widthHeightField.appendChild(widthInput);
      widthHeightField.appendChild(heightLabel);
      widthHeightField.appendChild(heightInput);
      
      // 偏移量微调
      const offsetField = document.createElement('div');
      offsetField.className = 'field';
      
      const xOffsetLabel = document.createElement('label');
      xOffsetLabel.textContent = 'X偏移 (像素)';
      
      const xOffsetInput = document.createElement('input');
      xOffsetInput.type = 'number';
      xOffsetInput.min = -100;
      xOffsetInput.max = 100;
      xOffsetInput.value = comp.xOffset || 0;
      xOffsetInput.dataset.componentId = comp.id;
      xOffsetInput.className = 'component-xoffset-input';
      
      const yOffsetLabel = document.createElement('label');
      yOffsetLabel.textContent = 'Y偏移 (像素)';
      
      const yOffsetInput = document.createElement('input');
      yOffsetInput.type = 'number';
      yOffsetInput.min = -100;
      yOffsetInput.max = 100;
      yOffsetInput.value = comp.yOffset || 0;
      yOffsetInput.dataset.componentId = comp.id;
      yOffsetInput.className = 'component-yoffset-input';
      
      offsetField.appendChild(xOffsetLabel);
      offsetField.appendChild(xOffsetInput);
      offsetField.appendChild(yOffsetLabel);
      offsetField.appendChild(yOffsetInput);
      
      // 字体选择
      const fontField = document.createElement('div');
      fontField.className = 'field';
      
      const fontLabel = document.createElement('label');
      fontLabel.textContent = '字体（可选择或手动输入）';
      
      // 使用 input + datalist 支持选择和手动输入
      const fontInput = document.createElement('input');
      fontInput.type = 'text';
      fontInput.dataset.componentId = comp.id;
      fontInput.className = 'component-fontfamily-input';
      fontInput.value = comp.fontFamily || 'Arial';
      fontInput.placeholder = '选择或输入字体名称';
      fontInput.setAttribute('list', `font-list-${comp.id}`);
      
      const fontDatalist = document.createElement('datalist');
      fontDatalist.id = `font-list-${comp.id}`;
      
      // 使用动态获取的字体列表
      const fonts = availableFonts.length > 0 ? availableFonts : ['Arial', '微软雅黑'];
      
      fonts.forEach(font => {
        const option = document.createElement('option');
        option.value = font;
        fontDatalist.appendChild(option);
      });
      
      fontField.appendChild(fontLabel);
      fontField.appendChild(fontInput);
      fontField.appendChild(fontDatalist);
      
      // 文本颜色选择
      const textColorField = document.createElement('div');
      textColorField.className = 'field';
      
      const textColorLabel = document.createElement('label');
      textColorLabel.textContent = '文本颜色（16级灰度）';
      
      const textColorSelect = document.createElement('select');
      textColorSelect.dataset.componentId = comp.id;
      textColorSelect.className = 'component-textcolor-input';
      
      for (let i = 0; i <= 15; i++) {
        const option = document.createElement('option');
        option.value = i;
        const grayValue = i * 17;
        const grayHex = grayValue.toString(16).padStart(2, '0');
        option.textContent = `级别 ${i} (#${grayHex}${grayHex}${grayHex})`;
        option.style.backgroundColor = `rgb(${grayValue}, ${grayValue}, ${grayValue})`;
        option.style.color = i < 8 ? '#ffffff' : '#000000';
        if (i === (comp.textColor || 0)) {
          option.selected = true;
        }
        textColorSelect.appendChild(option);
      }
      
      textColorField.appendChild(textColorLabel);
      textColorField.appendChild(textColorSelect);
      
      // 背景颜色选择
      const bgColorField = document.createElement('div');
      bgColorField.className = 'field';
      
      const bgColorLabel = document.createElement('label');
      bgColorLabel.textContent = '背景颜色';
      
      const bgColorSelect = document.createElement('select');
      bgColorSelect.dataset.componentId = comp.id;
      bgColorSelect.className = 'component-bgcolor-input';
      
      // 透明选项
      const transparentOption = document.createElement('option');
      transparentOption.value = 'transparent';
      transparentOption.textContent = '透明';
      if (comp.bgColor === 'transparent' || comp.bgColor === undefined) {
        transparentOption.selected = true;
      }
      bgColorSelect.appendChild(transparentOption);
      
      // 16级灰度
      for (let i = 0; i <= 15; i++) {
        const option = document.createElement('option');
        option.value = i;
        const grayValue = i * 17;
        const grayHex = grayValue.toString(16).padStart(2, '0');
        option.textContent = `级别 ${i} (#${grayHex}${grayHex}${grayHex})`;
        option.style.backgroundColor = `rgb(${grayValue}, ${grayValue}, ${grayValue})`;
        option.style.color = i < 8 ? '#ffffff' : '#000000';
        if (i === comp.bgColor) {
          option.selected = true;
        }
        bgColorSelect.appendChild(option);
      }
      
      bgColorField.appendChild(bgColorLabel);
      bgColorField.appendChild(bgColorSelect);

      // 旋转角度
      const rotField = document.createElement('div');
      rotField.className = 'field';

      const rotLabel = document.createElement('label');
      rotLabel.textContent = '旋转角度（度）';

      const rotInput = document.createElement('input');
      rotInput.type = 'number';
      rotInput.min = -180;
      rotInput.max = 180;
      rotInput.step = 1;
      rotInput.value = comp.rotation || 0;
      rotInput.dataset.componentId = comp.id;
      rotInput.className = 'component-rotation-input';

      rotField.appendChild(rotLabel);
      rotField.appendChild(rotInput);
      
      // 分割线特有配置：线条颜色（0-15灰度）
      const lineColorField = document.createElement('div');
      lineColorField.className = 'field';
      
      const lineColorLabel = document.createElement('label');
      lineColorLabel.textContent = '线条颜色（16级灰度）';
      
      const lineColorSelect = document.createElement('select');
      lineColorSelect.dataset.componentId = comp.id;
      lineColorSelect.className = 'component-linecolor-input';
      
      for (let i = 0; i <= 15; i++) {
        const option = document.createElement('option');
        option.value = i;
        const grayValue = i * 17;
        const grayHex = grayValue.toString(16).padStart(2, '0');
        option.textContent = `级别 ${i} (#${grayHex}${grayHex}${grayHex})`;
        option.style.backgroundColor = `rgb(${grayValue}, ${grayValue}, ${grayValue})`;
        option.style.color = i < 8 ? '#ffffff' : '#000000';
        if (i === (comp.lineColor || 0)) {
          option.selected = true;
        }
        lineColorSelect.appendChild(option);
      }
      
      lineColorField.appendChild(lineColorLabel);
      lineColorField.appendChild(lineColorSelect);
      
      // 分割线样式
      const lineStyleField = document.createElement('div');
      lineStyleField.className = 'field';
      
      const lineStyleLabel = document.createElement('label');
      lineStyleLabel.textContent = '线条样式';
      
      const lineStyleSelect = document.createElement('select');
      lineStyleSelect.dataset.componentId = comp.id;
      lineStyleSelect.className = 'component-linestyle-input';
      
      const lineStyles = [
        { value: 'solid', label: '实线' },
        { value: 'dashed', label: '虚线' },
        { value: 'dotted', label: '点线' },
        { value: 'double', label: '双线' }
      ];
      
      lineStyles.forEach(style => {
        const option = document.createElement('option');
        option.value = style.value;
        option.textContent = style.label;
        if (style.value === (comp.lineStyle || 'solid')) {
          option.selected = true;
        }
        lineStyleSelect.appendChild(option);
      });
      
      lineStyleField.appendChild(lineStyleLabel);
      lineStyleField.appendChild(lineStyleSelect);
      
      // 分割线粗细
      const lineWidthField = document.createElement('div');
      lineWidthField.className = 'field';
      
      const lineWidthLabel = document.createElement('label');
      lineWidthLabel.textContent = '线条粗细（像素）';
      
      const lineWidthInput = document.createElement('input');
      lineWidthInput.type = 'number';
      lineWidthInput.min = 1;
      lineWidthInput.max = 20;
      lineWidthInput.value = comp.lineWidth || 2;
      lineWidthInput.dataset.componentId = comp.id;
      lineWidthInput.className = 'component-linewidth-input';
      
      lineWidthField.appendChild(lineWidthLabel);
      lineWidthField.appendChild(lineWidthInput);
      
      // 创建详情容器（默认折叠）
      const detailsContainer = document.createElement('div');
      detailsContainer.className = 'component-details';
      // 默认折叠；若该组件之前处于展开状态，则保持展开
      detailsContainer.style.display = (typeof expandedComponentId !== 'undefined' && expandedComponentId === comp.id) ? 'block' : 'none';
      detailsContainer.style.marginTop = '10px';
      
      // 分割线组件的配置
      if (comp.type === 'divider') {
        detailsContainer.appendChild(posField);
        detailsContainer.appendChild(widthHeightField);
        detailsContainer.appendChild(offsetField);
        detailsContainer.appendChild(lineColorField);
        detailsContainer.appendChild(lineStyleField);
        detailsContainer.appendChild(lineWidthField);
        detailsContainer.appendChild(rotField);
      }
      // 列表组件的配置
      else if (comp.type === 'list') {
        // 创建行间距字段
        const marginField = document.createElement('div');
        marginField.className = 'field';
        
        const marginLabel = document.createElement('label');
        marginLabel.textContent = '行间距（像素）';
        
        const marginInput = document.createElement('input');
        marginInput.type = 'number';
        marginInput.min = 0;
        marginInput.max = 50;
        marginInput.step = 1;
        marginInput.value = comp.margin !== undefined ? comp.margin : 10;
        marginInput.dataset.componentId = comp.id;
        marginInput.className = 'component-margin-input';
        marginInput.addEventListener('input', (e) => {
          const id = parseInt(e.target.dataset.componentId);
          updateComponentMargin(id, e.target.value);
        });
        
        marginField.appendChild(marginLabel);
        marginField.appendChild(marginInput);
        
        detailsContainer.appendChild(field);  // 文本输入（分号分隔）
        detailsContainer.appendChild(posField);
        detailsContainer.appendChild(widthHeightField);
        detailsContainer.appendChild(offsetField);
        detailsContainer.appendChild(sizeField);  // 字体大小
        detailsContainer.appendChild(textColorField);  // 文本颜色
        detailsContainer.appendChild(marginField);  // 行间距
        // 列表不支持字体选择、旋转、背景色、对齐
      }
      // RSS组件的配置（与列表类似）
      else if (comp.type === 'rss') {
        // 创建行间距字段
        const marginField = document.createElement('div');
        marginField.className = 'field';
        
        const marginLabel = document.createElement('label');
        marginLabel.textContent = '行间距（像素）';
        
        const marginInput = document.createElement('input');
        marginInput.type = 'number';
        marginInput.min = 0;
        marginInput.max = 50;
        marginInput.step = 1;
        marginInput.value = comp.margin !== undefined ? comp.margin : 10;
        marginInput.dataset.componentId = comp.id;
        marginInput.className = 'component-margin-input';
        marginInput.addEventListener('input', (e) => {
          const id = parseInt(e.target.dataset.componentId);
          updateComponentMargin(id, e.target.value);
        });
        
        marginField.appendChild(marginLabel);
        marginField.appendChild(marginInput);
        
        detailsContainer.appendChild(field);  // URL输入
        detailsContainer.appendChild(posField);
        detailsContainer.appendChild(widthHeightField);
        detailsContainer.appendChild(offsetField);
        detailsContainer.appendChild(sizeField);  // 字体大小
        detailsContainer.appendChild(textColorField);  // 文本颜色
        detailsContainer.appendChild(marginField);  // 行间距
        // RSS不支持字体选择、旋转、背景色、对齐
      }
      // 天气组件的配置
      else if (comp.type === 'weather') {
        // 城市代码输入
        const citycodeField = document.createElement('div');
        citycodeField.className = 'field';
        
        const citycodeLabel = document.createElement('label');
        citycodeLabel.textContent = '城市代码';
        
        const citycodeInput = document.createElement('input');
        citycodeInput.type = 'text';
        citycodeInput.value = comp.citycode || '110000';
        citycodeInput.placeholder = '例如: 110000 (北京)';
        citycodeInput.dataset.componentId = comp.id;
        citycodeInput.className = 'component-citycode-input';
        citycodeInput.addEventListener('input', (e) => {
          const id = parseInt(e.target.dataset.componentId);
          updateComponentCitycode(id, e.target.value);
        });
        
        citycodeField.appendChild(citycodeLabel);
        citycodeField.appendChild(citycodeInput);
        
        // API Key输入
        const apiKeyField = document.createElement('div');
        apiKeyField.className = 'field';
        
        const apiKeyLabel = document.createElement('label');
        apiKeyLabel.textContent = '高德地图 API Key';
        
        const apiKeyInput = document.createElement('input');
        apiKeyInput.type = 'text';
        apiKeyInput.value = comp.apiKey || '';
        apiKeyInput.placeholder = '请输入高德地图 API Key';
        apiKeyInput.dataset.componentId = comp.id;
        apiKeyInput.className = 'component-apikey-input';
        apiKeyInput.addEventListener('input', (e) => {
          const id = parseInt(e.target.dataset.componentId);
          updateComponentApiKey(id, e.target.value);
        });
        
        apiKeyField.appendChild(apiKeyLabel);
        apiKeyField.appendChild(apiKeyInput);
        
        detailsContainer.appendChild(citycodeField);
        detailsContainer.appendChild(apiKeyField);
        detailsContainer.appendChild(posField);
        detailsContainer.appendChild(widthHeightField);
        detailsContainer.appendChild(offsetField);
        detailsContainer.appendChild(sizeField);
        detailsContainer.appendChild(textColorField);
        // 天气组件不支持字体选择、旋转、背景色
      }
      else {
        // 文本类组件（今日诗词组件不显示文本输入框）
        if (comp.type !== 'daily_poem' && comp.type !== 'reading_status') {
          detailsContainer.appendChild(field);
        }
        detailsContainer.appendChild(posField);
        detailsContainer.appendChild(widthHeightField);
        detailsContainer.appendChild(offsetField);
        detailsContainer.appendChild(sizeField);
        // 动态文本（dynamic_text, daily_poem）不支持字体选择和旋转
        if (comp.type !== 'dynamic_text' && comp.type !== 'daily_poem' && comp.type !== 'reading_status') {
          detailsContainer.appendChild(fontField);
        }
        detailsContainer.appendChild(textColorField);
        // 动态文本不显示背景色设置
        if (comp.type !== 'dynamic_text' && comp.type !== 'daily_poem' && comp.type !== 'reading_status') {
          detailsContainer.appendChild(bgColorField);
        }
        // 动态文本和今日诗词默认左对齐，不显示对齐选项
        if (comp.type !== 'dynamic_text' && comp.type !== 'daily_poem' && comp.type !== 'reading_status') {
          detailsContainer.appendChild(rotField);
        }
      }
      
      // 添加折叠/展开功能（只允许一个展开）
      header.addEventListener('click', (e) => {
        // 如果点击的是删除按钮，不触发折叠
        if (e.target === deleteBtn || e.target.closest('.button.is-small.error')) {
          return;
        }
        const isExpanded = detailsContainer.style.display !== 'none';

        if (!isExpanded) {
          // 收起所有其他详情并重置图标
          listEl.querySelectorAll('.component-details').forEach(dc => {
            dc.style.display = 'none';
          });
          listEl.querySelectorAll('.toggle-icon').forEach(ti => {
            ti.style.transform = 'rotate(0deg)';
          });

          // 展开当前并记录 id
          detailsContainer.style.display = 'block';
          toggleIcon.style.transform = 'rotate(90deg)';
          expandedComponentId = comp.id;
        } else {
          // 收起当前并清除记录
          detailsContainer.style.display = 'none';
          toggleIcon.style.transform = 'rotate(0deg)';
          if (expandedComponentId === comp.id) expandedComponentId = null;
        }
      });
      
      item.appendChild(header);
      item.appendChild(detailsContainer);
      listEl.appendChild(item);
    });
    
    console.log('[updateComponentList] 组件列表渲染完成');
  }

  // 删除组件
  function removeComponent(id) {
    components = components.filter(c => c.id !== id);
    if (expandedComponentId === id) expandedComponentId = null;
    updateScreenPreview();
    updateComponentList();
    updateBackgroundPreview(); // 重新渲染canvas以移除已删除组件的内容
  }

  // 更新组件文本
  function updateComponentText(id, text) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.text = text;
      updateBackgroundPreview(); // 实时更新预览
    }
  }

  // 更新组件列
  function updateComponentCol(id, col) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.col = Math.max(0, Math.min(SCREEN_COLS - 1, parseInt(col) || 0));
      updateScreenPreview();
      updateComponentList();
      updateBackgroundPreview(); // 更新画布渲染
    }
  }

  // 更新组件行
  function updateComponentRow(id, row) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.row = Math.max(0, Math.min(SCREEN_ROWS - 1, parseInt(row) || 0));
      updateScreenPreview();
      updateComponentList();
      updateBackgroundPreview(); // 更新画布渲染
    }
  }
  
  // 更新组件字体大小
  function updateComponentFontSize(id, fontSize) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.fontSize = parseInt(fontSize) || 24;
      updateBackgroundPreview(); // 实时更新预览
    }
  }

  // 更新组件旋转角度
  function updateComponentRotation(id, rotation) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.rotation = parseInt(rotation) || 0;
      updateBackgroundPreview(); // 实时更新预览
    }
  }
  
  // 更新组件字体
  function updateComponentFontFamily(id, fontFamily) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.fontFamily = fontFamily;
      updateBackgroundPreview(); // 实时更新预览
    }
  }
  
  // 更新组件文本颜色
  function updateComponentTextColor(id, color) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.textColor = parseInt(color) || 0;
      updateBackgroundPreview();
    }
  }
  
  // 更新组件背景颜色
  function updateComponentBgColor(id, color) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.bgColor = color === 'transparent' ? 'transparent' : parseInt(color);
      updateBackgroundPreview();
    }
  }
  
  // 更新组件对齐方式
  function updateComponentAlign(id, value) {
    const comp = components.find(c => c.id === id);
    if (!comp) return;
    comp.align = value;
    updateBackgroundPreview();
  }
  
  // 更新组件宽度
  function updateComponentWidth(id, width) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.width = Math.max(1, Math.min(SCREEN_COLS, parseInt(width) || 3));
      updateScreenPreview();
      updateComponentList();
      updateBackgroundPreview();
    }
  }
  
  // 更新组件高度
  function updateComponentHeight(id, height) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.height = Math.max(1, Math.min(SCREEN_ROWS, parseInt(height) || 2));
      updateScreenPreview();
      updateComponentList();
      updateBackgroundPreview();
    }
  }
  
  // 更新组件X偏移
  function updateComponentXOffset(id, xOffset) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.xOffset = Math.max(-100, Math.min(100, parseInt(xOffset) || 0));
      updateBackgroundPreview();
    }
  }
  
  // 更新组件Y偏移
  function updateComponentYOffset(id, yOffset) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.yOffset = Math.max(-100, Math.min(100, parseInt(yOffset) || 0));
      updateBackgroundPreview();
    }
  }
  
  // 更新分割线颜色
  function updateComponentLineColor(id, color) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.lineColor = parseInt(color) || 0;
      updateBackgroundPreview();
    }
  }
  
  // 更新分割线样式
  function updateComponentLineStyle(id, style) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.lineStyle = style || 'solid';
      updateBackgroundPreview();
    }
  }
  
  // 更新分割线粗细
  function updateComponentLineWidth(id, width) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.lineWidth = Math.max(1, Math.min(20, parseInt(width) || 2));
      updateBackgroundPreview();
    }
  }
  
  // 更新列表行间距
  function updateComponentMargin(id, margin) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.margin = Math.max(0, Math.min(50, parseInt(margin) || 10));
      updateBackgroundPreview();
    }
  }
  
  // 更新天气组件城市代码
  function updateComponentCitycode(id, citycode) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.citycode = citycode || '110000';
      updateBackgroundPreview();
    }
  }
  
  // 更新天气组件API Key
  function updateComponentApiKey(id, apiKey) {
    const comp = components.find(c => c.id === id);
    if (comp) {
      comp.apiKey = apiKey || '';
      updateBackgroundPreview();
    }
  }
  
  // 复位配置
  function resetConfig() {
    if (components.length === 0 && !backgroundImage) {
      alert('当前没有配置需要复位');
      return;
    }
    
    if (!confirm('确定要清空所有组件和背景图吗？此操作不可恢复！')) {
      return;
    }
    
    // 清空所有数据
    components = [];
    backgroundImage = null;
    hasBgPic = false;
    selectedCell = null;
    
    // 清除选中状态
    document.querySelectorAll('.screen-cell.selected').forEach(cell => {
      cell.classList.remove('selected');
    });
    
    // 更新界面
    updateScreenPreview();
    updateComponentList();
    updateBackgroundPreview();
    
    // 更新复选框
    const chkIncludeBgPic = document.getElementById('chkIncludeBgPic');
    if (chkIncludeBgPic) {
      chkIncludeBgPic.checked = false;
    }
    
    // 清空本地存储
    try {
      if (chrome && chrome.storage && chrome.storage.local) {
        chrome.storage.local.remove(['display_config', 'background_image', 'has_bg_pic']);
      }
    } catch (e) {
      // ignore
    }
    
    setStatus('配置已复位', 'success', 'display');
  }

  // 生成渲染图片 (.png) - 540x960 分辨率
  async function generateRenderImage() {
    return new Promise((resolve, reject) => {
      try {
        // 创建离屏 canvas，尺寸为 540x960（M5Paper 分辨率）
        const canvas = document.createElement('canvas');
        canvas.width = 540;
        canvas.height = 960;
        const ctx = canvas.getContext('2d');

        // 清空背景为白色
        ctx.fillStyle = '#FFFFFF';
        ctx.fillRect(0, 0, canvas.width, canvas.height);

        // 绘制背景图（如果有）
        const drawComponents = () => {
          // 计算单元格尺寸
          const cellWidth = canvas.width / SCREEN_COLS; // 540 / 9 = 60
          const cellHeight = canvas.height / SCREEN_ROWS; // 960 / 16 = 60

          // 绘制组件（只渲染 dynamic = false 的组件）
          components.forEach((comp) => {
            // 跳过需要设备动态渲染的组件
            if (comp.dynamic) {
              return;
            }
            
            const x = comp.col * cellWidth + (comp.xOffset || 0);
            const y = comp.row * cellHeight + (comp.yOffset || 0);
            const w = comp.width * cellWidth;
            const h = comp.height * cellHeight;

            // 绘制分割线
            if (comp.type === 'divider') {
              const cx = x + w / 2;
              const cy = y + h / 2;
              const angle = (comp.rotation || 0) * Math.PI / 180;
              ctx.save();
              ctx.translate(cx, cy);
              ctx.rotate(angle);
              
              const lineGray = (comp.lineColor || 0) * 17;
              ctx.strokeStyle = `rgb(${lineGray}, ${lineGray}, ${lineGray})`;
              ctx.lineWidth = comp.lineWidth || 2;
              
              // 设置线条样式
              const lineStyle = comp.lineStyle || 'solid';
              if (lineStyle === 'dashed') {
                ctx.setLineDash([10, 5]);
              } else if (lineStyle === 'dotted') {
                ctx.setLineDash([2, 3]);
              } else if (lineStyle === 'double') {
                ctx.setLineDash([]);
                // 绘制双线
                ctx.beginPath();
                ctx.moveTo(-w/2, -2);
                ctx.lineTo(w/2, -2);
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(-w/2, 2);
                ctx.lineTo(w/2, 2);
                ctx.stroke();
              } else {
                ctx.setLineDash([]);
              }
              
              // 绘制单线（非double样式）
              if (lineStyle !== 'double') {
                ctx.beginPath();
                ctx.moveTo(-w/2, 0);
                ctx.lineTo(w/2, 0);
                ctx.stroke();
              }
              
              ctx.restore();
            }
            // 绘制文本（支持旋转）
            else if (comp.text && comp.type === 'text') {
              const cx = x + w / 2;
              const cy = y + h / 2;
              const angle = (comp.rotation || 0) * Math.PI / 180;
              ctx.save();
              ctx.translate(cx, cy);
              ctx.rotate(angle);

              // 背景色
              if (comp.bgColor !== 'transparent' && comp.bgColor !== undefined) {
                const bgGray = (comp.bgColor || 0) * 17;
                ctx.fillStyle = `rgb(${bgGray}, ${bgGray}, ${bgGray})`;
                ctx.fillRect(-w/2, -h/2, w, h);
              }

              // 文本
              const textGray = (comp.textColor || 0) * 17;
              ctx.fillStyle = `rgb(${textGray}, ${textGray}, ${textGray})`;
              const fontSize = comp.fontSize || 24;
              const fontFamily = comp.fontFamily || 'Arial';
              ctx.font = `${fontSize}px ${fontFamily}, sans-serif`;
              ctx.textAlign = 'center';
              ctx.textBaseline = 'middle';

              // 文本换行处理
              const maxCharsPerLine = Math.floor(w / (fontSize * 0.6));
              const lines = [];
              let currentLine = '';
              for (let i = 0; i < comp.text.length; i++) {
                currentLine += comp.text[i];
                if (currentLine.length >= maxCharsPerLine || i === comp.text.length - 1) {
                  lines.push(currentLine);
                  currentLine = '';
                }
              }

              const lineHeight = fontSize * 1.2;
              const startYLocal = -(lines.length - 1) * lineHeight / 2;
              lines.forEach((line, idx) => {
                ctx.fillText(line, 0, startYLocal + idx * lineHeight);
              });

              ctx.restore();
            }
          });

          // 转换为 PNG 格式（自动压缩）
          canvas.toBlob((blob) => {
            if (blob) {
              resolve(blob);
            } else {
              reject(new Error('无法生成 PNG 图片'));
            }
          }, 'image/png', 0.9); // PNG 格式，质量 0.9
        };

        // 如果有背景图，先绘制背景
        if (backgroundImage && hasBgPic) {
          const img = new Image();
          img.onload = function() {
            ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
            drawComponents();
          };
          img.onerror = function() {
            console.error('背景图加载失败，仅绘制组件');
            drawComponents();
          };
          img.src = backgroundImage;
        } else {
          drawComponents();
        }
      } catch (e) {
        reject(e);
      }
    });
  }

  // 生成RDT配置JSON
  function generateRDTConfig() {
    return {
      version: '1.0',
      // timestamp: ISO 格式的最后修改时间，每次生成 .rdt 时更新
      timestamp: new Date().toISOString(),
      // 刷新周期（分钟）——兼容旧格式，会写入 readpaper.rdt
      refreshPeriod: readRefreshPeriod(),
      bgpic: hasBgPic,
      screen: {
        width: SCREEN_COLS,
        height: SCREEN_ROWS
      },
      components: components.map(comp => ({
        type: comp.type,
        position: {
          x: comp.col,
          y: comp.row
        },
        size: {
          width: comp.width,
          height: comp.height
        },
        config: {
          // RSS组件特殊处理：text字段映射为url
          ...(comp.type === 'rss' ? { url: comp.text } : { text: comp.text }),
          fontSize: comp.fontSize || 24,
          fontFamily: comp.fontFamily || 'Arial',
          textColor: comp.textColor !== undefined ? comp.textColor : 0,
          bgColor: comp.bgColor !== undefined ? comp.bgColor : 'transparent',
          rotation: comp.rotation !== undefined ? comp.rotation : 0,
          align: comp.align || 'left',  // 对齐方式
          xOffset: comp.xOffset || 0,  // x偏移量
          yOffset: comp.yOffset || 0,  // y偏移量
          lineColor: comp.lineColor !== undefined ? comp.lineColor : 0,  // 分割线颜色
          lineStyle: comp.lineStyle || 'solid',  // 分割线样式
          lineWidth: comp.lineWidth || 2,  // 分割线粗细
          margin: comp.margin !== undefined ? comp.margin : ((comp.type === 'list' || comp.type === 'rss') ? 10 : undefined),  // 列表和RSS行间距
          // 天气组件特有字段
          ...(comp.type === 'weather' ? { 
            citycode: comp.citycode || '110000',
            apiKey: comp.apiKey || ''
          } : {})
        },
        dynamic: comp.dynamic !== undefined ? comp.dynamic : (comp.type !== 'text')
      }))
    };
  }

  // 从本地文件导入配置
  async function loadFromLocalFile() {
    const rdtFileInput = document.getElementById('rdtFileInput');
    if (!rdtFileInput) {
      console.error('找不到 rdtFileInput 元素');
      return;
    }
    
    rdtFileInput.onchange = async function(e) {
      const file = e.target.files[0];
      if (!file) return;
      
      try {
        setStatus('读取本地配置文件...', 'info', 'display');
        
        const content = await file.text();
        console.log('文件内容:', content.substring(0, 200) + '...');
        
        // 检查内容是否为空
        if (!content || content.trim() === '') {
          throw new Error('配置文件为空');
        }
        
        const config = JSON.parse(content);
        console.log('解析后的配置:', config);
        console.log('组件数量:', config.components ? config.components.length : 0);
        
        // 验证配置结构
        if (!config || typeof config !== 'object') {
          throw new Error('配置文件格式无效');
        }
        
        // 解析配置
        loadDisplayConfig(config);
        
        console.log('导入后的 components 数组:', components);
        console.log('导入后的 components 长度:', components.length);
        
        setStatus('本地配置导入成功', 'success', 'display');
        
      } catch (err) {
        console.error('本地导入失败:', err);
        setStatus('本地导入失败: ' + err.message, 'error', 'display');
      }
      
      // 重置 input，允许重复选择同一文件
      e.target.value = '';
    };
    
    rdtFileInput.click();
  }
  
  // 从配置对象加载显示配置（提取为公共函数，供WebDAV和本地导入共用）
  function loadDisplayConfig(config) {
    console.log('[loadDisplayConfig v2] 开始执行，配置:', config);
    
    if (!config) {
      console.error('[loadDisplayConfig] 配置对象为空');
      return;
    }
    
    if (!config.components || !Array.isArray(config.components)) {
      console.error('[loadDisplayConfig] 没有有效的组件数组');
      return;
    }
    
    // 如果旧格式缺少 refreshPeriod，则补上默认值以兼容
    if (config.refreshPeriod === undefined || config.refreshPeriod === null) {
      config.refreshPeriod = 30;
      console.log('[loadDisplayConfig] 未检测到 refreshPeriod，已补默认 30');
    }
    // 将 refreshPeriod 更新到 UI 输入框（并保持在允许范围内）
    try {
      const rp = Math.min(1440, Math.max(10, parseInt(config.refreshPeriod, 10) || 30));
      if (refreshPeriodEl) refreshPeriodEl.value = rp;
    } catch (e) { if (refreshPeriodEl) refreshPeriodEl.value = 30; }

    console.log('[loadDisplayConfig] 原始组件数组长度:', config.components.length);
    
    // 为每个组件生成唯一ID（使用时间戳+索引，乘以1000确保不重复）
    const baseId = Date.now();
    const newComponents = [];
    
    config.components.forEach((comp, idx) => {
      try {
        console.log(`[loadDisplayConfig] 处理组件 ${idx}:`, comp);
        
        const newComp = {
          id: baseId + idx * 1000,
          type: comp.type || 'text',
          row: comp.position?.y ?? 0,
          col: comp.position?.x ?? 0,
          width: comp.size?.width ?? 3,
          height: comp.size?.height ?? 2,
          // RSS组件特殊处理：url字段映射为text
          text: comp.type === 'rss' ? (comp.config?.url || '') : (comp.config?.text || ''),
          fontSize: comp.config?.fontSize || 24,
          fontFamily: comp.config?.fontFamily || 'Arial',
          textColor: comp.config?.textColor !== undefined ? comp.config.textColor : 0,
          bgColor: comp.config?.bgColor !== undefined ? comp.config.bgColor : 'transparent',
          rotation: comp.config?.rotation !== undefined ? comp.config.rotation : 0,
          align: comp.config?.align || 'left',
          xOffset: comp.config?.xOffset || 0,
          yOffset: comp.config?.yOffset || 0,
          lineColor: comp.config?.lineColor !== undefined ? comp.config.lineColor : 0,
          lineStyle: comp.config?.lineStyle || 'solid',
          lineWidth: comp.config?.lineWidth || 2,
          margin: comp.config?.margin !== undefined ? comp.config.margin : ((comp.type === 'list' || comp.type === 'rss') ? 10 : undefined),
          // 天气组件特有字段
          citycode: comp.type === 'weather' ? (comp.config?.citycode || '110000') : undefined,
          apiKey: comp.type === 'weather' ? (comp.config?.apiKey || '') : undefined,
          dynamic: comp.dynamic !== undefined ? comp.dynamic : (comp.type !== 'text')
        };
        
        newComponents.push(newComp);
        console.log(`[loadDisplayConfig] 组件 ${idx} 处理成功`);
      } catch (err) {
        console.error(`[loadDisplayConfig] 处理组件 ${idx} 时出错:`, err, comp);
      }
    });
    
    components = newComponents;
    console.log('[loadDisplayConfig] 最终 components 数组长度:', components.length);
    
    // 处理 bgpic（本地导入时不加载背景图片，需要用户手动设置）
    hasBgPic = !!config.bgpic;
    backgroundImage = null;
    
    // 更新"包含背景图"复选框状态
    const chkIncludeBgPic = document.getElementById('chkIncludeBgPic');
    if (chkIncludeBgPic) {
      chkIncludeBgPic.checked = hasBgPic;
      console.log('[loadDisplayConfig] 复选框已更新');
    } else {
      console.warn('[loadDisplayConfig] 找不到 chkIncludeBgPic 元素');
    }
    
    console.log('[loadDisplayConfig] 开始更新UI...');
    console.log('[loadDisplayConfig] 调用 updateScreenPreview');
    updateScreenPreview();
    
    console.log('[loadDisplayConfig] 调用 updateComponentList');
    updateComponentList();
    
    console.log('[loadDisplayConfig] 调用 updateBackgroundPreview');
    updateBackgroundPreview();
    
    // 更新按钮状态
    console.log('[loadDisplayConfig] 更新按钮状态');
    updateLoadCurrentButtonState();
    updateUploadButtonState();
    
    console.log('[loadDisplayConfig] 执行完成');
  }

  // 从浏览器本地存储异步加载显示配置（页面初始化时调用）
  async function loadDisplayConfigFromLocalStorage() {
    try {
      if (chrome && chrome.storage && chrome.storage.local) {
        return new Promise((resolve) => {
          chrome.storage.local.get(['display_config', 'background_image', 'has_bg_pic'], (res) => {
            if (res && res.display_config) {
              loadDisplayConfig(res.display_config);
              if (res.background_image) {
                backgroundImage = res.background_image;
              }
              if (res.has_bg_pic !== undefined) {
                hasBgPic = res.has_bg_pic;
              }
              // 更新复选框状态
              const chkIncludeBgPic = document.getElementById('chkIncludeBgPic');
              if (chkIncludeBgPic) {
                chkIncludeBgPic.checked = hasBgPic;
              }
              console.log('已从本地存储加载展示配置');
            }
            resolve();
          });
        });
      }
    } catch (e) {
      console.warn('从本地存储加载配置失败:', e);
    }
  }
  
  // 从 WebDAV 读取当前配置
  async function loadFromWebDAV() {
    try {
      setStatus('从 WebDAV 读取配置...', 'info', 'display');
      
      // 获取 WebDAV 配置
      const url = urlEl ? urlEl.value.trim() : '';
      const username = userEl ? userEl.value.trim() : '';
      const password = passEl ? passEl.value : '';
      
      if (!url) {
        setStatus('请先配置 WebDAV', 'error', 'display');
        return;
      }

      // 确保 /readpaper 目录存在（backgroundFetch 会自动处理权限）
      const dirReady = await ensureReadpaperDir(url, username, password);
      if (!dirReady) {
        setStatus('无法访问 WebDAV /readpaper 目录', 'error', 'display');
        return;
      }

      let base = url;
      if (!base.endsWith('/')) base += '/';
      const rdtUrl = base + 'readpaper/readpaper.rdt';

      const headers = {};
      if (username || password) {
        const raw = username + ':' + password;
        const b64 = b64EncodeUtf8(raw);
        if (b64) {
          headers['Authorization'] = 'Basic ' + b64;
        }
      }

      // 读取 RDT 文件
      const response = await backgroundFetch(rdtUrl, {
        method: 'GET',
        headers: headers,
        credentials: 'omit',
        cache: 'no-store'
      });

      if (response.status === 404) {
        setStatus('WebDAV 上未找到 readpaper.rdt 文件', 'error', 'display');
        return;
      }

      if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
      }

      const content = await response.text();
      
      // 检查响应内容是否为空
      if (!content || content.trim() === '') {
        setStatus('WebDAV 返回的配置文件为空', 'error', 'display');
        console.error('[loadFromWebDAV] 空响应内容');
        return;
      }
      
      // 尝试解析 JSON，提供详细的错误信息
      let config;
      try {
        config = JSON.parse(content);
      } catch (parseError) {
        setStatus(`配置文件格式错误: ${parseError.message}`, 'error', 'display');
        console.error('[loadFromWebDAV] JSON 解析失败:', parseError);
        console.error('[loadFromWebDAV] 响应内容:', content.substring(0, 200)); // 只显示前200字符
        return;
      }
      
      // 验证配置结构
      if (!config || typeof config !== 'object') {
        setStatus('配置文件格式无效', 'error', 'display');
        console.error('[loadFromWebDAV] 配置不是有效对象:', config);
        return;
      }

      // 解析配置（使用公共函数）
      loadDisplayConfig(config);
      
      // WebDAV独有：如果有背景图则尝试下载
      hasBgPic = !!config.bgpic;
      
      if (hasBgPic) {
        // 尝试读取背景图 _0.png
        const bgUrl = base + 'readpaper/readpaper_0.png';
        try {
          const bgResponse = await backgroundFetch(bgUrl, {
            method: 'GET',
            headers: headers,
            credentials: 'omit',
            cache: 'no-store'
          });
          
          if (bgResponse.ok) {
            const blob = await bgResponse.blob();
            const reader = new FileReader();
            reader.onload = function(e) {
              backgroundImage = e.target.result;
              // 延迟更新确保图片加载完成
              setTimeout(() => {
                updateBackgroundPreview();
              }, 50);
            };
            reader.readAsDataURL(blob);
          }
        } catch (e) {
          console.warn('无法读取背景图:', e);
        }
      } else {
        backgroundImage = null;
      }

      // 刷新预览
      updateBackgroundPreview();

      // 更新复选框状态
      const chkIncludeBgPic = document.getElementById('chkIncludeBgPic');
      if (chkIncludeBgPic) {
        chkIncludeBgPic.checked = hasBgPic;
      }
      
      setStatus('已从 WebDAV 加载配置', 'success', 'display');
    } catch (e) {
      console.error('[loadFromWebDAV] 加载配置失败:', e);
      console.error('[loadFromWebDAV] 错误堆栈:', e.stack);
      
      let errorMsg = e.message;
      if (e.message.includes('Unexpected end of JSON')) {
        errorMsg = 'WebDAV 返回的配置文件为空或格式不正确。请确保文件已正确保存。';
      } else if (e.message.includes('需要访问权限')) {
        errorMsg = '需要先授予 WebDAV 访问权限。刷新页面后在浏览器弹出的对话框中点击"允许"。';
      }
      
      setStatus(`加载失败: ${errorMsg}`, 'error', 'display');
    }
  }

  // 修改背景图
  async function changeBgImage() {
    const input = document.getElementById('bgImageInput');
    if (!input) return;
    
    input.click();
  }

  // 处理背景图文件选择
  function handleBgImageSelect(event) {
    const file = event.target.files[0];
    if (!file) return;

    // 验证文件类型
    if (!file.type.match(/^image\/(png|jpeg|jpg)$/)) {
      alert('请选择 PNG 或 JPG 格式的图片');
      return;
    }

    const reader = new FileReader();
    reader.onload = function(e) {
      backgroundImage = e.target.result;
      hasBgPic = true;
      
      // 更新复选框
      const chkIncludeBgPic = document.getElementById('chkIncludeBgPic');
      if (chkIncludeBgPic) {
        chkIncludeBgPic.checked = true;
      }
      
      // 确保在图片加载完成后更新预览
      setTimeout(() => {
        updateBackgroundPreview();
        setStatus('背景图已更新（请查看左侧预览）', 'success', 'display');
      }, 50);
    };
    reader.readAsDataURL(file);
  }

  // 保存到本地
  async function saveToLocal() {
    try {
      const config = generateRDTConfig();
      
      // 保存 .rdt 文件
      const rdtBlob = new Blob([JSON.stringify(config, null, 2)], { type: 'application/json' });
      const rdtUrl = URL.createObjectURL(rdtBlob);
      const rdtLink = document.createElement('a');
      rdtLink.href = rdtUrl;
      rdtLink.download = 'readpaper.rdt';
      rdtLink.click();
      URL.revokeObjectURL(rdtUrl);
      
      // 生成并保存渲染图片
      setStatus('生成渲染图片...', 'info', 'display');
      const pngBlob = await generateRenderImage();
      const pngUrl = URL.createObjectURL(pngBlob);
      const pngLink = document.createElement('a');
      pngLink.href = pngUrl;
      pngLink.download = 'readpaper.png';
      pngLink.click();
      URL.revokeObjectURL(pngUrl);
      
      // 如果有背景图，也一起保存
      if (backgroundImage && hasBgPic) {
        // 将背景图转换为 Blob
        const bgResponse = await fetch(backgroundImage);
        const bgBlob = await bgResponse.blob();
        const bgUrl = URL.createObjectURL(bgBlob);
        const bgLink = document.createElement('a');
        bgLink.href = bgUrl;
        bgLink.download = 'readpaper_0.png';
        bgLink.click();
        URL.revokeObjectURL(bgUrl);
        
        setStatus('已保存 .rdt、.png 和背景图到本地', 'success', 'display');
      } else {
        setStatus('已保存 .rdt 和 .png 文件到本地', 'success', 'display');
      }
      
      // 保存到本地存储
      try {
        if (chrome && chrome.storage && chrome.storage.local) {
          chrome.storage.local.set({ 
            display_config: config,
            background_image: backgroundImage,
            has_bg_pic: hasBgPic
          });
        }
      } catch (e) {
        // ignore
      }
    } catch (e) {
      console.error('保存失败:', e);
      setStatus(`保存失败: ${e.message}`, 'error', 'display');
    }
  }

  // 上传到WebDAV
  async function uploadToWebDAV() {
    const config = generateRDTConfig();
    const content = JSON.stringify(config, null, 2);
    
    // 验证生成的配置
    if (!content || content.trim() === '' || content === '{}') {
      showUploadError('配置为空，无法上传');
      console.error('[uploadToWebDAV] 生成的配置为空');
      return;
    }
    
    console.log('[uploadToWebDAV] 配置大小:', content.length, '字节');
    console.log('[uploadToWebDAV] 配置预览:', content.substring(0, 200));
    
    try {
      showUploadProgress('准备上传到云端', '正在生成配置和图片...', 0);
      
      // 在上传之前先保存配置到本地存储，避免刷新后丢失
      try {
        if (chrome && chrome.storage && chrome.storage.local) {
          chrome.storage.local.set({ 
            display_config: config,
            background_image: backgroundImage,
            has_bg_pic: hasBgPic
          });
        }
      } catch (e) {
        // ignore
      }
      
      // 获取 WebDAV 配置
      const url = urlEl ? urlEl.value.trim() : '';
      const username = userEl ? userEl.value.trim() : '';
      const password = passEl ? passEl.value : '';
      
      if (!url) {
        showUploadError('请先配置 WebDAV');
        return;
      }

      // 确保 /readpaper 目录存在（backgroundFetch 会自动处理权限）
      showUploadProgress('检查云端目录', '正在检查/创建 /readpaper 目录...', 5);
      const dirReady = await ensureReadpaperDir(url, username, password);
      if (!dirReady) {
        showUploadError('无法访问或创建 WebDAV /readpaper 目录');
        return;
      }

      let base = url;
      if (!base.endsWith('/')) base += '/';
      const rdtUrl = base + 'readpaper/readpaper.rdt';

      const headers = {};
      if (username || password) {
        const raw = username + ':' + password;
        const b64 = b64EncodeUtf8(raw);
        if (b64) {
          headers['Authorization'] = 'Basic ' + b64;
        }
      }
      headers['Content-Type'] = 'application/json';

      // 上传配置文件
      showUploadProgress('上传配置文件', '正在上传 readpaper.rdt...', 15);
      
      console.log('[uploadToWebDAV] 开始上传到:', rdtUrl);
      console.log('[uploadToWebDAV] 内容大小:', content.length);
      
      const response = await backgroundFetch(rdtUrl, {
        method: 'PUT',
        headers: headers,
        body: content,
        credentials: 'omit',
        cache: 'no-store'
      });

      console.log('[uploadToWebDAV] 上传响应状态:', response.status, response.ok);

      if (!response.ok) {
        throw new Error(`上传配置失败: HTTP ${response.status}`);
      }
      
      console.log('[uploadToWebDAV] 配置文件上传成功');

      // 生成并上传渲染图片
      showUploadProgress('生成渲染图片', '正在生成 PNG...', 30);
      const pngBlob = await generateRenderImage();
      const pngUrl = base + 'readpaper/readpaper.png';
      
      const pngHeaders = {};
      if (username || password) {
        const raw = username + ':' + password;
        const b64 = b64EncodeUtf8(raw);
        if (b64) {
          pngHeaders['Authorization'] = 'Basic ' + b64;
        }
      }
      pngHeaders['Content-Type'] = 'image/png';

      showUploadProgress('上传渲染图片', `正在上传 PNG (${Math.round(pngBlob.size / 1024)} KB)...`, 50);

      const pngResponse = await backgroundFetch(pngUrl, {
        method: 'PUT',
        headers: pngHeaders,
        body: pngBlob,
        credentials: 'omit',
        cache: 'no-store'
      });

      if (!pngResponse.ok) {
        throw new Error(`上传渲染图片失败: HTTP ${pngResponse.status}`);
      }

      let uploadedFiles = ['配置文件', '渲染图片'];

      // 如果有背景图且用户选中了包含背景图选项，则上传背景图
      const chkIncludeBgPic = document.getElementById('chkIncludeBgPic');
      if (hasBgPic && backgroundImage && chkIncludeBgPic && chkIncludeBgPic.checked) {
        showUploadProgress('上传背景图', '正在上传 readpaper_0.png...', 70);
        
        const bgUrl = base + 'readpaper/readpaper_0.png';
        
        // 将 base64 转换为 blob
        const mimeMatch = backgroundImage.match(/^data:([^;]+);base64,/);
        const mimeType = mimeMatch ? mimeMatch[1] : 'image/png';
        const base64Data = backgroundImage.split(',')[1];
        const byteCharacters = atob(base64Data);
        const byteNumbers = new Array(byteCharacters.length);
        for (let i = 0; i < byteCharacters.length; i++) {
          byteNumbers[i] = byteCharacters.charCodeAt(i);
        }
        const byteArray = new Uint8Array(byteNumbers);
        const blob = new Blob([byteArray], { type: mimeType });

        const bgHeaders = {};
        if (username || password) {
          const raw = username + ':' + password;
          const b64 = b64EncodeUtf8(raw);
          if (b64) {
            bgHeaders['Authorization'] = 'Basic ' + b64;
          }
        }
        bgHeaders['Content-Type'] = mimeType;

        const bgResponse = await backgroundFetch(bgUrl, {
          method: 'PUT',
          headers: bgHeaders,
          body: blob,
          credentials: 'omit',
          cache: 'no-store'
        });

        if (!bgResponse.ok) {
          throw new Error(`上传背景图失败: HTTP ${bgResponse.status}`);
        }
        
        uploadedFiles.push('背景图');
      }
      
      showUploadProgress('完成上传', '正在完成...', 98);
      
      // 保存到本地存储
      try {
        if (chrome && chrome.storage && chrome.storage.local) {
          chrome.storage.local.set({ 
            display_config: config,
            background_image: backgroundImage,
            has_bg_pic: hasBgPic
          });
        }
      } catch (e) {
        // ignore
      }
      
      showUploadSuccess(`已上传 ${uploadedFiles.join('、')} 到 WebDAV`);
      
      // 更新"读取当前"按钮状态
      updateLoadCurrentButtonState();
    } catch (e) {
      console.error('上传失败:', e);
      showUploadError(`上传失败: ${e.message}`.substring(0, 80));
    }
  }

  // 加载配置
  // 从浏览器本地存储加载显示配置（用于页面初始化）
  async function loadDisplayConfigFromStorage() {
    try {
      if (chrome && chrome.storage && chrome.storage.local) {
        await new Promise((resolve) => {
          chrome.storage.local.get(['display_config', 'background_image', 'has_bg_pic'], (res) => {
            const config = res && res.display_config;
                if (config && config.components) {
                components = config.components.map((comp, idx) => ({
                id: Date.now() + idx,
                type: comp.type || 'text',
                row: comp.position.y,
                col: comp.position.x,
                width: comp.size.width,
                height: comp.size.height,
                // RSS组件特殊处理：url字段映射为text
                text: comp.type === 'rss' ? (comp.config.url || '') : (comp.config.text || ''),
                fontSize: comp.config.fontSize || 24,
                fontFamily: comp.config.fontFamily || 'Arial',
                textColor: comp.config.textColor !== undefined ? comp.config.textColor : 0,
                bgColor: comp.config.bgColor !== undefined ? comp.config.bgColor : 'transparent',
                rotation: comp.config.rotation !== undefined ? comp.config.rotation : 0,
                align: comp.config.align || 'left',
                xOffset: comp.config.xOffset || 0,
                yOffset: comp.config.yOffset || 0,
                lineColor: comp.config.lineColor !== undefined ? comp.config.lineColor : 0,
                lineStyle: comp.config.lineStyle || 'solid',
                lineWidth: comp.config.lineWidth !== undefined ? comp.config.lineWidth : 2,
                margin: comp.config.margin !== undefined ? comp.config.margin : ((comp.type === 'list' || comp.type === 'rss') ? 10 : undefined),
                // 天气组件特有字段
                citycode: comp.type === 'weather' ? (comp.config.citycode || '110000') : undefined,
                apiKey: comp.type === 'weather' ? (comp.config.apiKey || '') : undefined,
                dynamic: comp.dynamic !== undefined ? comp.dynamic : (comp.type !== 'text')
              }));
              updateScreenPreview();
              updateComponentList();
            }

            // 加载背景图
            if (res.has_bg_pic && res.background_image) {
              hasBgPic = true;
              backgroundImage = res.background_image;
              // 延迟更新以确保 DOM 已准备好
              setTimeout(() => {
                updateBackgroundPreview();
              }, 100);
            }

            // 更新复选框
            const chkIncludeBgPic = document.getElementById('chkIncludeBgPic');
            if (chkIncludeBgPic) {
              chkIncludeBgPic.checked = hasBgPic;
            }

            resolve();
          });
        });
      }
    } catch (e) {
      // ignore
    }
  }

  // 检查 WebDAV 上是否存在 readpaper.rdt
  async function checkWebDAVRDT() {
    try {
      const url = urlEl ? urlEl.value.trim() : '';
      const username = userEl ? userEl.value.trim() : '';
      const password = passEl ? passEl.value : '';
      
      if (!url) {
        return false;
      }

      let base = url;
      if (!base.endsWith('/')) base += '/';
      const rdtUrl = base + 'readpaper/readpaper.rdt';

      const headers = {};
      if (username || password) {
        const raw = username + ':' + password;
        const b64 = b64EncodeUtf8(raw);
        if (b64) {
          headers['Authorization'] = 'Basic ' + b64;
        }
      }

      const response = await backgroundFetch(rdtUrl, {
        method: 'HEAD',
        headers: headers,
        credentials: 'omit',
        cache: 'no-store'
      }, true); // 跳过权限申请，仅检查已有权限

      return response.ok;
    } catch (e) {
      return false;
    }
  }

  // 检查 WebDAV 服务是否可访问（用于决定上传按钮是否可用）
  async function checkWebDAVReachable() {
    try {
      const url = urlEl ? urlEl.value.trim() : '';
      const username = userEl ? userEl.value.trim() : '';
      const password = passEl ? passEl.value : '';
      if (!url) {
        console.log('[WebDAV检测] 未配置 URL');
        return false;
      }

      let base = url;
      if (!base.endsWith('/')) base += '/';

      const headers = {};
      if (username || password) {
        const raw = username + ':' + password;
        const b64 = b64EncodeUtf8(raw);
        if (b64) headers['Authorization'] = 'Basic ' + b64;
      }

      console.log('[WebDAV检测] 开始检测:', base);

      // 策略1: 尝试 OPTIONS 请求（最轻量，CORS 友好）
      try {
        const optResponse = await backgroundFetch(base, {
          method: 'OPTIONS',
          headers: headers,
          redirect: 'manual',
          credentials: 'omit',
          cache: 'no-store'
        }, true); // 跳过权限申请，仅检查已有权限
        
        if (optResponse.status === 401) {
          console.warn('[WebDAV检测] OPTIONS 鉴权失败 (401)');
          return false;
        }
        
        if (optResponse.status >= 200 && optResponse.status < 500) {
          console.log(`[WebDAV检测] OPTIONS 成功 (HTTP ${optResponse.status}) - 可达`);
          return true;
        }
      } catch (optErr) {
        console.warn('[WebDAV检测] OPTIONS 请求失败，降级到 HEAD:', optErr.message);
      }

      // 策略2: 降级到 HEAD 请求
      try {
        const headResponse = await backgroundFetch(base, {
          method: 'HEAD',
          headers: headers,
          redirect: 'manual',
          credentials: 'omit',
          cache: 'no-store'
        }, true); // 跳过权限申请，仅检查已有权限
        
        if (headResponse.status === 401) {
          console.warn('[WebDAV检测] HEAD 鉴权失败 (401)');
          return false;
        }
        
        if (headResponse.status >= 200 && headResponse.status < 500) {
          console.log(`[WebDAV检测] HEAD 成功 (HTTP ${headResponse.status}) - 可达`);
          return true;
        }
      } catch (headErr) {
        console.warn('[WebDAV检测] HEAD 请求失败，最后尝试 PROPFIND:', headErr.message);
      }

      // 策略3: 最后尝试 PROPFIND（标准 WebDAV 方法）
      const propHeaders = { ...headers, 'Depth': '0' };
      const propResponse = await backgroundFetch(base, {
        method: 'PROPFIND',
        headers: propHeaders,
        redirect: 'manual',
        credentials: 'omit',
        cache: 'no-store'
      });
      
      if (propResponse.status === 401) {
        console.warn('[WebDAV检测] PROPFIND 鉴权失败 (401)');
        return false;
      }
      
      if (propResponse.status >= 200 && propResponse.status < 500) {
        console.log(`[WebDAV检测] PROPFIND 成功 (HTTP ${propResponse.status}) - 可达`);
        return true;
      }
      
      console.error(`[WebDAV检测] 所有方法均返回异常状态 (${propResponse.status})`);
      return false;
    } catch (e) {
      console.error('[WebDAV检测] 完全失败 - 可能是网络错误或 CORS 阻止:', e);
      // 如果所有请求都失败（通常是 CORS 或网络问题），保守地返回 true
      // 让用户尝试上传，由实际上传操作报告真实错误
      console.warn('[WebDAV检测] 由于无法确定连通性，允许用户尝试上传');
      return true;
    }
  }

  // 更新"读取当前"按钮状态
  async function updateLoadCurrentButtonState() {
    const btnLoadCurrent = document.getElementById('btnLoadCurrent');
    if (!btnLoadCurrent) return;

    const exists = await checkWebDAVRDT();
    btnLoadCurrent.disabled = !exists;
    
    if (!exists) {
      btnLoadCurrent.title = 'WebDAV 上不存在 readpaper.rdt 文件';
    } else {
      btnLoadCurrent.title = '从 WebDAV 读取配置';
    }
  }

  // 更新上传按钮状态
  async function updateUploadButtonState() {
    const btn = document.getElementById('btnUploadToWebdav');
    if (!btn) return;
    try {
      const reachable = await checkWebDAVReachable();
      btn.disabled = !reachable;
      if (!reachable) {
        btn.title = '无法连接到 WebDAV，请先点击「保存设置」授权或检查地址凭据';
      } else {
        btn.title = '上传配置到 WebDAV';
      }
    } catch (err) {
      // 权限检查失败（未授权）
      btn.disabled = true;
      btn.title = '请先点击「保存设置」按钮授权访问 WebDAV';
      console.log('[updateUploadButtonState] 权限检查失败:', err.message);
    }
  }

  /**
   * 保存 WebDAV 设置到扩展存储（同时申请权限）
   * 必须在用户点击事件中调用以符合 Firefox 权限要求
   * 关键：权限请求必须在同步调用链中，不能有 await 延迟
   */
  function saveWebDAVSettings() {
    try {
      setStatus('保存 WebDAV 设置...', 'info', 'webdav');
      
      const url = urlEl ? urlEl.value.trim() : '';
      const username = userEl ? userEl.value.trim() : '';
      const password = passEl ? passEl.value : '';
      
      if (!url) {
        setStatus('请先填写 WebDAV 地址', 'error', 'webdav');
        return;
      }
      
      if (!chrome || !chrome.permissions) {
        setStatus('浏览器不支持动态权限', 'error', 'webdav');
        return;
      }
      
      // 解析 URL 获取 origin
      let origin;
      try {
        const urlObj = new URL(url);
        origin = urlObj.origin + "/*";
      } catch (e) {
        setStatus('WebDAV 地址格式无效', 'error', 'webdav');
        return;
      }
      
      // 关键：直接同步调用 chrome.permissions.request()，不使用 await
      // Firefox 要求权限请求必须在用户输入处理器的同步调用链中
      console.log('[saveWebDAVSettings] 请求权限:', origin);
      chrome.permissions.request({ origins: [origin] }, function(granted) {
        if (chrome.runtime.lastError) {
          console.error('[saveWebDAVSettings] 权限申请错误:', chrome.runtime.lastError.message);
          setStatus('权限申请失败: ' + chrome.runtime.lastError.message, 'error', 'webdav');
          return;
        }
        
        if (!granted) {
          console.warn('[saveWebDAVSettings] 权限被拒绝');
          setStatus('权限申请被拒绝', 'error', 'webdav');
          return;
        }
        
        console.log('[saveWebDAVSettings] 权限已授予:', origin);
        
        // 保存到扩展存储
        if (chrome.storage && chrome.storage.local) {
          chrome.storage.local.set({
            webdav_url: url,
            webdav_username: username,
            webdav_password: password,
            webdav_permission_granted: true
          }, function() {
            if (chrome.runtime.lastError) {
              setStatus('保存失败: ' + chrome.runtime.lastError.message, 'error', 'webdav');
              return;
            }
            
            setStatus('WebDAV 设置已保存，权限已授予', 'success', 'webdav');
            console.log('[saveWebDAVSettings] 设置已保存:', { url, username: username ? '***' : '' });
            
            // 更新按钮状态
            updateUploadButtonState();
            updateLoadCurrentButtonState();
          });
        } else {
          setStatus('浏览器不支持扩展存储', 'error', 'webdav');
        }
      });
    } catch (err) {
      setStatus('保存设置失败: ' + err.message, 'error', 'webdav');
      console.error('[saveWebDAVSettings] 失败:', err);
    }
  }

  // 将 Blob 转为 base64 字符串（不含 data:* 前缀）
  function blobToBase64(blob) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onloadend = function() {
        const dataUrl = reader.result;
        // data:image/png;base64,xxxxx
        const idx = dataUrl.indexOf(',');
        if (idx >= 0) resolve(dataUrl.substring(idx + 1));
        else resolve(dataUrl);
      };
      reader.onerror = reject;
      reader.readAsDataURL(blob);
    });
  }

  // 显示上传进度横幅
  function showUploadProgress(title, detail, percent) {
    const banner = document.getElementById('uploadProgressBanner');
    const titleEl = document.getElementById('uploadProgressTitle');
    const detailEl = document.getElementById('uploadProgressDetail');
    const barFill = document.getElementById('uploadProgressBarFill');
    
    if (!banner) return;
    
    banner.style.display = 'block';
    banner.className = 'upload-progress-banner';
    
    if (titleEl) titleEl.textContent = title;
    if (detailEl) detailEl.textContent = detail;
    if (barFill) barFill.style.width = percent + '%';
  }

  // 隐藏上传进度横幅
  function hideUploadProgress() {
    const banner = document.getElementById('uploadProgressBanner');
    if (banner) {
      setTimeout(() => {
        banner.style.display = 'none';
      }, 3000);
    }
  }

  // 显示上传成功
  function showUploadSuccess(message) {
    const banner = document.getElementById('uploadProgressBanner');
    const titleEl = document.getElementById('uploadProgressTitle');
    const detailEl = document.getElementById('uploadProgressDetail');
    const barFill = document.getElementById('uploadProgressBarFill');
    
    if (!banner) return;
    
    banner.className = 'upload-progress-banner success';
    if (titleEl) titleEl.textContent = '✓ 上传成功';
    if (detailEl) detailEl.textContent = message;
    if (barFill) barFill.style.width = '100%';
    
    hideUploadProgress();
  }

  // 显示上传失败
  function showUploadError(message) {
    const banner = document.getElementById('uploadProgressBanner');
    const titleEl = document.getElementById('uploadProgressTitle');
    const detailEl = document.getElementById('uploadProgressDetail');
    
    if (!banner) return;
    
    banner.className = 'upload-progress-banner error';
    if (titleEl) titleEl.textContent = '✗ 上传失败';
    if (detailEl) detailEl.textContent = message;
    
    hideUploadProgress();
  }

  // 将当前生成的 .rdt 和 PNG 推送到设备的 /rdt 目录（分块上传）
  async function pushToDevice() {
    const btn = document.getElementById('btnPushToDevice');
    if (btn) btn.disabled = true;
    try {
      showUploadProgress('准备上传', '正在生成配置和图片...', 0);

      const config = generateRDTConfig();
      const rdtText = JSON.stringify(config, null, 2);

      // 在上传之前先保存配置到本地存储，避免刷新后丢失
      try {
        if (chrome && chrome.storage && chrome.storage.local) {
          chrome.storage.local.set({ 
            display_config: config,
            background_image: backgroundImage,
            has_bg_pic: hasBgPic
          });
        }
      } catch (e) {
        // ignore
      }

      const pngBlob = await generateRenderImage();
      const pngBase64 = await blobToBase64(pngBlob);

      console.log('[pushToDevice] PNG info:', {
        blobSize: pngBlob.size,
        base64Length: pngBase64.length,
        estimatedDecodedSize: Math.floor(pngBase64.length * 3 / 4)
      });

      const deviceUrl = 'http://192.168.4.1';
      const CHUNK_SIZE = 8192; // 每块 8KB

      // 上传 RDT（分块）
      showUploadProgress('上传配置文件', '初始化 RDT 上传...', 5);
      
      await fetch(`${deviceUrl}/api/update_display_start`, {
        method: 'POST',
        mode: 'cors',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ type: 'rdt' })
      });

      const rdtTotalChunks = Math.ceil(rdtText.length / CHUNK_SIZE);
      for (let offset = 0, chunkIndex = 0; offset < rdtText.length; offset += CHUNK_SIZE, chunkIndex++) {
        const chunk = rdtText.substring(offset, offset + CHUNK_SIZE);
        const resp = await fetch(`${deviceUrl}/api/update_display_chunk`, {
          method: 'POST',
          mode: 'cors',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ type: 'rdt', data: chunk })
        });
        if (!resp.ok) {
          const text = await resp.text();
          throw new Error(`RDT chunk upload failed: ${resp.status} ${text}`);
        }
        const percent = 5 + Math.floor((chunkIndex + 1) / rdtTotalChunks * 15);
        showUploadProgress('上传配置文件', `RDT: ${chunkIndex + 1}/${rdtTotalChunks} 块`, percent);
      }

      await fetch(`${deviceUrl}/api/update_display_commit`, {
        method: 'POST',
        mode: 'cors',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ type: 'rdt' })
      });

      // 上传 PNG（分块）
      showUploadProgress('上传显示图片', '初始化 PNG 上传...', 20);
      
      await fetch(`${deviceUrl}/api/update_display_start`, {
        method: 'POST',
        mode: 'cors',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ type: 'png' })
      });

      const totalChunks = Math.ceil(pngBase64.length / CHUNK_SIZE);
      for (let offset = 0, chunkIndex = 0; offset < pngBase64.length; offset += CHUNK_SIZE, chunkIndex++) {
        const chunk = pngBase64.substring(offset, offset + CHUNK_SIZE);
        const resp = await fetch(`${deviceUrl}/api/update_display_chunk`, {
          method: 'POST',
          mode: 'cors',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ type: 'png', data: chunk })
        });
        if (!resp.ok) {
          const text = await resp.text();
          throw new Error(`PNG chunk ${chunkIndex + 1}/${totalChunks} upload failed: ${resp.status} ${text}`);
        }
        // 进度从 20% 到 95%
        const percent = 20 + Math.floor((chunkIndex + 1) / totalChunks * 75);
        showUploadProgress('上传显示图片', `PNG: ${chunkIndex + 1}/${totalChunks} 块 (${Math.round(pngBase64.length / 1024)} KB)`, percent);
      }

      showUploadProgress('完成上传', '正在保存文件...', 98);
      
      const commitResp = await fetch(`${deviceUrl}/api/update_display_commit`, {
        method: 'POST',
        mode: 'cors',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ type: 'png' })
      });

      if (!commitResp.ok) {
        const text = await commitResp.text();
        throw new Error(`PNG commit failed: ${commitResp.status} ${text}`);
      }

      const j = await commitResp.json();
      if (j && j.ok) {
        showUploadSuccess('已更新 /rdt/readpaper.rdt 和 /rdt/readpaper.png');
      } else {
        throw new Error(j && j.message ? j.message : '未知错误');
      }
    } catch (e) {
      console.error('pushToDevice error', e);
      showUploadError((e && e.message ? e.message : String(e)).substring(0, 80));
    } finally {
      // 重新根据在线状态决定是否启用
      updatePushButtonState();
    }
  }

  // 根据 heartbeat（device-info 元素是否可见）更新按钮状态
  function updatePushButtonState() {
    const btn = document.getElementById('btnPushToDevice');
    if (!btn) return;
    const devEl = document.getElementById('device-info');
    const online = devEl && devEl.classList && devEl.classList.contains('show');
    btn.disabled = !online;
    if (!online) btn.title = '设备未检测到（请连接到设备热点）';
    else btn.title = '将 readpaper.rdt 与 readpaper.png 上传到设备并保存到 /rdt';
  }

  // 绑定展示配置事件
  const btnAddComponent = document.getElementById('btnAddComponent');
  const btnUploadToWebdav = document.getElementById('btnUploadToWebdav');
  const btnSaveLocal = document.getElementById('btnSaveLocal');
  const btnLoadCurrent = document.getElementById('btnLoadCurrent');
  const btnChangeBg = document.getElementById('btnChangeBg');
  const bgImageInput = document.getElementById('bgImageInput');
  const btnResetConfig = document.getElementById('btnResetConfig');

  if (btnAddComponent) {
    btnAddComponent.addEventListener('click', addComponent);
  }
  if (btnUploadToWebdav) {
    btnUploadToWebdav.addEventListener('click', uploadToWebDAV);
  }
  const btnPushToDevice = document.getElementById('btnPushToDevice');
  if (btnPushToDevice) {
    btnPushToDevice.addEventListener('click', pushToDevice);
  }
  if (btnSaveLocal) {
    btnSaveLocal.addEventListener('click', saveToLocal);
  }
  if (btnLoadCurrent) {
    btnLoadCurrent.addEventListener('click', loadFromWebDAV);
  }
  const btnLoadLocal = document.getElementById('btnLoadLocal');
  if (btnLoadLocal) {
    btnLoadLocal.addEventListener('click', loadFromLocalFile);
  }
  if (btnChangeBg) {
    btnChangeBg.addEventListener('click', changeBgImage);
  }
  if (bgImageInput) {
    bgImageInput.addEventListener('change', handleBgImageSelect);
  }
  if (btnResetConfig) {
    btnResetConfig.addEventListener('click', resetConfig);
  }
  const btnSaveWebDAVSettings = document.getElementById('btnSaveWebDAVSettings');
  if (btnSaveWebDAVSettings) {
    btnSaveWebDAVSettings.addEventListener('click', saveWebDAVSettings);
  }

  // 初始化 push 按钮状态并周期检查 device-info（heartbeat）
  updatePushButtonState();
  setInterval(updatePushButtonState, 3000);

  // 使用事件委托处理组件列表中的删除和文本更新
  const componentList = document.getElementById('componentList');
  if (componentList) {
    componentList.addEventListener('click', (e) => {
      // 处理删除按钮点击
      if (e.target.classList.contains('button') && e.target.classList.contains('error')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          removeComponent(id);
        }
      }
    });
    
    componentList.addEventListener('input', (e) => {
      // 处理文本输入变化
      if (e.target.tagName === 'INPUT' && e.target.type === 'text' && e.target.classList.contains('component-text-input')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentText(id, e.target.value);
        }
      }
      // 处理字体大小输入变化
      if (e.target.tagName === 'INPUT' && e.target.type === 'number' && e.target.classList.contains('component-fontsize-input')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentFontSize(id, e.target.value);
        }
      }
      // 处理旋转角度输入变化
      if (e.target.tagName === 'INPUT' && e.target.type === 'number' && e.target.classList.contains('component-rotation-input')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentRotation(id, e.target.value);
        }
      }
    });
    
    componentList.addEventListener('change', (e) => {
      // 处理字体选择变化（支持 input 和 select）
      if ((e.target.tagName === 'SELECT' || e.target.tagName === 'INPUT') && e.target.classList.contains('component-fontfamily-input')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentFontFamily(id, e.target.value);
        }
      }
      // 处理文本颜色变化
      if (e.target.tagName === 'SELECT' && e.target.classList.contains('component-textcolor-input')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentTextColor(id, e.target.value);
        }
      }
      // 处理背景颜色变化
      if (e.target.tagName === 'SELECT' && e.target.classList.contains('component-bgcolor-input')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentBgColor(id, e.target.value);
        }
      }
      // 处理对齐方式变化
      if (e.target.tagName === 'SELECT' && e.target.classList.contains('component-align-input')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentAlign(id, e.target.value);
        }
      }
      // 处理列/行位置变化
      if (e.target.tagName === 'SELECT' && e.target.classList.contains('component-col-select')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentCol(id, e.target.value);
        }
      }
      if (e.target.tagName === 'SELECT' && e.target.classList.contains('component-row-select')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentRow(id, e.target.value);
        }
      }
      // 处理宽度/高度变化
      if (e.target.tagName === 'INPUT' && e.target.type === 'number' && e.target.classList.contains('component-width-input')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentWidth(id, e.target.value);
        }
      }
      if (e.target.tagName === 'INPUT' && e.target.type === 'number' && e.target.classList.contains('component-height-input')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentHeight(id, e.target.value);
        }
      }
      // 处理x偏移/y偏移变化
      if (e.target.tagName === 'INPUT' && e.target.type === 'number' && e.target.classList.contains('component-xoffset-input')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentXOffset(id, e.target.value);
        }
      }
      if (e.target.tagName === 'INPUT' && e.target.type === 'number' && e.target.classList.contains('component-yoffset-input')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentYOffset(id, e.target.value);
        }
      }
      // 处理分割线颜色变化
      if (e.target.tagName === 'SELECT' && e.target.classList.contains('component-linecolor-input')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentLineColor(id, e.target.value);
        }
      }
      // 处理分割线样式变化
      if (e.target.tagName === 'SELECT' && e.target.classList.contains('component-linestyle-input')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentLineStyle(id, e.target.value);
        }
      }
      // 处理分割线粗细变化
      if (e.target.tagName === 'INPUT' && e.target.type === 'number' && e.target.classList.contains('component-linewidth-input')) {
        const id = parseInt(e.target.dataset.componentId);
        if (id && !isNaN(id)) {
          updateComponentLineWidth(id, e.target.value);
        }
      }
    });
  }

  // 监听 bgpic 复选框变化
  const chkIncludeBgPic = document.getElementById('chkIncludeBgPic');
  if (chkIncludeBgPic) {
    chkIncludeBgPic.addEventListener('change', (e) => {
      const wasChecked = hasBgPic;
      hasBgPic = e.target.checked;
      
      // 如果取消勾选，提示用户
      if (!hasBgPic && wasChecked && backgroundImage) {
        if (!confirm('取消勾选将不会在上传时包含背景图，但本地预览仍保留。确定吗？')) {
          e.target.checked = true;
          hasBgPic = true;
          return;
        }
      }
      
      updateBackgroundPreview();
    });
  }

  // ========== 初始化 ==========

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', async () => {
      activateTab('wifi');
      await loadSystemFonts(); // 先加载字体
      // 先尽快加载本地草稿并初始化预览，避免被 WebDAV 探测阻塞
      await loadDisplayConfigFromLocalStorage();
      initScreenPreview();

      // 后台异步加载设备配置与 WebDAV 探测，不阻塞 UI
      loadWifiConfig();
      loadConfig();
      updateLoadCurrentButtonState();
      updateUploadButtonState();
    });
  } else {
    (async () => {
      activateTab('wifi');
      await loadSystemFonts(); // 先加载字体
      // 先尽快加载本地草稿并初始化预览，避免被 WebDAV 探测阻塞
      await loadDisplayConfigFromLocalStorage();
      initScreenPreview();

      // 后台异步加载设备配置与 WebDAV 探测，不阻塞 UI
      loadWifiConfig();
      loadConfig();
      updateLoadCurrentButtonState();
      updateUploadButtonState();
    })();
  }
})();
