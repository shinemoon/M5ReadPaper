(function(){
  const API_BASE = 'http://192.168.4.1'; // 若在同域热点内访问，可留空改为 ''
  const PAGE_SIZE = 10;
  let currentCat = 'book';
  let currentPage = 1;
  let cache = { book:null, font:null, image:null, screenshot:null };
  let selectedFiles = [];
  let selectedForDelete = new Set();
  
  // 后端分页支持检测（首次请求时自动检测）
  let paginationSupported = null; // null=未检测, true=支持, false=不支持

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

  const hints = {
    book:'支持 unicode/GBK 编码的 txt 文件。',
    font:'请上传工具生成的 font.bin；<1.2.9 旧版本字体建议重新生成。',
    image:'锁屏图片建议 540x960，支持透明 png，优先同名图片 > default.png > 系统自带。',
    screenshot:'设备截图存储目录。双击屏幕左上角(230,0)-(310,80)区域可触发截图。仅支持下载和删除，不支持上传。'
  };
  const catNames = { book:'书籍', font:'字体', image:'锁屏', screenshot:'截图' };

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
    uploadTitle.textContent=catNames[cat]+'-文件上传';
    hint.textContent = hints[cat];
    // 更新 tab 激活
    document.querySelectorAll('#catTabs a').forEach(a=>{
      if(a.dataset.cat===cat) a.classList.add('active'); else a.classList.remove('active');
    });
    // clear any selected-for-delete state and selectAll checkbox
    selectedForDelete.clear();
    const delBtn = document.getElementById('btnDeleteSelected'); if(delBtn) delBtn.disabled = true;
    const selAll = document.getElementById('selectAll'); if(selAll){ selAll.checked=false; selAll.indeterminate=false; }
    
    // 对于screenshot tab，隐藏上传区域
    if(uploadBox){
      if(cat === 'screenshot'){
        uploadBox.style.display = 'none';
      } else {
        uploadBox.style.display = 'block';
      }
    }
    
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
      
      // 首次请求时检测后端是否支持分页
      if(paginationSupported === null){
        // 尝试分页请求
        const testUrl = `${API_BASE}/list/${currentCat}?page=1&perPage=${PAGE_SIZE}`;
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
        const r = await fetch(`${API_BASE}/list/${currentCat}?page=${currentPage}&perPage=${PAGE_SIZE}`);
        if(!r.ok) throw new Error('HTTP '+r.status);
        data = await r.json();
      } else {
        // 已知不支持分页，请求全部数据
        const r = await fetch(`${API_BASE}/list/${currentCat}`);
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
      fileBody.innerHTML = '<tr><td colspan="5" class="text-center">暂无文件</td></tr>';
      pager.classList.add('hidden');
      updateUploadState(0);
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
    fileBody.innerHTML = list.map(f=>{
      // Prefer server-supplied full path (untruncated). Fallback to constructed path.
      const fullPath = f.path ? f.path : `/${currentCat}/${f.name}`;
      const disableDelete = !!f.isCurrent;
      // show small badge when book has .idx (isIdxed)
      const idxBadge = (currentCat === 'book' && f.isIdxed) ? ` <span class="badge idx-badge" title="存在目录 (.idx)">目录</span>` : '';
      const currentBadge = f.isCurrent ? ` <span class='badge-current'>${currentCat==='book'?'正在阅读':currentCat==='font'?'当前字体':'当前'}</span>` : '';
      return `<tr>
        <td><input type="checkbox" class="file-select-checkbox" data-path="${encodeURIComponent(fullPath)}" ${disableDelete? 'disabled' : ''}></td>
        <td class="file-name-cell">${f.name}${currentBadge}${idxBadge}</td>
        <td>${f.type==='file'?formatSize(f.size):''}</td>
        <td>${f.isCurrent? '✔':''}</td>
        <td class='file-actions nowrap'>
          ${f.type==='file'?`<a href='${API_BASE}/download?path=${encodeURIComponent(fullPath)}' data-path='${encodeURIComponent(fullPath)}' class='download-link button is-small outline' title='下载'>下载</a>`:''}
          <button class='button is-small outline' data-del='${fullPath}' ${disableDelete?'disabled':''}>删除</button>
          ${currentCat==='book' && f.type==='file'?`<button class='button is-small outline' data-record='${fullPath}' title='查看阅读记录'>记录</button>`:''}
        </td>
      </tr>`;
    }).join('');

    // 绑定删除按钮
    // bind delete buttons (single or batch for screenshots)
    fileBody.querySelectorAll('button[data-del]').forEach(btn=>{
      btn.onclick = async ()=>{
        const path = btn.getAttribute('data-del');

        // If current category is screenshot and multiple items are selected,
        // treat this as a batch delete for all selected files.
        if(currentCat === 'screenshot' && selectedForDelete.size > 1){
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
    if(currentCat === 'book'){
      fileBody.querySelectorAll('button[data-record]').forEach(btn=>{
        btn.onclick = ()=>{
          const bookPath = btn.getAttribute('data-record');
          window.open(`readingRecord.html?book=${encodeURIComponent(bookPath)}`, '_blank');
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
          if(currentCat === 'screenshot' && selectedForDelete.size > 1 && window.JSZip){
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
    if(count===0){ info.textContent = "何处见那'遁去的一'?"; btnUpload.disabled = selectedFiles.length===0; info.style.color='var(--grey)' }
    else if(count>=99){ info.textContent = '宁缺毋滥，九九归一'; btnUpload.disabled = true; info.style.color='#b30000'; }
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

  // 拖拽
  ['dragenter','dragover','dragleave','drop'].forEach(ev=>{
    uploadBox.addEventListener(ev,e=>{ e.preventDefault(); e.stopPropagation(); });
  });
  ['dragenter','dragover'].forEach(ev=>uploadBox.addEventListener(ev,()=>uploadBox.classList.add('dragover')));
  ['dragleave','drop'].forEach(ev=>uploadBox.addEventListener(ev,()=>uploadBox.classList.remove('dragover')));
  uploadBox.addEventListener('drop', e=>{ selectedFiles = Array.from(e.dataTransfer.files); fileInput.value=''; btnUpload.textContent='上传 '+selectedFiles.length+' 个文件'; btnUpload.disabled = selectedFiles.length===0; updateUploadState(cache[currentCat]?cache[currentCat].length:0); });

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
    // Implement automatic retry with exponential backoff to match original template.html behaviour.
    const maxRetries = 2; // same as template.html
    const baseDelay = 500; // ms
    const currentTab = currentCat;

    return (async function(){
      for(let attempt=0; attempt<=maxRetries; attempt++){
        try{
          const res = await new Promise((resolve,reject)=>{
            const xhr = new XMLHttpRequest();
            xhr.open('POST', `${API_BASE}/upload?tab=${encodeURIComponent(currentTab)}`);
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
            const fd = new FormData(); fd.append('data', file); xhr.send(fd);
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

  // 初始绑定 Tabs
  document.querySelectorAll('#catTabs a').forEach(a=>{
    a.onclick = ()=>{ switchCat(a.dataset.cat); };
  });

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
    btnBatchRecords.onclick = ()=>{
      if(selectedForDelete.size === 0){
        toast('请先选择要查看阅读记录的书籍', 'error', 3000);
        return;
      }
      const books = Array.from(selectedForDelete).join(',');
      window.open(`readingRecord.html?books=${encodeURIComponent(books)}`, '_blank');
    };
  }

  // All reading records button
  const btnAllRecords = document.getElementById('btnAllRecords');
  if(btnAllRecords){
    btnAllRecords.onclick = ()=>{
      window.open(`readingRecord.html?all=true`, '_blank');
    };
  }

  // 初始化
  switchCat('book');

  // Ensure the top nav's "文件管理" tab appears active/text-dark on this page
  // and clicking it does nothing (no-op) while the user is already on filemanager.html.
  (function ensureFileTabActive(){
    const start = Date.now();
    const iv = setInterval(()=>{
      const tabs = document.querySelectorAll('.tabs a');
      if(tabs && tabs.length){
        // find by visible text (中文), fallback to href matching
        let fileTab = Array.from(tabs).find(a=> (a.textContent||'').trim()==='文件管理');
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
