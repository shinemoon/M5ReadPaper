// data_center.js — 数据中心（IndexedDB 存储 + 假数据调试）
(function(){
  const DB_NAME = 'readpaper_data_center';
  const DB_VERSION = 1;
  const STORE = 'reading_records';

  // 当为 true 时，不使用真实 IndexedDB，而使用内存假数据源
  window.__DATA_CENTER_DEBUG = window.__DATA_CENTER_DEBUG || false;

  // override global showmethemoney to control this page's debug flag
  window.showmethemoney = function(enable) {
    window.__DATA_CENTER_DEBUG = !!enable;
    const debugToggle = document.getElementById('debugToggle');
    if (debugToggle) debugToggle.checked = !!enable;
    render();
    console.log('[data_center] showmethemoney ->', enable);
  };

  // --- IndexedDB helpers ---
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

  async function addRecordToDB(record) {
    const db = await openDB();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, 'readwrite');
      const store = tx.objectStore(STORE);
      // ensure fields
      const rec = Object.assign({}, record);
      if (!rec.bookname && rec.book_name) rec.bookname = rec.book_name;
      if (!rec.timestamp) rec.timestamp = Date.now();
      store.add(rec).onsuccess = (e) => resolve(e.target.result);
      tx.onerror = (e) => reject(e.target.error || e);
    });
  }

  async function getAllFromDB() {
    const db = await openDB();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, 'readonly');
      const store = tx.objectStore(STORE);
      const req = store.getAll();
      req.onsuccess = () => resolve(req.result || []);
      req.onerror = (e) => reject(e.target.error || e);
    });
  }

  async function clearDB() {
    const db = await openDB();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, 'readwrite');
      const store = tx.objectStore(STORE);
      const req = store.clear();
      req.onsuccess = () => resolve();
      req.onerror = (e) => reject(e.target.error || e);
    });
  }

  async function deleteById(id) {
    const db = await openDB();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, 'readwrite');
      const store = tx.objectStore(STORE);
      const req = store.delete(Number(id));
      req.onsuccess = () => resolve();
      req.onerror = (e) => reject(e.target.error || e);
    });
  }

  // --- Fake data source for debug mode ---
  const FAKE_DB = [];
  function seedFakeData(count = 20) {
    FAKE_DB.length = 0;
    const now = Date.now();
    for (let i = 0; i < count; i++) {
      const bname = `测试书籍_${i+1}.txt`;
      const rec = {
        book_path: `/sd/book/${bname}`,
        book_name: bname,
        bookname: bname,
        total_hours: Math.floor(Math.random() * 30),
        total_minutes: Math.floor(Math.random() * 60),
        hourly_records: {},
        daily_summary: {},
        monthly_summary: {},
        timestamp: now - Math.floor(Math.random() * 1000 * 60 * 60 * 24 * 30)
      };
      FAKE_DB.push(rec);
    }
  }

  // --- UI rendering ---
  async function loadRecords() {
    if (window.__DATA_CENTER_DEBUG) {
      return FAKE_DB.slice();
    }
    try {
      const all = await getAllFromDB();
      return all;
    } catch (e) {
      console.error('loadRecords error', e);
      return [];
    }
  }

  function formatTime(ts) {
    try {
      const d = new Date(Number(ts));
      return d.toLocaleString();
    } catch (e) { return String(ts); }
  }

  async function render() {
    const container = document.getElementById('recordsContainer');
    const summary = document.getElementById('listSummary');
    if (!container || !summary) return;
    container.innerHTML = '加载中…';
    const records = await loadRecords();

    const q = (document.getElementById('searchInput') || {}).value || '';
    const filtered = q ? records.filter(r => (r.bookname||r.book_name||'').indexOf(q) !== -1) : records;

    summary.textContent = `共 ${filtered.length} 条记录 （${window.__DATA_CENTER_DEBUG ? '调试假数据' : 'IndexedDB'}）`;
    if (filtered.length === 0) {
      container.innerHTML = '<div class="no-data">暂无记录</div>';
      return;
    }

    const table = document.createElement('table');
    table.style.width = '100%';
    table.style.borderCollapse = 'collapse';
    const thead = document.createElement('thead');
    thead.innerHTML = '<tr><th style="text-align:left">ID</th><th style="text-align:left">书名</th><th style="text-align:left">时间</th><th style="text-align:left">摘要</th><th></th></tr>';
    table.appendChild(thead);
    const tbody = document.createElement('tbody');

    filtered.forEach(r => {
      const tr = document.createElement('tr');
      tr.style.borderTop = '1px solid #eee';
      const idCell = document.createElement('td');
      idCell.textContent = r.id || '';
      const nameCell = document.createElement('td');
      const bookNameText = r.bookname || r.book_name || (r.book_path ? r.book_path.split('/').pop() : '—');
      // create clickable link to readingRecord.html showing this book's records
      const a = document.createElement('a');
      a.textContent = bookNameText;
      // determine book path param if available, otherwise use book name
      const bookPathParam = encodeURIComponent(r.book_path || (`/sd/book/${bookNameText}`));
      a.href = `readingRecord.html?book=${bookPathParam}`;
      a.style.cursor = 'pointer';
      nameCell.appendChild(a);
      const timeCell = document.createElement('td');
      timeCell.textContent = formatTime(r.timestamp || r._timestamp || '—');
      const summaryCell = document.createElement('td');
      summaryCell.textContent = (r.total_hours ? `${r.total_hours}h ` : '') + (r.total_minutes ? `${r.total_minutes}m` : '—');
      const actionCell = document.createElement('td');
      const delBtn = document.createElement('button');
      delBtn.className = 'button is-small outline';
      delBtn.textContent = '删除';
      delBtn.addEventListener('click', async () => {
        if (window.__DATA_CENTER_DEBUG) {
          const idx = FAKE_DB.indexOf(r);
          if (idx >= 0) FAKE_DB.splice(idx,1);
          render();
        } else {
          if (!r.id) return alert('记录无 id，无法删除');
          await deleteById(r.id);
          render();
        }
      });
      actionCell.appendChild(delBtn);

      tr.appendChild(idCell);
      tr.appendChild(nameCell);
      tr.appendChild(timeCell);
      tr.appendChild(summaryCell);
      tr.appendChild(actionCell);
      tbody.appendChild(tr);
    });

    table.appendChild(tbody);
    container.innerHTML = '';
    container.appendChild(table);
  }

  // --- wire UI ---
  function setupUI() {
    const debugToggle = document.getElementById('debugToggle');
    const seedFake = document.getElementById('seedFake');
    const clearBtn = document.getElementById('clearDb');
    const exportBtn = document.getElementById('exportBtn');
    const searchInput = document.getElementById('searchInput');

    if (debugToggle) {
      debugToggle.checked = !!window.__DATA_CENTER_DEBUG;
      debugToggle.addEventListener('change', () => {
        window.__DATA_CENTER_DEBUG = !!debugToggle.checked;
        // call global helper
        if (window.showmethemoney) window.showmethemoney(!!debugToggle.checked);
        render();
      });
    }
    if (seedFake) seedFake.addEventListener('click', () => { seedFakeData(30); render(); });
    if (clearBtn) clearBtn.addEventListener('click', async () => { if (confirm('确定清空数据库？此操作不可恢复')) { await clearDB(); render(); } });
    if (exportBtn) exportBtn.addEventListener('click', async () => {
      const data = await loadRecords();
      const blob = new Blob([JSON.stringify(data, null, 2)], {type:'application/json'});
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url; a.download = 'readpaper_records.json'; document.body.appendChild(a); a.click(); a.remove(); URL.revokeObjectURL(url);
    });
    if (searchInput) searchInput.addEventListener('input', () => render());
  }

  // initialize
  document.addEventListener('DOMContentLoaded', async () => {
    setupUI();
    // if debug flag initially true, seed some fake data so UI isn't empty
    if (window.__DATA_CENTER_DEBUG) seedFakeData(20);
    await render();
  });

})();
