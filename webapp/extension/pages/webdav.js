(function(){
  const API_BASE = 'http://192.168.4.1';
  const urlEl = document.getElementById('webdavUrl');
  const userEl = document.getElementById('webdavUser');
  const passEl = document.getElementById('webdavPassword');
  const statusEl = document.getElementById('webdavStatus');
  const btnReload = document.getElementById('btnReload');
  const btnSave = document.getElementById('btnSave');
  const btnToggle = document.getElementById('togglePassword');

  const setStatus = (text, type = 'info') => {
    if (!statusEl) return;
    statusEl.textContent = text;
    statusEl.classList.remove('text-success', 'text-error');
    if (type === 'success') statusEl.classList.add('text-success');
    if (type === 'error') statusEl.classList.add('text-error');
  };

  const setDisabled = (disabled) => {
    if (btnReload) btnReload.disabled = disabled;
    if (btnSave) btnSave.disabled = disabled;
  };

  async function loadConfig(){
    setDisabled(true);
    setStatus('读取中...');
    try {
      const r = await fetch(`${API_BASE}/api/webdav_config`, { method: 'GET' });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const j = await r.json();
      if (!j || j.ok !== true) throw new Error(j && j.message ? j.message : '读取失败');
      const cfg = j.config || {};
      if (urlEl) urlEl.value = cfg.url || '';
      if (userEl) userEl.value = cfg.username || '';
      if (passEl) passEl.value = cfg.password || '';
      setStatus('已同步设备配置', 'success');
    } catch (e) {
      setStatus(`读取失败: ${e.message}`, 'error');
    } finally {
      setDisabled(false);
    }
  }

  async function saveConfig(){
    setDisabled(true);
    setStatus('保存中...');
    try {
      const payload = {
        url: urlEl ? urlEl.value.trim() : '',
        username: userEl ? userEl.value.trim() : '',
        password: passEl ? passEl.value : ''
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
      setStatus('已保存到设备', 'success');
    } catch (e) {
      setStatus(`保存失败: ${e.message}`, 'error');
    } finally {
      setDisabled(false);
    }
  }

  if (btnReload) btnReload.addEventListener('click', loadConfig);
  if (btnSave) btnSave.addEventListener('click', saveConfig);
  if (btnToggle && passEl) {
    btnToggle.addEventListener('click', () => {
      const next = passEl.type === 'password' ? 'text' : 'password';
      passEl.type = next;
      btnToggle.textContent = next === 'password' ? '显示' : '隐藏';
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', loadConfig);
  } else {
    loadConfig();
  }
})();
