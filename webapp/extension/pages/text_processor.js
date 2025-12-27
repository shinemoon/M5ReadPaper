// text_processor.js — placeholder for 文本处理 页面 logic
document.addEventListener('DOMContentLoaded', () => {
	const el = id => document.getElementById(id);
	const fileInput = el('fileInput');
	const dropArea = el('dropArea');
	const fileListEl = el('fileList');
	const clearListBtn = el('clearList');
	const generateBtn = el('generateBtn');
	const cancelBtn = el('cancelBtn');
	const progressBar = el('progressBar');
	const logEl = el('log');
	const optGenerateIndexEl = el('opt_generate_index');
	const optIdxDiagEl = el('opt_idx_diag');
	const optIndexRegex1 = el('opt_index_regex1');
	const optIndexRegex2 = el('opt_index_regex2');
	const optIndexRegex3 = el('opt_index_regex3');
	const indexOptions = el('indexOptions');
	// previewer / previewerInfo 已移除：预览机制被取消

	const CHUNK_SIZE = 128 * 1024; // 128KB
	const SAMPLE_SIZE = 128 * 1024; // 128KB for detection (increased for better accuracy)
	// 编码尝试顺序优化：先尝试有 BOM 的，然后常见中文编码，最后 UTF-8
	// GB18030 是 GBK 的超集，能更好地处理中文
	const tryEncodings = ['gb18030','gbk','utf-8','utf-16le','utf-16be','shift_jis','euc-jp','iso-8859-1'];

	// 默认正则表达式：匹配常见的“第...章 / 第...回 / 第...卷”形式
	// 支持阿拉伯数字与常见中文数字（零一二三四五六七八九十百千两）
	const DEFAULT_REGEX_1 = '/(?:^|\\s)第\\s*[0-9零一二三四五六七八九十百千两〇]+\\s*(?:章|回|卷)[^\\n]*/u';
	const DEFAULT_REGEX_2 = '';
	const DEFAULT_REGEX_3 = '';

	// 从 localStorage 加载配置
	function loadConfig(){
		try{
			const saved = localStorage.getItem('text_processor_toc_config');
			if(saved){
				const cfg = JSON.parse(saved);
				if(optIndexRegex1 && cfg.regex1 !== undefined) optIndexRegex1.value = cfg.regex1;
				if(optIndexRegex2 && cfg.regex2 !== undefined) optIndexRegex2.value = cfg.regex2;
				if(optIndexRegex3 && cfg.regex3 !== undefined) optIndexRegex3.value = cfg.regex3;
				if(optGenerateIndexEl && cfg.generateIndex !== undefined) optGenerateIndexEl.checked = cfg.generateIndex;
				log('已恢复上次的目录配置');
			}
		}catch(e){
			log('加载配置失败: ' + e.message);
		}
	}

	// 保存配置到 localStorage
	function saveConfig(){
		try{
			const cfg = {
				regex1: optIndexRegex1 ? optIndexRegex1.value : DEFAULT_REGEX_1,
				regex2: optIndexRegex2 ? optIndexRegex2.value : DEFAULT_REGEX_2,
				regex3: optIndexRegex3 ? optIndexRegex3.value : DEFAULT_REGEX_3,
				generateIndex: optGenerateIndexEl ? optGenerateIndexEl.checked : false
			};
			localStorage.setItem('text_processor_toc_config', JSON.stringify(cfg));
		}catch(e){
			log('保存配置失败: ' + e.message);
		}
	}

	// 恢复默认配置
	function restoreDefaults(){
		if(optIndexRegex1) optIndexRegex1.value = DEFAULT_REGEX_1;
		if(optIndexRegex2) optIndexRegex2.value = DEFAULT_REGEX_2;
		if(optIndexRegex3) optIndexRegex3.value = DEFAULT_REGEX_3;
		saveConfig();
		log('已恢复默认正则表达式');
	}

	// 加载配置（页面启动时）
	loadConfig();

	// 监听配置变化并自动保存
	if(optIndexRegex1){
		optIndexRegex1.addEventListener('input', ()=>{
			// 当第一个正则被清空时，恢复默认值
			if(optIndexRegex1.value.trim() === ''){
				restoreDefaults();
			}else{
				saveConfig();
			}
		});
	}
	if(optIndexRegex2) optIndexRegex2.addEventListener('input', saveConfig);
	if(optIndexRegex3) optIndexRegex3.addEventListener('input', saveConfig);
	if(optGenerateIndexEl) optGenerateIndexEl.addEventListener('change', saveConfig);

	let files = []; // {id, file, status, progress, parts, dom}
	let running = false;
	let abortAll = false;

	function log(msg){ const now=new Date().toLocaleTimeString(); logEl.textContent += `[${now}] ${msg}\n`; logEl.scrollTop = logEl.scrollHeight; }
	function fmtSize(bytes){ if(bytes===0) return '0B'; const u=['B','KB','MB','GB']; const i=Math.floor(Math.log(bytes)/Math.log(1024)); return (bytes/Math.pow(1024,i)).toFixed(1)+u[i]; }
	function escapeHtml(s){ return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }
	// truncate to at most n user-perceived characters (code points), add ellipsis if truncated
	function truncateStr(s, n){ if(!s) return s; const a = Array.from(s); return a.length > n ? a.slice(0,n).join('') + '…' : s; }

	function addFilesFromList(list){
		for(const f of list){
			const id = Date.now().toString(36) + Math.random().toString(36).slice(2,8);
			const item = {id, file: f, status: '待处理', progress: 0, parts: [], encoding: null, dom: null};
			files.push(item);
			renderFileItem(item);
		}
		updateGlobalProgress();
	}

	function renderFileItem(item){
		const row = document.createElement('div');
		row.className = 'file-item col-12';
		row.innerHTML = `
			<div class="row align-center">
				<div class="col-5">
					<strong>${escapeHtml(item.file.name)}</strong>
					<span class="muted">${fmtSize(item.file.size)}</span>
				</div>
				<div class="col-3"><progress id="prog-${item.id}" value="0" max="100" class="progress-full"></progress></div>
				<div class="col-4 text-right">
					<span id="status-${item.id}">${item.status}</span>
					<span style="margin-top:6px; margin-left:10px;"><button id="start-${item.id}" class="button is-small primary is-hidden">处理</button></span>
					<span style="margin-top:6px; margin-left:10px;"><button id="remove-${item.id}" class="button is-small outline">移除</button></span>
				</div>
			</div>
		`;

		fileListEl.appendChild(row);
		item.dom = row;
		// start (per-file preview only)
		row.querySelector(`#start-${item.id}`).onclick = async ()=>{
			try{
				// 取消预览机制：改为单文件直接处理
				if(running){ alert('当前有任务正在运行，请稍后或取消后再试'); return; }
				await processQueue([item]);
			}catch(e){ log('处理失败: '+e.message); alert('处理失败: '+e.message); }
		};

		// remove action
		row.querySelector(`#remove-${item.id}`).onclick = ()=>{
			files = files.filter(f=>f.id!==item.id);
			row.remove(); updateGlobalProgress();
		};
	}

	// 预览机制已移除（scanAndPreviewItem 已删除）

	function updateGlobalProgress(){
		const total = files.reduce((s,i)=>s+i.file.size,0);
		const done = files.reduce((s,i)=>s + (i.file.size * (i.progress/100)),0);
		const pct = total>0? Math.round((done/total)*100) : 0;
		progressBar.value = pct;
		// 当没有待处理文件且未在运行时，禁用“移除所有”按钮；有文件或正在运行时启用
		if(cancelBtn) cancelBtn.disabled = (!running && files.length === 0);
	}

	// drag & drop
	['dragenter','dragover','dragleave','drop'].forEach(ev=> dropArea.addEventListener(ev, e=>{ e.preventDefault(); e.stopPropagation(); }));
	['dragenter','dragover'].forEach(ev=> dropArea.addEventListener(ev, ()=> dropArea.classList.add('dragover')));
	['dragleave','drop'].forEach(ev=> dropArea.addEventListener(ev, ()=> dropArea.classList.remove('dragover')));
	dropArea.addEventListener('drop', e=>{ const list = Array.from(e.dataTransfer.files||[]); if(list.length) addFilesFromList(list); });

	fileInput.onchange = ()=>{ const list = Array.from(fileInput.files||[]); if(list.length) addFilesFromList(list); fileInput.value=''; };
	clearListBtn.onclick = ()=>{ files=[]; fileListEl.innerHTML=''; updateGlobalProgress(); };

	// index options show/hide and enable/disable inputs
	if(indexOptions && optGenerateIndexEl){
		const toggleIndexInputs = ()=>{
			const on = !!optGenerateIndexEl.checked;
			if(optIndexRegex1) optIndexRegex1.disabled = !on;
			if(optIndexRegex2) optIndexRegex2.disabled = !on;
			if(optIndexRegex3) optIndexRegex3.disabled = !on;
			// 折叠/展开：使用 class 切换以触发 CSS 过渡动画（display:none 无法过渡）
			indexOptions.classList.toggle('collapsed', !on);
			// 同时保留轻微透明反馈（可选），并设置无障碍属性
			indexOptions.style.opacity = on ? '1' : '0.6';
			indexOptions.setAttribute('aria-hidden', on ? 'false' : 'true');
		};
		toggleIndexInputs();
		optGenerateIndexEl.addEventListener('change', toggleIndexInputs);
	}

	// bind right-side buttons to batch actions
	if(generateBtn){
		generateBtn.onclick = ()=>{
			// if user selected files via file input, add them first
			const list = Array.from(fileInput.files||[]);
			if(list.length) addFilesFromList(list);
			if(running) return; if(files.length) processQueue(files.slice()); else alert('请选择文件');
		};
	}
	if(cancelBtn){
		cancelBtn.onclick = ()=>{
			// 如果当前有任务在运行：请求取消全部任务
			if(running){
				abortAll = true; log('已请求取消全部任务');
			}
			// 无论是否在运行，移除所有文件并清理界面
			files = [];
			fileListEl.innerHTML = '';
			updateGlobalProgress();
			log('已移除所有文件');
		};
	}

	// 全局预览按钮已移除；使用每个文件条目的“预览”按钮来显示针对性预览

	// previewApply 已移除（预览被取消）

	async function processQueue(queue){
		if(running) return;
		running = true; abortAll = false;
		if(generateBtn) generateBtn.disabled = true; if(cancelBtn) cancelBtn.disabled = false;
		log('开始批量处理 '+queue.length+' 个文件');
		const results = [];
		for(const item of queue){
			if(abortAll) { item.status='已取消'; setStatus(item); break; }
			if(item.status === '已完成') continue;
			try{
				const res = await processFile(item);
				if(res) results.push(res);
			}catch(err){
				log(`文件 ${item.file.name} 失败: ${err.message}`);
			}
		}
		// 汇总所有结果并显示在 #@convert-info 中（仅显示综合统计）
		try{
			const convEl = el('convert-info');
			if(results.length > 0){
								const agg = { files: results.length, totalModified:0, trimLeadingChanges:0, ensureTwoSpaceChanges:0, removedBlankLines:0, bytesRead:0, bytesWritten:0, timeMs:0, indexEntries:0 };
				for(const r of results){
					agg.totalModified += r.totalModified || 0;
					agg.trimLeadingChanges += r.trimLeadingChanges || 0;
					agg.ensureTwoSpaceChanges += r.ensureTwoSpaceChanges || 0;
					agg.removedBlankLines += r.removedBlankLines || 0;
					agg.bytesRead += r.bytesRead || 0;
					agg.bytesWritten += r.bytesWritten || 0;
					agg.timeMs += r.timeMs || 0;
										agg.indexEntries += r.indexEntries || 0;
				}
				if(convEl){
					convEl.innerHTML = `
						<div class="convert-summary">
						  <div class="convert-summary-title">转换汇总 (<span class="count">${agg.files}</span> 个文件)</div>
						  <table class="convert-summary-table" role="table" aria-label="转换汇总">
						    <tbody>
						      <tr><th>总改动行数</th><td><span class="num">${agg.totalModified}</span></td></tr>
						      <tr><th>去除多余空行</th><td><span class="num">${agg.removedBlankLines}</span></td></tr>
						      <tr><th>行首顶格改动</th><td><span class="num">${agg.trimLeadingChanges}</span></td></tr>
						      <tr><th>添加2空格改动</th><td><span class="num">${agg.ensureTwoSpaceChanges}</span></td></tr>
															<tr><th>目录项数</th><td><span class="num">${agg.indexEntries}</span></td></tr>
						      <tr><th>输入字节</th><td><span class="num">${fmtSize(agg.bytesRead)}</span></td></tr>
						      <tr><th>输出字节</th><td><span class="num">${fmtSize(agg.bytesWritten)}</span></td></tr>
						      <tr><th>总耗时</th><td><span class="num">${Math.round(agg.timeMs)} ms</span></td></tr>
						    </tbody>
						  </table>
						</div>`;
				} else {
					// 如果找不到元素则在日志中输出汇总
					log('未找到 #convert-info 元素，已在日志中输出汇总：');
					log(`汇总: 文件=${agg.files}; 总改动行=${agg.totalModified}; 去除空行=${agg.removedBlankLines}; 顶格=${agg.trimLeadingChanges}; 加2空格=${agg.ensureTwoSpaceChanges}; in=${fmtSize(agg.bytesRead)} out=${fmtSize(agg.bytesWritten)}; 用时=${Math.round(agg.timeMs)}ms`);
				}
			} else {
				if(convEl) convEl.innerHTML = '<div style="padding:8px;color:#666">未处理任何文件或所有文件均未产生改动。</div>';
			}
		}catch(e){ log('生成汇总时出错: '+ (e && e.message)); }
		running = false; if(generateBtn) generateBtn.disabled = false; if(cancelBtn) cancelBtn.disabled = true; updateGlobalProgress(); log('批量处理结束');
	}

	function setProgress(item, pct){ item.progress = pct; const p = item.dom.querySelector(`#prog-${item.id}`); if(p) p.value = Math.round(pct); updateGlobalProgress(); }
	function setStatus(item, s){ item.status = s; const sp = item.dom.querySelector(`#status-${item.id}`); if(sp) sp.textContent = s; }

	async function processFile(item){
		setStatus(item,'读取样本'); log('读取样本: '+item.file.name);
		const sampleSize = Math.min(SAMPLE_SIZE, item.file.size);
		let detected = null;
		try{
			const sampleBuf = await item.file.slice(0, sampleSize).arrayBuffer();
			detected = detectEncoding(sampleBuf);
			log(`文件 ${item.file.name} 检测编码: ${detected} (样本大小: ${fmtSize(sampleSize)})`);
		}catch(e){ setStatus(item,'读取错误'); throw new Error('读取样本失败'); }
		setStatus(item,'编码: '+detected);

		// read index generation options (defer matching until after out file is created)
		const doIndex = optGenerateIndexEl ? !!optGenerateIndexEl.checked : false;
		const regexStrs = [optIndexRegex1 ? (optIndexRegex1.value||'') : '', optIndexRegex2 ? (optIndexRegex2.value||'') : '', optIndexRegex3 ? (optIndexRegex3.value||'') : ''];
		let indexMatches = [];

		// read user options
		const optTrim = el('opt_trim_leading');
		const optRemoveBlank = el('opt_remove_extra_blank');
		const trimLeading = optTrim ? !!optTrim.checked : false;
		const removeExtraBlank = optRemoveBlank ? !!optRemoveBlank.checked : false;

		const decoder = createDecoder(detected);
		const encoder = new TextEncoder();
		const parts = [];
		// stats for this file
		const stats = {
			totalModified: 0,
			trimLeadingChanges: 0,
			ensureTwoSpaceChanges: 0,
			removedBlankLines: 0,
			bytesRead: 0,
			bytesWritten: 0,
			startTime: Date.now(),
			encoding: detected
		};
		let offset = 0;
		const size = item.file.size;

		// determine whether original file ends with newline (LF or CR)
		let fileEndsWithNewline = false;
		if(size > 0){
			try{
				const lastBuf = await item.file.slice(size-1, size).arrayBuffer();
				const b = new Uint8Array(lastBuf)[0];
				if(b === 10 || b === 13) fileEndsWithNewline = true;
			}catch(_){ /* ignore */ }
		}

		// per-file streaming state for line processing
		item._state = { leftover: '', lastBlankEmitted: false };

		try{
			while(offset < size){
				if(abortAll) { setStatus(item,'已取消'); throw new Error('已取消'); }
				const end = Math.min(offset + CHUNK_SIZE, size);
				const buf = await item.file.slice(offset, end).arrayBuffer();
				stats.bytesRead += buf.byteLength;
				const chunkStr = decoder.decode(buf, {stream:true});
				if(chunkStr == null) { offset = end; continue; }

				// combine leftover and normalize line endings
				let s = item._state.leftover + chunkStr;
				s = s.replace(/\r\n/g, '\n');
				const endsWithNewline = s.endsWith('\n');
				const lines = s.split('\n');
				if(!endsWithNewline) item._state.leftover = lines.pop(); else item._state.leftover = '';

				// process complete lines
				for(const line of lines){
					const isEmpty = line.trim().length === 0;
					if(removeExtraBlank && isEmpty){
						if(item._state.lastBlankEmitted){ stats.removedBlankLines++; stats.totalModified++; continue; }
						const enc = encoder.encode('\n'); parts.push(enc); stats.bytesWritten += enc.length; item._state.lastBlankEmitted = true; continue;
					}
					if(isEmpty){ const enc = encoder.encode('\n'); parts.push(enc); stats.bytesWritten += enc.length; item._state.lastBlankEmitted = true; continue; }

					// non-empty line
					let outLine = trimLeading ? line.replace(/^\s+/, '') : ('  ' + line.replace(/^\s*/, ''));
					const enc = encoder.encode(outLine + '\n'); parts.push(enc); stats.bytesWritten += enc.length;
					if(outLine !== line){ stats.totalModified++; if(trimLeading){ if(/^\s+/.test(line) && !/^\s/.test(outLine)) stats.trimLeadingChanges++; } else { if(!/^ {2}/.test(line) && /^ {2}/.test(outLine)) stats.ensureTwoSpaceChanges++; } }
					item._state.lastBlankEmitted = false;
				}

				offset = end;
				const pct = Math.round((offset/size)*100);
				setProgress(item, pct);
				setStatus(item, `处理中 (${pct}%)`);
			}

			// flush remaining decoder buffer and leftover as final content
			const tail = decoder.decode();
			let finalStr = (item._state.leftover || '') + (tail || '');
			finalStr = finalStr.replace(/\r\n/g, '\n');
			if(finalStr.length > 0){
				const lines = finalStr.split('\n');
				for(let i=0;i<lines.length;i++){
					const line = lines[i];
					const isLast = (i === lines.length - 1);
					const isEmpty = line.trim().length === 0;
					const appendNewline = isLast ? fileEndsWithNewline : true;
					if(removeExtraBlank && isEmpty){
						if(item._state.lastBlankEmitted){ stats.removedBlankLines++; stats.totalModified++; continue; }
						if(appendNewline){ const enc = encoder.encode('\n'); parts.push(enc); stats.bytesWritten += enc.length; }
						item._state.lastBlankEmitted = true; continue;
					}
					if(isEmpty){ if(appendNewline){ const enc = encoder.encode('\n'); parts.push(enc); stats.bytesWritten += enc.length; } }
					else {
						let outLine = trimLeading ? line.replace(/^\s+/, '') : ('  ' + line.replace(/^\s*/, ''));
						if(appendNewline){ const enc = encoder.encode(outLine + '\n'); parts.push(enc); stats.bytesWritten += enc.length; } else { const enc = encoder.encode(outLine); parts.push(enc); stats.bytesWritten += enc.length; }
						if(outLine !== line){ stats.totalModified++; if(trimLeading){ if(/^\s+/.test(line) && !/^\s/.test(outLine)) stats.trimLeadingChanges++; } else { if(!/^ {2}/.test(line) && /^ {2}/.test(outLine)) stats.ensureTwoSpaceChanges++; } }
						item._state.lastBlankEmitted = false;
					}
				}
			}

			setProgress(item, 100); setStatus(item,'生成输出');
			// create blob and download (remove original file extension from name)
			const origName = item.file.name;
			const idx = origName.lastIndexOf('.');
			const baseName = (idx > 0) ? origName.slice(0, idx) : origName;
			// 使用源文件名作为输出文件名（不再添加 .out.txt）
			const outName = origName;
			// create blob from parts exactly as before (do not alter parts content)
			const blob = new Blob(parts, {type:'text/plain;charset=utf-8'});

			// 如果需要基于 out 文件生成索引，则先从 blob 中读取 bytes（UTF-8），再做行扫描与正则匹配
			if(doIndex){
				try{
					const arrBuf = await blob.arrayBuffer();
					const outputBytes = new Uint8Array(arrBuf);
					const decoderUtf8 = new TextDecoder('utf-8');
					const outStr = decoderUtf8.decode(outputBytes);

					// helper: compute UTF-8 byte length of substring up to a given
					// UTF-16 index (charIndex). Regex match.index is a UTF-16
					// code-unit index; to avoid splitting surrogate pairs or
					// miscounting code points, convert the UTF-16 index into a
					// code-point aware prefix and measure its UTF-8 byte length.
					const utf8PrefixBytes = (str, utf16Index)=>{
						if(!str || utf16Index <= 0) return 0;
						// If requested index beyond string, return full length
						if(utf16Index >= str.length) return encoder.encode(str).length;
						let utf16Acc = 0;
						const cpParts = [];
						// iterate over code points (Array.from is code-point aware)
						for(const cp of Array.from(str)){
							const unitLen = cp.length; // 1 for BMP, 2 for surrogate pair
							if(utf16Acc + unitLen > utf16Index) break;
							cpParts.push(cp);
							utf16Acc += unitLen;
						}
						const prefix = cpParts.join('');
						return encoder.encode(prefix).length;
					};

					// helper: normalize line for regex (replace special spaces, trim) while tracking original indices
					const buildNormInfo = (line)=>{
						if(!line) return { norm:'', map:[] };
						const chars = [];
						const map = [];
						for(let i=0;i<line.length;i++){
							let ch = line[i];
							if(ch === '\u200B') continue; // zero-width space
							if(ch === '\u3000' || ch === '\u00A0') ch = ' ';
							chars.push(ch);
							map.push(i);
						}
						let normStr = chars.join('');
						let start = 0;
						let end = normStr.length;
						const isWs = (c)=> c === ' ' || c === '\t' || c === '\n' || c === '\r' || c === '\f' || c === '\v';
						while(start < end && isWs(normStr[start])) start++;
						while(end > start && isWs(normStr[end-1])) end--;
						return { norm: normStr.slice(start, end), map: map.slice(start, end) };
					};

					// helper: run regex and obtain first match info without affecting original regex state
					const execFirstMatch = (reg, text)=>{
						if(!reg || !text) return null;
						let flags = reg.flags || '';
						if(!flags.includes('g')) flags += 'g';
						try{
							const local = new RegExp(reg.source, flags);
							local.lastIndex = 0;
							const m = local.exec(text);
							return m ? { match: m[0], index: m.index } : null;
						}catch(_){ return null; }
					};

					// compute line start byte offsets by scanning bytes for LF (10)
					const lineStarts = [];
					if(outputBytes.length === 0) lineStarts.push(0);
					else{
						lineStarts.push(0);
						for(let i=0;i<outputBytes.length;i++){
							if(outputBytes[i] === 10){ // LF
								if(i+1 < outputBytes.length) lineStarts.push(i+1);
							}
						}
					}
					const lines = outStr.split('\n');

					// compile regexes (priority 1->2->3)
					const regs = regexStrs.map((rs, idx)=>{
						if(!rs || rs.trim()==='') return null;
						let body = rs.trim();
						let flags = 'u';
						if(body.startsWith('/') && body.lastIndexOf('/')>0){
							const li = body.lastIndexOf('/');
							flags = (body.slice(li+1) || '') || 'u';
							body = body.slice(1, li);
						}
						try{ return { body, flags }; }catch(e){ log('无效正则样式'+(idx+1)+': '+e.message); return null; }
					});

					// Prepare per-line normalized info once (avoid recomputing for each regex)
					const lineInfos = lines.map((line)=>{
						const ni = buildNormInfo(line || '');
						return { text: line || '', norm: ni.norm || '', map: ni.map || [] };
					});

					// helper: run a single regex against all lines inside a Worker with timeout
					const REGEX_WORKER_TIMEOUT = 8000; // Thanks for @哆啰啰哆啰啰（小红书）的发现和建议，从400ms把timeout提升到8s，以解决较大文本的正则耗时。
					const runRegexInWorker = (regObj, lineInfos, timeoutMs)=>{
						return new Promise((resolve)=>{
							if(!regObj) return resolve([]);
							let resolved = false;
							let worker = null;
							try{
								worker = new Worker('regex_worker.js');
							}catch(e){ dlog('无法创建 Worker: '+(e&&e.message)); return resolve([]); }
							const to = setTimeout(()=>{
								if(worker){ try{ worker.terminate(); }catch(_e){} }
								if(!resolved){ resolved = true; dlog('正则匹配超时，已终止 Worker'); resolve([]); }
							}, timeoutMs || REGEX_WORKER_TIMEOUT);
							worker.onmessage = function(ev){
								if(resolved) return;
								clearTimeout(to);
								resolved = true;
								const d = ev.data || {};
								if(d.ok && Array.isArray(d.matches)){
									resolve(d.matches);
								} else {
									dlog('Worker 返回错误: '+(d && d.error));
									resolve([]);
								}
								try{ worker.terminate(); }catch(_e){}
							};
							worker.onerror = function(err){
								if(resolved) return;
								clearTimeout(to);
								resolved = true;
								dlog('Worker 错误: '+ (err && err.message));
								try{ worker.terminate(); }catch(_e){}
								resolve([]);
							};
							// post lines (only needed fields) and regex
							try{
								worker.postMessage({ regSource: regObj.body, regFlags: regObj.flags, lines: lineInfos });
							}catch(e){ clearTimeout(to); resolved = true; try{ worker.terminate(); }catch(_e){} resolve([]); }
						});
					};

					let chosenMatches = [];
					for(let i=0;i<regs.length;i++){
						const regObj = regs[i]; if(!regObj) continue;
						let rawMatches = [];
						try{ rawMatches = await runRegexInWorker(regObj, lineInfos, REGEX_WORKER_TIMEOUT); }catch(_e){ rawMatches = []; }
						const out = [];
						for(const m of rawMatches){
							const li = m.lineIndex;
							const line = (li < lines.length) ? (lines[li] || '') : '';
							const baseByte = (li < lineStarts.length) ? lineStarts[li] : 0;
							const withinLineBytes = utf8PrefixBytes(line, m.index || 0);
							const absoluteByte = baseByte + withinLineBytes;
							out.push({ index: out.length+1, title: (m.match || line).replace(/\r?\n/g,'').trim(), startByte: absoluteByte, percent: (absoluteByte/outputBytes.length)*100 });
						}
						if(out.length>0){ chosenMatches = out; log('目录: 使用样式'+(i+1)+' 在 out 文件上命中 '+out.length+' 行'); break; }
					}
					if(chosenMatches.length === 0) log('目录: 在 out 文件上未命中任何样式');

					// write .idx if matches found
					log(`调试: doIndex=${doIndex}; chosenMatches=${chosenMatches.length}; opt_idx_diag_present=${!!optIdxDiagEl}; opt_idx_diag_checked=${optIdxDiagEl?optIdxDiagEl.checked:false}`);
					if(chosenMatches.length>0){
						// Diagnostic: verify that reported startByte matches actual byte position of the title in outStr
						if(optIdxDiagEl && optIdxDiagEl.checked){
							try{
								// Byte-level search to avoid JS string/UTF-16 vs UTF-8 subtle differences.
								const outBytes = outputBytes; // Uint8Array
								const bytesIndexOf = (hay, needle)=>{
									if(!hay || !needle || needle.length === 0) return -1;
									for(let i=0;i<=hay.length-needle.length;i++){
										let ok = true;
										for(let j=0;j<needle.length;j++){
											if(hay[i+j] !== needle[j]) { ok = false; break; }
										}
										if(ok) return i;
									}
									return -1;
								};
								for(let i=0;i<chosenMatches.length;i++){
									const it = chosenMatches[i];
									const title = (it.title || '').replace(/\r?\n/g,'').trim();
									const reported = it.startByte;
									const titleBytes = encoder.encode(title);
									const foundBytePos = bytesIndexOf(outBytes, titleBytes);
									if(foundBytePos >= 0){
										if(foundBytePos !== reported){
											log(`目录校验警告：文件 ${item.file.name} 第 ${i+1} 条标题位置不匹配：标题='${truncateStr(title,40)}'；idx 报告字节=${reported}，基于字节查得=${foundBytePos}`);
										}
									} else {
										log(`目录校验提示：文件 ${item.file.name} 第 ${i+1} 条标题未在输出字节中找到：标题='${truncateStr(title,40)}'；报告字节=${reported}`);
									}
								}
							}catch(e){ log('目录校验出错: '+(e && e.message)); }
						}

						const idxLines = [];
						for(let i=0;i<chosenMatches.length;i++){
							const it = chosenMatches[i];
							const seq = i+1;
							const title = (it.title || '').replace(/\r?\n/g,'').trim();
							const bytePos = it.startByte;
							const pct = (typeof it.percent === 'number') ? it.percent.toFixed(2) : ((bytePos / outputBytes.length)*100).toFixed(2);
							idxLines.push(`#${seq}#, #${title}#, #${bytePos}#, #${pct}#,`);
						}
						const idxBlob = new Blob([idxLines.join('\n')], {type:'text/plain;charset=utf-8'});
						downloadBlob(idxBlob, baseName + '.idx');
						stats.indexEntries = chosenMatches.length;
						log(`文件 ${item.file.name} 目录索引: 共 ${chosenMatches.length} 条，已生成 ${baseName}.idx`);
					} else {
						stats.indexEntries = 0;
					}
				}catch(e){ log('基于 out 文件生成目录时出错: '+(e && e.message)); stats.indexEntries = 0; }
			} else {
				stats.indexEntries = 0;
			}
			// finalize stats
			stats.timeMs = Date.now() - stats.startTime;
			if(stats.bytesWritten === 0){ for(const p of parts) if(p && p.length) stats.bytesWritten += p.length; }
			// log stats summary
			log(`文件 ${item.file.name} 统计: 编码=${stats.encoding}; 大小=${fmtSize(item.file.size)}; 采样=${sampleSize} bytes; 用时 ${Math.round(stats.timeMs)} ms`);
			log(`  总改动行数: ${stats.totalModified}; 去除多余空行: ${stats.removedBlankLines}; 行首顶格改动: ${stats.trimLeadingChanges}; 添加2空格改动: ${stats.ensureTwoSpaceChanges}; 输出字节: ${stats.bytesWritten}`);
			downloadBlob(blob, outName);
			setStatus(item,'已完成'); item.parts = null; log('完成: '+item.file.name+' -> '+outName);
			return stats;
		}catch(e){
			if(e && e.message === '已取消'){ setStatus(item,'已取消'); return stats; }
			else { setStatus(item,'错误'); throw e; }
		}
	}

	function detectEncoding(buffer){
		const bytes = new Uint8Array(buffer);
		
		// 1. 检测 BOM (Byte Order Mark) - 最可靠的方式
		if(bytes.length >= 3 && bytes[0] === 0xEF && bytes[1] === 0xBB && bytes[2] === 0xBF){
			return 'utf-8'; // UTF-8 BOM
		}
		if(bytes.length >= 2){
			if(bytes[0] === 0xFF && bytes[1] === 0xFE) return 'utf-16le'; // UTF-16 LE BOM
			if(bytes[0] === 0xFE && bytes[1] === 0xFF) return 'utf-16be'; // UTF-16 BE BOM
		}
		
		// 2. 启发式检测：统计字符特征
		const detectWithHeuristics = (encoding) => {
			try{
				const dec = new TextDecoder(encoding, {fatal: false}); // 不使用 fatal，允许替换字符
				const text = dec.decode(buffer);
				
				// 计算可疑字符的比例
				let replacementChars = 0;
				let controlChars = 0;
				let validChineseChars = 0;
				let asciiChars = 0;
				
				for(let i = 0; i < text.length; i++){
					const code = text.charCodeAt(i);
					if(text[i] === '\uFFFD') replacementChars++; // 替换字符（解码失败）
					else if(code < 0x20 && code !== 0x09 && code !== 0x0A && code !== 0x0D) controlChars++; // 异常控制字符
					else if(code >= 0x4E00 && code <= 0x9FFF) validChineseChars++; // 常用汉字区
					else if(code < 0x80) asciiChars++; // ASCII
				}
				
				const totalChars = text.length;
				if(totalChars === 0) return {encoding, score: -1, text: ''};
				
				// 计算得分（越高越可能是正确编码）
				const replacementRate = replacementChars / totalChars;
				const controlRate = controlChars / totalChars;
				const chineseRate = validChineseChars / totalChars;
				
				// 如果有大量替换字符或控制字符，说明编码不对
				if(replacementRate > 0.01 || controlRate > 0.01) return {encoding, score: -1, text};
				
				// 对于中文文本，GB18030/GBK 应该有较高的汉字比例
				// 对于 UTF-8，汉字也应该正常
				let score = 100;
				score -= replacementRate * 1000; // 严重惩罚替换字符
				score -= controlRate * 500; // 惩罚异常控制字符
				score += chineseRate * 10; // 奖励汉字
				
				// 针对不同编码的特殊处理
				if(encoding === 'gb18030' || encoding === 'gbk'){
					// GBK/GB18030 对中文文本应该有很高的汉字比例
					if(chineseRate > 0.1) score += 20; // 如果有 >10% 汉字，优先考虑 GBK
				}
				if(encoding === 'utf-8'){
					// UTF-8 编码的文本应该没有替换字符
					if(replacementRate === 0 && controlRate === 0) score += 15;
				}
				
				return {encoding, score, text, replacementRate, controlRate, chineseRate};
			}catch(e){
				return {encoding, score: -1, text: ''};
			}
		};
		
		// 3. 尝试所有编码并评分
		const results = [];
		for(const enc of tryEncodings){
			const result = detectWithHeuristics(enc);
			if(result.score > 0){
				results.push(result);
			}
		}
		
		// 4. 选择得分最高的编码
		if(results.length > 0){
			results.sort((a, b) => b.score - a.score);
			const best = results[0];
			// 如果最佳结果的得分远高于其他结果，使用它
			if(best.score > 80 || (results.length > 1 && best.score > results[1].score + 20)){
				return best.encoding;
			}
		}
		
		// 5. 如果启发式检测失败，回退到简单的 fatal 检测（兼容旧逻辑）
		for(const enc of tryEncodings){
			try{
				const dec = new TextDecoder(enc, {fatal:true});
				try{ 
					dec.decode(buffer); 
					return enc; 
				}catch(_){ }
			}catch(_){ }
		}
		
		// 6. 最后回退到 UTF-8（默认）
		return 'utf-8';
	}

	function createDecoder(enc){
		try{ 
			// 使用 fatal:false 以避免在遇到个别无效字节时完全失败
			// 这样可以继续处理大部分正确的内容
			return new TextDecoder(enc, {fatal: false}); 
		}catch(_){ 
			return new TextDecoder('utf-8', {fatal: false}); 
		}
	}

	function downloadBlob(blob, filename){
		// Use simple download behavior: set download attribute to filename and click the link.
		// Browsers will save to the user's default download directory. We do not attempt
		// to force subdirectories here because browser behavior is inconsistent.
		const url = URL.createObjectURL(blob);
		const a = document.createElement('a');
		a.href = url;
		a.download = filename;
		document.body.appendChild(a);
		a.click();
		a.remove();
		setTimeout(()=>URL.revokeObjectURL(url),5000);
	}

	log('文本处理（分块，多文件，拖拽） 已加载');
});
