// main.js — 实现浏览器端的简化 .bin 字体生成器（参考 tools/generate_1bit_font_bin.py 的输出格式）
document.addEventListener('DOMContentLoaded', () => {
  // global debug flag: set to true in the console (window.__FONTGEN_DEBUG = true)
  // to enable verbose diagnostic prints and generation of the .stats.json file.
  window.__FONTGEN_DEBUG = window.__FONTGEN_DEBUG || false;
  const fontFileInput = document.getElementById('fontFile');
  const btnBrowseFont = document.getElementById('btnBrowseFont');
  const fontPathText = document.getElementById('fontPath');
  const btnBrowseOut = document.getElementById('btnBrowseOut');
  const outputPath = document.getElementById('outputPath');
  const sizeInput = document.getElementById('size');
  const whiteInput = document.getElementById('white');
  const whiteValueDisplay = document.getElementById('whiteValue');
  const whiteDecBtn = document.getElementById('whiteDec');
  const whiteIncBtn = document.getElementById('whiteInc');
  const demoEnable = document.getElementById('demo_enable');
  const demoText = document.getElementById('demo_text');
  const demoOut = document.getElementById('demo_out');
  const chunkSizeInput = document.getElementById('chunkSize');
  const workerCountInput = document.getElementById('workerCount');
  const optGbk = document.getElementById('opt_gbk');
  const optTraditional = document.getElementById('opt_traditional');
  const optJapanese = document.getElementById('opt_japanese');
  const useFullCharset = document.getElementById('use_full_charset');
  const useCommonCharset = document.getElementById('use_common_charset');
  const useCustomCharset = document.getElementById('use_custom_charset');
  const customUnicodeInput = document.getElementById('custom_unicode');

  // Ensure radios behave as mutually exclusive and toggle custom input enable state
  try {
    function updateCharsetInputState() {
      if (customUnicodeInput) customUnicodeInput.disabled = !(useCustomCharset && useCustomCharset.checked);
    }
    [useFullCharset, useCommonCharset, useCustomCharset].forEach(el => {
      if (el) el.addEventListener('change', () => {
        // when selecting full, we may want to disable other controls visually
        updateCharsetInputState();
        try { const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) { }
      });
    });
    // initialize
    updateCharsetInputState();
  } catch (e) { }
  const nameValue = document.getElementById('nameValue');
  const styleValue = document.getElementById('styleValue');
  const statusValue = document.getElementById('statusValue');
  const generateBtn = document.getElementById('generateBtn');
  const cancelBtn = document.getElementById('cancelBtn');
  const logEl = document.getElementById('log');
  const progressBar = document.getElementById('progressBar');
  const firmwareSelect = document.getElementById('firmwareSelect');
  const edcThresholdControls = document.getElementById('edcThresholdControls');
  const edcWhiteInput = document.getElementById('edcWhite');
  const edcWhiteValue = document.getElementById('edcWhiteValue');
  const edcBlackInput = document.getElementById('edcBlack');
  const edcBlackValue = document.getElementById('edcBlackValue');
  const v3ThresholdControls = document.getElementById('v3ThresholdControls');
  const v3WhiteInput = document.getElementById('v3White');
  const v3WhiteValue = document.getElementById('v3WhiteValue');
  const v3GrayInput = document.getElementById('v3Gray');
  const v3GrayValue = document.getElementById('v3GrayValue');
  const sizeLabel = document.querySelector('label[for="size"]');
  const exportProgmemCheckbox = document.getElementById('export_progmem');
  const exportOptionsSection = document.getElementById('exportOptions');
  const exportOptionsTitle = document.getElementById('exportOptionsTitle');

  // 导出选项显示/隐藏切换函数
  window.showmethemoney = function(show) {
    if (exportOptionsSection) {
      exportOptionsSection.style.display = show ? 'block' : 'none';
    }
    if (exportOptionsTitle) {
      exportOptionsTitle.style.display = show ? 'block' : 'none';
    }
  };

  // Smoothing UI (created dynamically if not present in HTML)
  let smoothingEnable = null;
  let smoothingPassesInput = null;
  let enableGaussian = null;
  let gaussianRadiusInput = null;
  let gaussianSigmaInput = null;
  let enableBilateral = null;
  let bilateralRadiusInput = null;
  let bilateralSigmaSpaceInput = null;
  let bilateralSigmaRangeInput = null;
  (function ensureSmoothingControls() {
    // try existing elements first
    smoothingEnable = document.getElementById('smoothing_enable');
    smoothingPassesInput = document.getElementById('smoothing_passes');
    enableGaussian = document.getElementById('enable_gaussian');
    gaussianRadiusInput = document.getElementById('gaussian_radius');
    gaussianSigmaInput = document.getElementById('gaussian_sigma');
    enableBilateral = document.getElementById('enable_bilateral');
    bilateralRadiusInput = document.getElementById('bilateral_radius');
    bilateralSigmaSpaceInput = document.getElementById('bilateral_sigma_space');
    bilateralSigmaRangeInput = document.getElementById('bilateral_sigma_range');
    if (smoothingEnable && smoothingPassesInput) return;
    // create container near edcThresholdControls if possible, else append to top of body
    const container = document.createElement('div');
    container.id = 'smoothingControls';
    container.className = 'control-group smoothing-controls';
    container.style.margin = '8px 0';
    container.innerHTML = `
      <span id="smoothingBox">
        <label style="margin-right:12px"><input type="checkbox" id="smoothing_enable" /> 启用平滑</label>
        <label>平滑次数: <input type="number" id="smoothing_passes" min="0" max="4" value="1" style="width:3.5em; margin-left:6px"/></label>
      </span>
      <!div style="margin-top:8px;">
        <strong style="display:none;font-size:0.95em">预处理滤波（可选）</strong>
        <div style="display:flex; gap:12px; align-items:center; margin-top:6px;">
          <!-- 高斯模糊控件已隐藏/移除，默认不启用 -->
          <label style="margin-left:6px;display:; align-items:center; gap:6px;"><input type="checkbox" id="enable_bilateral" /> 平滑去噪</label>
          <label style="display:none;white-space:nowrap;">半径: <input id="bilateral_radius" type="number" min="0" max="6" value="3" style="width:3.2em; margin-left:6px"/></label>
          <label style="display:none;white-space:nowrap;">σ_space: <input id="bilateral_sigma_space" type="number" min="0" step="0.1" value="3.0" style="display:none;width:4em; margin-left:6px"/></label>
          <label style="display:none;white-space:nowrap;">σ_range: <input id="bilateral_sigma_range" type="number" min="1" step="1" value="40" style="width:4em; margin-left:6px"/></label>
        </div>
        <div style="margin-top:10px; display:none; gap:8px; align-items:center;">
          <button type="button" id="preset_quality" style="padding:6px 8px;">预设：高质量</button>
          <button type="button" id="preset_aggressive" style="padding:6px 8px;">预设：强力去噪</button>
        </div>
      </div>
      <div style="margin-left:6px; margin-top:12px;">
        <div style="display:flex; align-items:center; gap:12px;">
          <label><input type="checkbox" id="use_otsu" /> 自动阈值 <span id="otsu_bias_value" style="font-size:0.85em; ">25</span></label>
        </div>
        <div style="margin-top:8px; display:flex; align-items:center; gap:8px;">
          <div style="flex:1; display:flex; flex-direction:column; align-items:center;">
            
            <input type="range" id="otsu_bias" min="-80" max="120" step="1" value="60" style="width:100%; max-width:100%;"/>
          </div>
          <label id="stroke_expand_label" style="white-space:nowrap; display:none; margin-left:6px">笔画加粗: <input type="number" id="stroke_expand" min="0" max="3" value="0" style="width:3.5em; margin-left:6px"/></label>
        </div>
      </div>
    `;
    try {
      if (edcThresholdControls && edcThresholdControls.parentNode) edcThresholdControls.parentNode.insertBefore(container, edcThresholdControls.nextSibling);
      else document.body.insertBefore(container, document.body.firstChild);
    } catch (e) { document.body.insertBefore(container, document.body.firstChild); }
    smoothingEnable = document.getElementById('smoothing_enable');
    smoothingPassesInput = document.getElementById('smoothing_passes');
    // After inserting the HTML, re-fetch other controls so we can bind listeners
    enableGaussian = document.getElementById('enable_gaussian');
    gaussianRadiusInput = document.getElementById('gaussian_radius');
    gaussianSigmaInput = document.getElementById('gaussian_sigma');
    enableBilateral = document.getElementById('enable_bilateral');
    bilateralRadiusInput = document.getElementById('bilateral_radius');
    bilateralSigmaSpaceInput = document.getElementById('bilateral_sigma_space');
    bilateralSigmaRangeInput = document.getElementById('bilateral_sigma_range');
    // Ensure bilateral filter defaults to enabled with high-quality parameters
    try {
      // Best-quality defaults (reasonable balance between quality and performance)
      if (enableBilateral) enableBilateral.checked = true;
      if (bilateralRadiusInput) bilateralRadiusInput.value = '3';
      if (bilateralSigmaSpaceInput) bilateralSigmaSpaceInput.value = '3.0';
      if (bilateralSigmaRangeInput) bilateralSigmaRangeInput.value = '40';
      try { appendLog('默认: 双边滤波已启用 (radius=3, σ_space=3.0, σ_range=40)'); } catch (e) { }
    } catch (e) { /* best-effort defaults; ignore errors */ }
  })();
  // Wire smoothing controls to preview updates
  try {
    if (smoothingEnable) smoothingEnable.addEventListener('change', () => {
      try { const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) { }
    });
    // wire gaussian/bilateral controls -> preview
    if (enableGaussian) enableGaussian.addEventListener('change', () => { try { const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) {} });
    if (gaussianRadiusInput) gaussianRadiusInput.addEventListener('input', () => { try { const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) {} });
    if (gaussianSigmaInput) gaussianSigmaInput.addEventListener('input', () => { try { const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) {} });
    if (enableBilateral) enableBilateral.addEventListener('change', () => { try { const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) {} });
    if (bilateralRadiusInput) bilateralRadiusInput.addEventListener('input', () => { try { const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) {} });
    if (bilateralSigmaSpaceInput) bilateralSigmaSpaceInput.addEventListener('input', () => { try { const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) {} });
    if (bilateralSigmaRangeInput) bilateralSigmaRangeInput.addEventListener('input', () => { try { const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) {} });
    // V3 threshold controls -> preview updates
    if (v3WhiteInput) v3WhiteInput.addEventListener('input', () => { try { setV3ThresholdDisplays(); const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) {} });
    if (v3GrayInput) v3GrayInput.addEventListener('input', () => { try { setV3ThresholdDisplays(); const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) {} });
    // Otsu control: re-render preview when toggled and disable threshold slider
    let useOtsuEl = document.getElementById('use_otsu');
    // otsu_denoise control removed per UI simplification
    let otsuBiasEl = document.getElementById('otsu_bias');
    let strokeExpandEl = document.getElementById('stroke_expand');
    let otsuBiasValueEl = document.getElementById('otsu_bias_value');
    let otsuBiasDecBtn = document.getElementById('otsu_bias_dec');
    let otsuBiasIncBtn = document.getElementById('otsu_bias_inc');
    if (useOtsuEl) {
      const onUseOtsuChange = () => {
        try {
          const isEdcLocal = (currentFirmwareMode === FIRMWARE_MODES.EDC);
          const disabled = (useOtsuEl.checked) || isEdcLocal || configLocked;
          if (whiteInput) whiteInput.disabled = disabled;
          if (whiteValueDisplay) whiteValueDisplay.classList.toggle('muted', disabled);
          if (otsuBiasEl) otsuBiasEl.disabled = !useOtsuEl.checked || isEdcLocal || configLocked;
          if (otsuBiasDecBtn) otsuBiasDecBtn.disabled = !useOtsuEl.checked || isEdcLocal || configLocked;
          if (otsuBiasIncBtn) otsuBiasIncBtn.disabled = !useOtsuEl.checked || isEdcLocal || configLocked;
          if (strokeExpandEl) strokeExpandEl.disabled = !useOtsuEl.checked;
          if (otsuBiasValueEl) otsuBiasValueEl.classList.toggle('muted', !useOtsuEl.checked || isEdcLocal || configLocked);
        } catch (e) { }
        try { const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) { }
      };
      useOtsuEl.addEventListener('change', onUseOtsuChange);

      // otsu_denoise removed: no per-otsu denoise checkbox to bind

      // helper to clamp & apply bias
      function clampOtsuBias(v) {
        const min = parseInt(otsuBiasEl?.min || '-80', 10);
        const max = parseInt(otsuBiasEl?.max || '80', 10);
        return Math.max(min, Math.min(max, v));
      }
      function applyOtsuBias(v, triggerPreview = true) {
        if (!otsuBiasEl) return;
        const newVal = clampOtsuBias(Number(v));
        otsuBiasEl.value = String(newVal);
        if (otsuBiasValueEl) otsuBiasValueEl.textContent = String(newVal);
        if (triggerPreview && currentFirmwareMode === FIRMWARE_MODES.READPAPER) {
          try { const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) { }
        }
      }

      // wire range input -> display + preview
      if (otsuBiasEl) {
        // initialize display
        if (otsuBiasValueEl) otsuBiasValueEl.textContent = String(otsuBiasEl.value);
        otsuBiasEl.addEventListener('input', (ev) => {
          try {
            if (otsuBiasValueEl) otsuBiasValueEl.textContent = String(ev.target.value);
            const f = getCurrentFontFile(); if (f) schedulePreview(f);
          } catch (e) { }
        });
      }

      // fine-adjust +/- buttons with long-press support (similar to white controls)
      let __otsuRepeatTimer = null;
      function clearOtsuRepeat() { if (__otsuRepeatTimer) { clearInterval(__otsuRepeatTimer); __otsuRepeatTimer = null; } }
      function startOtsuRepeat(delta, ev) {
        applyOtsuBias((parseInt(otsuBiasEl.value, 10) || 0) + delta);
        clearOtsuRepeat();
        __otsuRepeatTimer = setInterval(() => {
          applyOtsuBias((parseInt(otsuBiasEl.value, 10) || 0) + delta);
        }, 120);
      }

      if (otsuBiasDecBtn) {
        otsuBiasDecBtn.addEventListener('click', (ev) => {
          const step = ev.shiftKey ? 5 : 1;
          applyOtsuBias((parseInt(otsuBiasEl.value, 10) || 0) - step);
        });
        otsuBiasDecBtn.addEventListener('mousedown', (ev) => { ev.preventDefault(); const step = ev.shiftKey ? 5 : 1; startOtsuRepeat(-step, ev); });
        ['mouseup', 'mouseleave', 'blur'].forEach(ev => otsuBiasDecBtn.addEventListener(ev, clearOtsuRepeat));
      }

      if (otsuBiasIncBtn) {
        otsuBiasIncBtn.addEventListener('click', (ev) => { const step = ev.shiftKey ? 5 : 1; applyOtsuBias((parseInt(otsuBiasEl.value, 10) || 0) + step); });
        otsuBiasIncBtn.addEventListener('mousedown', (ev) => { ev.preventDefault(); const step = ev.shiftKey ? 5 : 1; startOtsuRepeat(step, ev); });
        ['mouseup', 'mouseleave', 'blur'].forEach(ev => otsuBiasIncBtn.addEventListener(ev, clearOtsuRepeat));
      }

      if (strokeExpandEl) strokeExpandEl.addEventListener('input', () => { try { const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) { } });
      // initialize
      try { onUseOtsuChange(); } catch (e) { }

        // 默认启用 Otsu 并设置偏移为 60（控件保持可见）
      try {
        // apply bias without forcing preview for the initial state
        applyOtsuBias(60, false);
        useOtsuEl.checked = true;
        onUseOtsuChange();

        // 隐藏颜色深度滑块及其按钮/显示
        try { if (whiteInput) whiteInput.style.display = 'none'; } catch (e) {}
        try { if (whiteValueDisplay) whiteValueDisplay.style.display = 'none'; } catch (e) {}
        try { if (whiteDecBtn) whiteDecBtn.style.display = 'none'; } catch (e) {}
        try { if (whiteIncBtn) whiteIncBtn.style.display = 'none'; } catch (e) {}

        // 保持 Otsu 相关 UI 可见（显示/隐藏行为由 onUseOtsuChange 控制）
        try { if (useOtsuEl) useOtsuEl.style.display = ''; } catch (e) {}
        try { if (otsuBiasEl) otsuBiasEl.style.display = ''; } catch (e) {}
        try { const e1 = document.getElementById('otsu_bias_value'); if (e1) e1.style.display = ''; } catch (e) {}
        try { const e2 = document.getElementById('otsu_bias_value_top'); if (e2) e2.style.display = ''; } catch (e) {}
        try { if (otsuBiasDecBtn) otsuBiasDecBtn.style.display = ''; } catch (e) {}
        try { if (otsuBiasIncBtn) otsuBiasIncBtn.style.display = ''; } catch (e) {}
        // otsu_denoise UI removed; nothing to show/hide here
      } catch (e) {}
    }
    if (smoothingPassesInput) smoothingPassesInput.addEventListener('input', (ev) => {
      try {
        let v = parseInt(smoothingPassesInput.value, 10) || 0;
        if (v < 0) v = 0; if (v > 4) v = 4;
        smoothingPassesInput.value = String(v);
        const f = getCurrentFontFile(); if (f) schedulePreview(f);
      } catch (e) { }
    });
  } catch (e) { /* ignore */ }

  const FIRMWARE_MODES = {
    READPAPER: 'readpaper',
    READPAPER_V3: 'readpaper_v3',
    EDC: 'edc'
  };

  let currentFirmwareMode = FIRMWARE_MODES.READPAPER;
  let configLocked = false;

  // Toast/card notification helper (uses chota-like card markup)
  function ensureToastContainer() {
    let c = document.getElementById('toastContainer');
    if (!c) {
      c = document.createElement('div');
      c.id = 'toastContainer';
      c.className = 'toast-container';
      // Add inline styles so toasts are visible even if CSS wasn't compiled/loaded
      c.style.position = 'fixed';
      c.style.right = '1rem';
      c.style.bottom = '1rem';
      c.style.display = 'flex';
      c.style.flexDirection = 'column';
      c.style.gap = '0.5rem';
      c.style.zIndex = '9999';
      c.style.maxWidth = '320px';
      document.body.appendChild(c);
    }
    return c;
  }

  function showCard(message, options = {}) {
    // options: { type: 'info'|'success'|'error', ttl: ms }
    const ttl = typeof options.ttl === 'number' ? options.ttl : 4000;
    const type = options.type || 'info';
    const container = ensureToastContainer();
    const card = document.createElement('div');
    card.className = 'card toast-card';
    // fallback inline styles for visibility when CSS not applied
    card.style.boxShadow = '0 6px 18px rgba(0,0,0,0.08)';
    card.style.borderRadius = '6px';
    card.style.overflow = 'hidden';
    card.style.background = '#fff';
    card.setAttribute('role', 'status');
    const body = document.createElement('div');
    body.className = 'card-body';
    body.style.padding = '0.6rem 0.8rem';
    body.style.fontSize = '14px';
    // basic semantic coloring
    if (type === 'success') body.style.borderLeft = '4px solid #38b000';
    else if (type === 'error') body.style.borderLeft = '4px solid #d00000';
    else body.style.borderLeft = '4px solid #0ea5e9';
    body.style.padding = '0.6rem 0.8rem';
    body.style.fontSize = '14px';
    body.textContent = message;
    card.appendChild(body);
    // append and auto-remove
    container.appendChild(card);
    // remove after ttl
    const timer = setTimeout(() => {
      try { card.remove(); } catch (e) { }
    }, ttl);
    // clickable to dismiss early
    card.addEventListener('click', () => { clearTimeout(timer); try { card.remove(); } catch (e) { } });
    return card;
  }

  let worker = { cancelled: false, running: false };

  function appendLog(text = '') {
    const t = new Date().toLocaleTimeString();
    const line = `[${t}] ${text}\n`;
    try {
      if (logEl instanceof HTMLTextAreaElement || 'value' in logEl) {
        logEl.value += line;
      } else {
        // pre or div-like element
        logEl.textContent = (logEl.textContent || '') + line;
      }
      // attempt to keep scrolled to bottom
      logEl.scrollTop = logEl.scrollHeight;
    } catch (e) {
      // best-effort: ignore
      console.error('appendLog error', e);
    }
  }

  function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

  function getCurrentFontFile() {
    return (fontFileInput && fontFileInput.files && fontFileInput.files[0]) ? fontFileInput.files[0] : null;
  }

  function getSizeRange(mode = currentFirmwareMode) {
    // Unified size range for ReadPaper and EDC Book
    return { min: 24, max: 48 };
  }

  function clampSizeByMode(value, mode = currentFirmwareMode) {
    const { min, max } = getSizeRange(mode);
    return Math.max(min, Math.min(max, value));
  }

  function getColorDepthRange() {
    const min = parseInt(whiteInput?.min || '10', 10);
    const max = parseInt(whiteInput?.max || '240', 10);
    return { min, max };
  }

  function getColorDepthValue() {
    return parseInt(whiteInput?.value || '160', 10);
  }

  function setColorDepthValue(val) {
    if (!whiteInput) return;
    const { min, max } = getColorDepthRange();
    const clamped = Math.max(min, Math.min(max, val));
    whiteInput.value = clamped;
    if (whiteValueDisplay) whiteValueDisplay.textContent = String(clamped);
  }

  function getEdcThresholds() {
    const whiteVal = parseInt(edcWhiteInput?.value || '32', 10);
    const blackVal = parseInt(edcBlackInput?.value || '223', 10);
    return { white: whiteVal, black: blackVal };
  }

  function getV3Thresholds() {
    const whiteVal = parseInt(v3WhiteInput?.value || '200', 10);
    const grayVal = parseInt(v3GrayInput?.value || '120', 10);
    return { white: whiteVal, gray: grayVal };
  }

  function setV3ThresholdDisplays() {
    if (v3WhiteValue && v3WhiteInput) v3WhiteValue.textContent = String(v3WhiteInput.value);
    if (v3GrayValue && v3GrayInput) v3GrayValue.textContent = String(v3GrayInput.value);
  }

  function setEdcThresholdDisplays() {
    if (edcWhiteValue && edcWhiteInput) edcWhiteValue.textContent = String(edcWhiteInput.value);
    if (edcBlackValue && edcBlackInput) edcBlackValue.textContent = String(edcBlackInput.value);
  }

  function applyFirmwareUiState() {
    const isEdc = currentFirmwareMode === FIRMWARE_MODES.EDC;
    const isV3 = currentFirmwareMode === FIRMWARE_MODES.READPAPER_V3;
    const { min, max } = getSizeRange();

    if (sizeInput) {
      sizeInput.min = String(min);
      sizeInput.max = String(max);
      const raw = parseInt(sizeInput.value || String(min), 10);
      const clamped = clampSizeByMode(isNaN(raw) ? min : raw);
      if (clamped !== raw) sizeInput.value = clamped;
    }
    if (sizeLabel) {
      sizeLabel.innerHTML = `大小 (${min}-${max})`;
    }

    const useOtsuEl_local = document.getElementById('use_otsu');
    const disableColorDepth = configLocked || isEdc || (useOtsuEl_local && useOtsuEl_local.checked);
    if (whiteInput) whiteInput.disabled = disableColorDepth;
    if (whiteDecBtn) whiteDecBtn.disabled = disableColorDepth;
    if (whiteIncBtn) whiteIncBtn.disabled = disableColorDepth;
    if (whiteValueDisplay) {
      whiteValueDisplay.classList.toggle('muted', disableColorDepth);
    }
    const whiteControl = document.querySelector('.white-control');
    if (whiteControl) {
      whiteControl.classList.toggle('is-disabled', disableColorDepth);
    }
    // Ensure otsu bias slider is disabled unless Otsu is enabled and not in EDC and not locked
    try {
      const otsuBiasEl_local = document.getElementById('otsu_bias');
      if (otsuBiasEl_local) otsuBiasEl_local.disabled = (!useOtsuEl_local || !useOtsuEl_local.checked) || isEdc || configLocked;
    } catch (e) { /* ignore */ }

    // Show/hide V3-specific threshold controls and hide unrelated controls when V3 selected
    try {
      if (v3ThresholdControls) v3ThresholdControls.classList.toggle('is-hidden', !isV3);
      // If in V3 mode, hide controls that don't apply
      const smoothingContainer = document.getElementById('smoothingControls');
      if (smoothingContainer) smoothingContainer.style.display = isV3 ? 'none' : '';
      if (useOtsuEl_local) useOtsuEl_local.parentNode && (useOtsuEl_local.parentNode.style.display = isV3 ? 'none' : '');
      const strokeExpandLabel = document.getElementById('stroke_expand_label');
      if (strokeExpandLabel) {
        // Hide "笔画加粗" for ReadPaper (V2). Also keep it hidden for V3.
        if (currentFirmwareMode === FIRMWARE_MODES.READPAPER) {
          strokeExpandLabel.style.display = 'none';
        } else {
          strokeExpandLabel.style.display = isV3 ? 'none' : '';
        }
      }
      // Hide white-control slider as V3 uses its own thresholds
      const whiteControlElem = document.querySelector('.white-control');
      if (whiteControlElem) whiteControlElem.style.display = isV3 ? 'none' : '';
      // update V3 displays
      setV3ThresholdDisplays();
    } catch (e) { /* ignore UI toggling errors */ }

  // Bind preset buttons (if present) to set parameters and trigger preview
  try {
    const presetQuality = document.getElementById('preset_quality');
    const presetAgg = document.getElementById('preset_aggressive');
    function applyPresetAndPreview(setter) {
      try { setter(); const f = getCurrentFontFile(); if (f) schedulePreview(f); } catch (e) { }
    }
    // '快速' 预设已移除 per user request

    if (presetQuality) presetQuality.addEventListener('click', () => applyPresetAndPreview(() => {
      // Ensure Gaussian and Otsu-post-denoise remain disabled by default
      if (enableGaussian) enableGaussian.checked = false;
      if (gaussianRadiusInput) gaussianRadiusInput.value = '0';
      if (gaussianSigmaInput) gaussianSigmaInput.value = '1.0';
      if (enableBilateral) enableBilateral.checked = true;
      if (bilateralRadiusInput) bilateralRadiusInput.value = '2';
      if (bilateralSigmaSpaceInput) bilateralSigmaSpaceInput.value = '2.0';
      if (bilateralSigmaRangeInput) bilateralSigmaRangeInput.value = '20';
      // otsu_denoise removed: no action needed
      if (smoothingPassesInput) smoothingPassesInput.value = '1';
    }));

    if (presetAgg) presetAgg.addEventListener('click', () => applyPresetAndPreview(() => {
      // Aggressive preset: still do not enable Gaussian or Otsu-post-denoise by default
      if (enableGaussian) enableGaussian.checked = false;
      if (gaussianRadiusInput) gaussianRadiusInput.value = '2';
      if (gaussianSigmaInput) gaussianSigmaInput.value = '1.5';
      if (enableBilateral) enableBilateral.checked = true;
      if (bilateralRadiusInput) bilateralRadiusInput.value = '2';
      if (bilateralSigmaSpaceInput) bilateralSigmaSpaceInput.value = '2.0';
      if (bilateralSigmaRangeInput) bilateralSigmaRangeInput.value = '30';
      // otsu_denoise removed: no action needed
      if (smoothingPassesInput) smoothingPassesInput.value = '1';
    }));
  } catch (e) { /* ignore preset binding errors */ }
    // ensure white input visual state reflects disabled due to Otsu
    try {
      if (whiteInput) whiteInput.disabled = disableColorDepth;
      if (whiteValueDisplay) whiteValueDisplay.classList.toggle('muted', disableColorDepth);
    } catch (e) { }
    // Show/hide smoothing controls for ReadPaper mode
    try {
      const smoothingBox = document.getElementById('smoothingBox');
      if (smoothingBox) {
        if (currentFirmwareMode === FIRMWARE_MODES.READPAPER) {
          smoothingBox.style.display = 'none';
          if (smoothingEnable) smoothingEnable.checked = false;
          if (smoothingPassesInput) smoothingPassesInput.value = '0';
        } else {
          smoothingBox.style.display = '';
        }
      }
    } catch (e) { /* ignore */ }

    if (firmwareSelect) firmwareSelect.disabled = configLocked;

    if (edcThresholdControls) {
      edcThresholdControls.classList.toggle('is-hidden', !isEdc);
    }
    if (edcWhiteInput) edcWhiteInput.disabled = configLocked;
    if (edcBlackInput) edcBlackInput.disabled = configLocked;

    // 控制 PROGMEM 导出选项的可用性（仅 ReadPaper V2 模式可用）
    // 注意：显示/隐藏由 showmethemoney() 函数控制
    if (exportProgmemCheckbox) {
      exportProgmemCheckbox.disabled = configLocked || isEdc || isV3;
      if (isEdc || isV3) {
        exportProgmemCheckbox.checked = false; // EDC 和 V3 模式下自动取消选中
      }
    }

    // Hide/disable smoothing controls for EDC firmware (EDC uses grayscale pipeline)
    try {
      const smoothingContainer = document.getElementById('smoothingControls');
      if (smoothingContainer) smoothingContainer.classList.toggle('is-hidden', isEdc);
      if (typeof smoothingEnable !== 'undefined' && smoothingEnable) smoothingEnable.disabled = configLocked || isEdc;
      if (typeof smoothingPassesInput !== 'undefined' && smoothingPassesInput) smoothingPassesInput.disabled = configLocked || isEdc;
    } catch (e) { /* ignore UI toggle errors */ }

    setEdcThresholdDisplays();
  }

  // Lock/unlock configuration inputs while generation is running
  function lockConfig(lock) {
    configLocked = !!lock;
    try {
      const disabled = configLocked;
      // file chooser + browse
      if (fontFileInput) fontFileInput.disabled = disabled;
      if (btnBrowseFont) btnBrowseFont.disabled = disabled;
      if (btnBrowseOut) btnBrowseOut.disabled = disabled;
      if (outputPath) outputPath.disabled = disabled;

      // main params
      if (sizeInput) sizeInput.disabled = disabled;

      // demo controls
      if (demoEnable) demoEnable.disabled = disabled;
      if (demoText) demoText.disabled = disabled;
      if (demoOut) demoOut.disabled = disabled;

      // batch / workers
      if (chunkSizeInput) chunkSizeInput.disabled = disabled;
      if (workerCountInput) workerCountInput.disabled = disabled;

      // charset options
      if (optGbk) optGbk.disabled = disabled;
      if (optTraditional) optTraditional.disabled = disabled;
      if (optJapanese) optJapanese.disabled = disabled;
    } catch (e) {
      // best-effort
      console.warn('lockConfig failed', e);
    }
    applyFirmwareUiState();
  }

  // Preview scheduling helpers
  let __previewTimeout = null;
  function schedulePreview(file) {
    try { if (__previewTimeout) clearTimeout(__previewTimeout); } catch (e) { }
    __previewTimeout = setTimeout(() => {
      __previewTimeout = null;
      try { generatePreviewFromFile(file); } catch (e) { appendLog('⚠️ 预览调度失败: ' + (e && e.message ? e.message : String(e))); }
    }, 250);
  }

  // Generate a preview image using the same worker pipeline. Safe to call repeatedly.
  async function generatePreviewFromFile(file) {
    if (!file) return;
    try {
      appendLog('准备字体预览...');
      const fontBuffer = await file.arrayBuffer();
      try { window.__lastFontBufferForDemo = fontBuffer; } catch (e) { /* ignore */ }
      // Always use binary fallback rendering for preview (no network or FontFace registration)
      let previewFontFaceName = null;

      // Build a minimal charset from demo text (unique codepoints)
      const demoStr = (demoText && demoText.value && demoText.value.trim()) ? demoText.value : '行至水穷处，坐看云起时';
      const cps = new Set();
      for (const ch of demoStr) {
        const cp = ch.codePointAt(0);
        if (typeof cp === 'number' && cp >= 0x20) cps.add(ch);
      }
      // always include space and a replacement glyph if missing
      cps.add(' ');
      cps.add('\u25A1');
      const charset = Array.from(cps);

      const size = clampSizeByMode(parseInt(sizeInput.value, 10) || getSizeRange().min);
      const isEdc = currentFirmwareMode === FIRMWARE_MODES.EDC;
      const isV3 = currentFirmwareMode === FIRMWARE_MODES.READPAPER_V3;
      const colorDepth = getColorDepthValue();
      const edcThresholds = getEdcThresholds();
      const v3Thresholds = getV3Thresholds();
      const whiteThreshold = isEdc ? edcThresholds.white : (isV3 ? v3Thresholds.white : colorDepth);
      const grayThreshold = isV3 ? v3Thresholds.gray : null;
      const blackThreshold = isEdc ? edcThresholds.black : null;

      const thresholdLog = isEdc ? `white=${whiteThreshold}, black=${blackThreshold}` : (isV3 ? `white=${whiteThreshold}, gray=${grayThreshold}` : `white=${whiteThreshold}`);
      appendLog(`预览: 使用 size=${size}, ${thresholdLog}, chars=${charset.length}`);
      // Diagnostic: log current filter settings so user can see they are read
      try {
        const eg = (document.getElementById('enable_gaussian') && document.getElementById('enable_gaussian').checked) ? true : false;
        const eb = (document.getElementById('enable_bilateral') && document.getElementById('enable_bilateral').checked) ? true : false;
        appendLog(`预览: 滤波设置 gauss=${eg}, bilateral=${eb}`);
      } catch (e) { }
      // run processChunks on this small charset to obtain entries
      const useOtsu = (document.getElementById('use_otsu') && document.getElementById('use_otsu').checked) ? true : false;
      // otsu_denoise checkbox removed; keep denoise disabled by default
      const enableDenoise = false;
      const cpMap = await processChunks({
        fontBuffer,
        charset,
        size,
        whiteThreshold,
        grayThreshold,
        blackThreshold,
        chunkSize: Math.max(1, charset.length),
        workerCount: 1,
        updateProgress: false,
        mode: currentFirmwareMode,
        smoothingPasses: (smoothingEnable && smoothingEnable.checked) ? (parseInt(smoothingPassesInput.value, 10) || 0) : 0,
        useOtsu,
        enableDenoise,
        otsuBias: (document.getElementById('otsu_bias') ? parseInt(document.getElementById('otsu_bias').value, 10) || 0 : 0),
        strokeExpand: (document.getElementById('stroke_expand') ? parseInt(document.getElementById('stroke_expand').value, 10) || 0 : 0)
        , enableGaussian: (document.getElementById('enable_gaussian') && document.getElementById('enable_gaussian').checked) ? true : false,
        gaussianRadius: (document.getElementById('gaussian_radius') ? parseInt(document.getElementById('gaussian_radius').value, 10) || 1 : 1),
        gaussianSigma: (document.getElementById('gaussian_sigma') ? Number(document.getElementById('gaussian_sigma').value) || 1.0 : 1.0),
        enableBilateral: (document.getElementById('enable_bilateral') && document.getElementById('enable_bilateral').checked) ? true : false,
        bilateralRadius: (document.getElementById('bilateral_radius') ? parseInt(document.getElementById('bilateral_radius').value, 10) || 3 : 3),
        bilateralSigmaSpace: (document.getElementById('bilateral_sigma_space') ? Number(document.getElementById('bilateral_sigma_space').value) || 3.0 : 3.0),
        bilateralSigmaRange: (document.getElementById('bilateral_sigma_range') ? Number(document.getElementById('bilateral_sigma_range').value) || 40 : 40)
      });
      
      // convert cpMap -> entries array
      const entries = [];
      for (const [k, v] of cpMap.entries()) entries.push({ cp: v.cp, advance: v.advance, bw: v.bw, bh: v.bh, xo: v.xo, yo: v.yo, bmp: v.bmp });
      // expose last preview entries for debugging in console
      try { window.__lastPreviewEntries = entries.slice(); } catch (e) { /* ignore */ }

      // render demo PNG from entries and show in #previewer
      try {
        const blob = await renderDemoPNG(entries, size, demoStr, previewFontFaceName, currentFirmwareMode);
        const url = URL.createObjectURL(blob);
        const previewer = document.getElementById('previewer');
        // revoke old preview if any
        try {
          if (window.__lastPreviewURL) { URL.revokeObjectURL(window.__lastPreviewURL); }
        } catch (e) { }
        window.__lastPreviewURL = url;
        previewer.innerHTML = `<img src="${url}" alt="预览" class="preview-image"/>`;
        appendLog('预览: 已生成');
      } catch (err) {
        appendLog('⚠️ 预览图片生成失败，尝试回退: ' + (err && err.message ? err.message : String(err)));
        // fallback to font-based render
        try {
          const blob2 = await renderDemoPNG([], size, demoStr, previewFontFaceName, currentFirmwareMode);
          const url2 = URL.createObjectURL(blob2);
          const previewer = document.getElementById('previewer');
          try { if (window.__lastPreviewURL) { URL.revokeObjectURL(window.__lastPreviewURL); } } catch (e) { }
          window.__lastPreviewURL = url2;
          previewer.innerHTML = `<img src="${url2}" alt="预览" class="preview-image"/>`;
        } catch (err2) {
          appendLog('⚠️ 回退渲染也失败: ' + (err2 && err2.message ? err2.message : String(err2)));
        }
      }
    } catch (err) {
      appendLog('⚠️ 生成预览失败: ' + (err && err.message ? err.message : String(err)));
    }
  }

  // File selection handling
  btnBrowseFont.addEventListener('click', () => fontFileInput.click());
  // Ensure slider display is initialized and wired immediately
  try {
    if (whiteInput && whiteValueDisplay) whiteValueDisplay.textContent = whiteInput.value;
    if (whiteInput) {
      whiteInput.addEventListener('input', (e) => {
        if (whiteValueDisplay) whiteValueDisplay.textContent = e.target.value;
        if (currentFirmwareMode !== FIRMWARE_MODES.READPAPER) return;
        try {
          const f = getCurrentFontFile();
          if (f) schedulePreview(f);
        } catch (err) { /* ignore */ }
      });
    }

    // Fine-adjust buttons for the white slider: support click, shift+click (bigger step), and hold for continuous change
    function clampWhite(v) {
      const { min, max } = getColorDepthRange();
      return Math.max(min, Math.min(max, v));
    }

    function applyWhiteValue(v, triggerPreview = true) {
      const newVal = clampWhite(v);
      setColorDepthValue(newVal);
      if (triggerPreview && currentFirmwareMode === FIRMWARE_MODES.READPAPER) {
        try {
          const f = getCurrentFontFile();
          if (f) schedulePreview(f);
        } catch (err) { /* ignore */ }
      }
    }

    let __whiteRepeatTimer = null;
    function startWhiteRepeat(delta, event) {
      // immediate change
      applyWhiteValue(parseInt(whiteInput.value, 10) + delta);
      // then start repeating after a short delay
      clearWhiteRepeat();
      __whiteRepeatTimer = setInterval(() => {
        applyWhiteValue(parseInt(whiteInput.value, 10) + delta);
      }, 120);
    }

    function clearWhiteRepeat() {
      if (__whiteRepeatTimer) { clearInterval(__whiteRepeatTimer); __whiteRepeatTimer = null; }
    }

    if (whiteDecBtn) {
      whiteDecBtn.addEventListener('click', (ev) => {
        const step = ev.shiftKey ? 10 : 1;
        applyWhiteValue(parseInt(whiteInput.value, 10) - step);
      });
      whiteDecBtn.addEventListener('mousedown', (ev) => {
        ev.preventDefault();
        const step = ev.shiftKey ? 10 : 1;
        startWhiteRepeat(-step, ev);
      });
      ['mouseup', 'mouseleave', 'blur'].forEach(ev => whiteDecBtn.addEventListener(ev, clearWhiteRepeat));
    }

    if (whiteIncBtn) {
      whiteIncBtn.addEventListener('click', (ev) => {
        const step = ev.shiftKey ? 10 : 1;
        applyWhiteValue(parseInt(whiteInput.value, 10) + step);
      });
      whiteIncBtn.addEventListener('mousedown', (ev) => {
        ev.preventDefault();
        const step = ev.shiftKey ? 10 : 1;
        startWhiteRepeat(step, ev);
      });
      ['mouseup', 'mouseleave', 'blur'].forEach(ev => whiteIncBtn.addEventListener(ev, clearWhiteRepeat));
    }
    // update preview when size changes; allow free typing but clamp on blur/change
    sizeInput.addEventListener && sizeInput.addEventListener('input', (e) => {
      try {
        const f = getCurrentFontFile();
        if (f) schedulePreview(f);
      } catch (err) { /* ignore */ }
    });

    // enforce allowed range when the user finishes editing (blur or change)
    sizeInput.addEventListener && sizeInput.addEventListener('blur', (e) => {
      try {
        const raw = parseInt(sizeInput.value, 10) || getSizeRange().min;
        const clamped = clampSizeByMode(raw);
        if (clamped !== raw) sizeInput.value = clamped;
        const f = getCurrentFontFile();
        if (f) schedulePreview(f);
      } catch (err) { /* ignore */ }
    });
    sizeInput.addEventListener && sizeInput.addEventListener('change', (e) => {
      try {
        const raw = parseInt(sizeInput.value, 10) || getSizeRange().min;
        const clamped = clampSizeByMode(raw);
        if (clamped !== raw) sizeInput.value = clamped;
      } catch (err) { /* ignore */ }
    });
    // update preview when demo text changes
    demoText.addEventListener && demoText.addEventListener('input', (e) => {
      try {
        const f = getCurrentFontFile();
        if (f) schedulePreview(f);
      } catch (err) { /* ignore */ }
    });

    if (edcWhiteInput) {
      edcWhiteInput.addEventListener('input', () => {
        setEdcThresholdDisplays();
        if (currentFirmwareMode !== FIRMWARE_MODES.EDC) return;
        const f = getCurrentFontFile();
        if (f) schedulePreview(f);
      });
    }
    if (edcBlackInput) {
      edcBlackInput.addEventListener('input', () => {
        setEdcThresholdDisplays();
        if (currentFirmwareMode !== FIRMWARE_MODES.EDC) return;
        const f = getCurrentFontFile();
        if (f) schedulePreview(f);
      });
    }
  } catch (e) { /* ignore */ }

  if (firmwareSelect) {
    firmwareSelect.addEventListener('change', (e) => {
      const val = String(e.target.value || '').toLowerCase();
      if (val === FIRMWARE_MODES.EDC) {
        currentFirmwareMode = FIRMWARE_MODES.EDC;
      } else if (val === FIRMWARE_MODES.READPAPER_V3) {
        currentFirmwareMode = FIRMWARE_MODES.READPAPER_V3;
      } else {
        currentFirmwareMode = FIRMWARE_MODES.READPAPER;
      }
      applyFirmwareUiState();
      const f = getCurrentFontFile();
      if (f) schedulePreview(f);
    });
  }

  applyFirmwareUiState();

  fontFileInput.addEventListener('change', (e) => {
    const f = e.target.files && e.target.files[0];
    if (f) {
      fontPathText.value = f.name;
      // optimistic defaults while we extract proper names
      nameValue.textContent = f.name;
      styleValue.textContent = `(正在解析...)`;
      const base = f.name.replace(/\.[^.]+$/, '');
      outputPath.value = `${base}.bin`;

      // Asynchronously extract family/style from the file (uses opentype.js).
      // Update labels as soon as we can so user sees correct names without
      // needing to click "开始生成".
      (async () => {
        try {
          const names = await getFontNamesFromFile(f);
          if (names && typeof names.family === 'string') nameValue.textContent = names.family;
          if (names && typeof names.style === 'string' && names.style.trim() !== '') styleValue.textContent = names.style;
          else styleValue.textContent = `(未知)`;
        } catch (err) {
          // keep the filename as family if parsing failed
          appendLog('⚠️ 解析字体 name table 失败: ' + (err && err.message ? err.message : String(err)));
          styleValue.textContent = `(未知)`;
        }
      })();
      // Schedule a preview render (debounced)
      schedulePreview(f);
    }
  });

  // Output browse is a prompt in browser demo
  btnBrowseOut.addEventListener('click', () => {
    const v = prompt('请输入输出文件路径（示例: myfont.bin）', outputPath.value || 'output.bin');
    if (v !== null) outputPath.value = v;
  });

  // Cancel generation
  cancelBtn.addEventListener('click', () => {
    if (worker.running) {
      worker.cancelled = true;
      appendLog('用户已请求取消生成...');
      statusValue.textContent = '已取消';
    }
  });

  // -------------------- Helper functions for real generation --------------------
  async function loadFontFileToDocument(file) {
    const name = `GenFont_${Date.now()}`;
    try {
      // Use ArrayBuffer as source to avoid blob/url network loading issues in some browsers
      const buf = await file.arrayBuffer();
      const fontFace = new FontFace(name, buf);
      await fontFace.load();
      document.fonts.add(fontFace);
      return name;
    } catch (err) {
      // Fallback: try blob URL (older browsers)
      try {
        const blobUrl = URL.createObjectURL(file);
        const fontFace2 = new FontFace(name, `url(${blobUrl})`);
        await fontFace2.load();
        document.fonts.add(fontFace2);
        // revoke the blob URL later (can't reliably do it here)
        return name;
      } catch (err2) {
        // Do not log network/load errors; caller will use binary fallback rendering.
        throw err2;
      }
    }
  }

  // Ensure opentype.js loaded in main thread (for extracting font names)
  function ensureOpenTypeLoaded() {
    return new Promise((resolve, reject) => {
      if (window.opentype) return resolve(window.opentype);
      // Try local copy only (network/CDN loading disabled)
      const tryLoad = (src) => new Promise((res, rej) => {
        const s = document.createElement('script');
        s.src = src;
        s.onload = () => res(true);
        s.onerror = () => rej(new Error('load failed: ' + src));
        document.head.appendChild(s);
      });

      (async () => {
        // Try likely local paths relative to this page (extension builds typically place vendors next to pages)
        const localCandidates = ['../vendors/opentype.min.js', './opentype.min.js', './vendors/opentype.min.js'];
        for (const src of localCandidates) {
          try {
            appendLog(`尝试加载本地 opentype.js: ${src}`);
            await tryLoad(src);
            if (window.opentype) return resolve(window.opentype);
          } catch (e) {
            appendLog(`本地加载失败: ${src} -> ${e && e.message ? e.message : String(e)}`);
          }
        }

        // Do not attempt CDN/network loading; require local copy.
        return reject(new Error('无法加载 opentype.js（未找到本地副本，已禁用网络加载）'));
      })();
    });
  }

  async function getFontNamesFromFile(file) {
    // returns { family, style }
    try {
      await ensureOpenTypeLoaded();
      const buffer = await file.arrayBuffer();
      const font = (window.opentype ? window.opentype.parse(buffer) : null);
      // Fallback: minimal name table parser that reads name records directly from the ArrayBuffer
      function parseNameTableFromBuffer(arrayBuffer) {
        try {
          const dv = new DataView(arrayBuffer);
          const u8 = new Uint8Array(arrayBuffer);
          // number of tables (big-endian at offset 4)
          const numTables = dv.getUint16(4, false);
          let nameOffset = null; let nameLength = null;
          // table records start at offset 12
          let off = 12;
          for (let i = 0; i < numTables; i++) {
            const tag = String.fromCharCode(u8[off], u8[off + 1], u8[off + 2], u8[off + 3]);
            const tblOffset = dv.getUint32(off + 8, false);
            const tblLength = dv.getUint32(off + 12, false);
            if (tag === 'name') { nameOffset = tblOffset; nameLength = tblLength; break; }
            off += 16;
          }
          if (nameOffset === null) return [];
          const format = dv.getUint16(nameOffset, false);
          const count = dv.getUint16(nameOffset + 2, false);
          const stringOffset = dv.getUint16(nameOffset + 4, false);
          const records = [];
          let recOff = nameOffset + 6;
          const storageStart = nameOffset + stringOffset;
          for (let i = 0; i < count; i++) {
            const platformID = dv.getUint16(recOff, false);
            const encodingID = dv.getUint16(recOff + 2, false);
            const languageID = dv.getUint16(recOff + 4, false);
            const nameID = dv.getUint16(recOff + 6, false);
            const length = dv.getUint16(recOff + 8, false);
            const offset = dv.getUint16(recOff + 10, false);
            const start = storageStart + offset;
            const end = start + length;
            if (start >= 0 && end <= u8.length) {
              const bytes = u8.slice(start, end);
              // try decode: UTF-16BE for platform 0/3, else try utf-8 then latin1
              let text = null;
              try { if (platformID === 0 || platformID === 3) { text = new TextDecoder('utf-16be', { fatal: false }).decode(bytes); } }
              catch (e) { text = null; }
              if (!text) {
                try { text = new TextDecoder('utf-8', { fatal: false }).decode(bytes); } catch (e) { text = null; }
              }
              if (!text) {
                try { text = new TextDecoder('latin1', { fatal: false }).decode(bytes); } catch (e) { text = null; }
              }
              records.push({ nameID, platformID, encodingID, languageID, bytes, text });
            }
            recOff += 12;
          }
          return records;
        } catch (e) { return []; }
      }
      // Try to read raw name records (mimic tools/generate_1bit_font_bin.py pick_name)
      function getNameRecords(f) {
        try {
          if (f.tables && f.tables.name && Array.isArray(f.tables.name.names)) return f.tables.name.names;
        } catch (e) { }
        return [];
      }

      function recordToText(rec) {
        if (!rec) return null;
        // common property names in opentype.js parsed records
        if (typeof rec.toUnicode === 'function') {
          try { const t = rec.toUnicode(); if (t) return t; } catch (e) { }
        }
        if (rec.string) return String(rec.string);
        if (rec.name) return String(rec.name);
        if (rec.value) return String(rec.value);
        // some builds store as a Buffer/Uint8Array
        if (rec.bytes) {
          try { return new TextDecoder('utf-8', { fatal: false }).decode(rec.bytes); } catch (e) { }
        }
        return null;
      }

      function pickNameFromNameTable(f, nameIds) {
        const recs = getNameRecords(f);
        if (!recs.length) return null;

        const chineseLangs = new Set([0x0804, 0x0404, 0x0C04]);
        const cjkRe = /[\u4E00-\u9FFF]/;

        const candidates = [];
        for (const rec of recs) {
          try {
            if (!rec || (rec.nameID === undefined && rec.nameid === undefined && rec.nameId === undefined)) continue;
            const nid = rec.nameID ?? rec.nameid ?? rec.nameId;
            if (!nameIds.includes(nid)) continue;
            const txt = recordToText(rec);
            if (!txt) continue;

            // attempt to get language ID
            let lang_id = null;
            try { lang_id = rec.langID ?? rec.languageID ?? rec.language ?? null; } catch (e) { lang_id = null; }

            const contains_cjk = cjkRe.test(txt);
            const is_chinese = contains_cjk || (lang_id !== null && chineseLangs.has(Number(lang_id)));
            const platform = rec.platformID ?? rec.platform ?? rec.platformId ?? rec.platformId ?? 999;
            const plat_prio = (platform === 3) ? 2 : (platform === 0 ? 1 : 0);
            candidates.push({ is_chinese: !!is_chinese, plat_prio, platform: Number(platform), text: txt });
          } catch (e) {
            continue;
          }
        }

        if (!candidates.length) return null;

        candidates.sort((a, b) => {
          // same as Python key: (0 if is_chinese else 1, -plat_prio, platformID)
          const ka = (a.is_chinese ? 0 : 1);
          const kb = (b.is_chinese ? 0 : 1);
          if (ka !== kb) return ka - kb;
          if (a.plat_prio !== b.plat_prio) return b.plat_prio - a.plat_prio;
          return a.platform - b.platform;
        });

        return candidates[0].text;
      }

      // Diagnostic: dump a few raw name records (simplified) to log for debugging
      try {
        const raw = getNameRecords(font);
        const simple = [];
        function bytesToHex(bs) {
          try {
            return Array.from(bs).map(b => b.toString(16).padStart(2, '0')).join('');
          } catch (e) { return null; }
        }
        for (let i = 0; i < raw.length; i++) {
          const r = raw[i];
          try {
            const nid = r.nameID ?? r.nameid ?? r.nameId;
            const plat = r.platformID ?? r.platform ?? r.platformId ?? null;
            const lang = r.langID ?? r.languageID ?? r.language ?? null;
            const txt = recordToText(r);

            // try to surface raw bytes and multiple decodings when available
            let rawBytes = null;
            try { rawBytes = r.bytes || r.raw || (r['data'] && r['data'].bytes) || null; } catch (e) { rawBytes = null; }
            let hex = null; let decUtf8 = null; let decUtf16le = null; let decUtf16be = null;
            if (rawBytes) {
              try { const u8 = rawBytes instanceof Uint8Array ? rawBytes : new Uint8Array(rawBytes); hex = bytesToHex(u8); } catch (e) { hex = null; }
              try { decUtf8 = new TextDecoder('utf-8', { fatal: false }).decode(rawBytes); } catch (e) { decUtf8 = null; }
              try { decUtf16le = new TextDecoder('utf-16le', { fatal: false }).decode(rawBytes); } catch (e) { decUtf16le = null; }
              try { decUtf16be = new TextDecoder('utf-16be', { fatal: false }).decode(rawBytes); } catch (e) { decUtf16be = null; }
            }

            simple.push({ idx: i, nameID: nid, platform: plat, langID: lang, text: txt, hasCJK: /[\u4E00-\u9FFF]/.test(String(txt || '')), bytesHex: hex, decodings: { utf8: decUtf8, utf16le: decUtf16le, utf16be: decUtf16be } });
          } catch (e) { /* ignore */ }
          if (simple.length >= 30) break;
        }
        if (window.__FONTGEN_DEBUG) appendLog('diagnostic: name table records (sample): ' + JSON.stringify(simple));
      } catch (e) { if (window.__FONTGEN_DEBUG) appendLog('diagnostic: name table read failed: ' + (e && e.message ? e.message : String(e))); }

      // Try typographic family/style (16/17), then nameID 1/2, then full name (4) as fallback for diagnostics
      let family = pickNameFromNameTable(font, [16, 1]) || pickNameFromNameTable(font, [4]) || pickNameFromNameTable(font, [1]) || null;
      let style = pickNameFromNameTable(font, [17, 2]) || pickNameFromNameTable(font, [2]) || null;
      // If opentype.js didn't expose name records, try parsing raw buffer name table directly
      try {
        const rawRecs = (font && getNameRecords(font) && getNameRecords(font).length) ? getNameRecords(font) : null;
        if ((!rawRecs || rawRecs.length === 0) && buffer) {
          const parsed = parseNameTableFromBuffer(buffer);
          if (parsed && parsed.length) {
            // build a temporary 'font-like' object to feed into existing pick logic
            const fakeFont = { tables: { name: { names: parsed.map((r, idx) => ({ nameID: r.nameID, platformID: r.platformID, langID: r.languageID, bytes: r.bytes, toUnicode: function () { try { return this.text || (this.text = (typeof r.text === 'string' ? r.text : '')); } catch (e) { return ''; } } })) } } };
            const pfamily = pickNameFromNameTable(fakeFont, [16, 1]) || pickNameFromNameTable(fakeFont, [4]) || pickNameFromNameTable(fakeFont, [1]);
            const pstyle = pickNameFromNameTable(fakeFont, [17, 2]) || pickNameFromNameTable(fakeFont, [2]);
            if (pfamily) family = pfamily;
            if (pstyle) style = pstyle;
            if (window.__FONTGEN_DEBUG) appendLog('diagnostic: parsed raw name table, derived family=' + String(pfamily) + ', style=' + String(pstyle));
          }
        }
      } catch (e) { /* ignore */ }
      // If the Full name (nameID=4) contains CJK characters, prefer it when family currently lacks CJK
      try {
        const full4 = pickNameFromNameTable(font, [4]);
        if (full4 && /[\u4E00-\u9FFF]/.test(full4) && (!family || !/[\u4E00-\u9FFF]/.test(family))) {
          family = full4;
        }
      } catch (e) {
        // ignore
      }
      if (window.__FONTGEN_DEBUG) appendLog('diagnostic: pickNameFromNameTable -> family: ' + String(family) + ', style: ' + String(style));

      // fallback to opentype.js convenience objects if name table parsing failed
      if (!family) {
        // try fullName/fontFamily objects but prefer entries that contain CJK characters
        const pickObjPreferCJK = (obj) => {
          if (!obj) return null;
          // prefer any value that contains CJK
          try {
            for (const k of Object.keys(obj)) {
              const v = obj[k];
              if (v && /[\u4E00-\u9FFF]/.test(String(v))) return v;
            }
          } catch (e) { }
          if (obj.en) return obj.en;
          const ks = Object.keys(obj);
          return ks.length ? obj[ks[0]] : null;
        };
        family = pickObjPreferCJK(font.names && font.names.fullName) || pickObjPreferCJK(font.names && font.names.fontFamily) || (file.name.replace(/\.[^.]+$/, ''));
      }
      if (!style) {
        const pickObj = (obj) => {
          if (!obj) return null;
          if (obj.en) return obj.en;
          const ks = Object.keys(obj);
          return ks.length ? obj[ks[0]] : null;
        };
        style = pickObj(font.names && font.names.fontSubfamily) || pickObj(font.names && font.names.fontStyle) || '';
      }

      // Extra fallbacks if style still missing: try opentype.getEnglishName, postScriptName parsing, or filename heuristics
      if (!style || String(style).trim() === '') {
        try {
          if (typeof font.getEnglishName === 'function') {
            const tryNames = ['fontSubfamily', 'fontSubfamily', 'fontSubfamily', 'postScriptName'];
            for (const nkey of tryNames) {
              try {
                const v = font.getEnglishName && font.getEnglishName(nkey);
                if (v) { style = v; break; }
              } catch (e) { }
            }
          }
        } catch (e) { }
      }

      if ((!style || String(style).trim() === '') && font.names && font.names.postScriptName) {
        // try postScriptName fallback (may be object or string)
        try {
          const ps = (typeof font.names.postScriptName === 'string') ? font.names.postScriptName : (font.names.postScriptName.en || Object.values(font.names.postScriptName)[0]);
          if (ps) {
            // try split by - or _ and take trailing token(s) that look like style
            const parts = String(ps).split(/[-_]/).filter(Boolean);
            if (parts.length > 1) {
              const last = parts[parts.length - 1];
              if (/[A-Za-z]+/.test(last)) style = last;
            }
          }
        } catch (e) { }
      }

      if (!style || String(style).trim() === '') {
        // try to infer from filename, e.g. ChillHuoSong_F_Regular_60.otf -> Regular
        try {
          const base = file.name.replace(/\.[^.]+$/, '');
          const tokens = base.split(/[-_ ]+/).filter(Boolean);
          const styleCandidates = ['Regular', 'Bold', 'Italic', 'Oblique', 'Medium', 'Light', 'Black', 'Semibold', 'SemiBold', 'Heavy', 'Thin', 'Book', 'Roman'];
          for (let i = tokens.length - 1; i >= 0; i--) {
            const t = tokens[i];
            for (const sc of styleCandidates) {
              if (t.toLowerCase() === sc.toLowerCase()) { style = sc; break; }
            }
            if (style) break;
          }
        } catch (e) { }
      }
      if (window.__FONTGEN_DEBUG) appendLog('diagnostic: final pick -> family: ' + String(family) + ', style: ' + String(style));
      return { family: String(family || ''), style: String(style || '') };
    } catch (err) {
      appendLog('⚠️ 无法从字体中提取 family/style: ' + (err && err.message ? err.message : String(err)));
      return { family: file.name.replace(/\.[^.]+$/, ''), style: '' };
    }
  }

  // Process charset in chunks using a pool of Workers
  // last parameter updateProgress (optional, default true) controls whether the global
  // progressBar is updated (set false when called for preview rendering).
  async function processChunks({
    fontBuffer,
    charset,
    size,
    whiteThreshold,
    grayThreshold = null,
    blackThreshold = null,
    chunkSize,
    workerCount,
    updateProgress = true,
    smoothingPasses = 1,
    mode = currentFirmwareMode,
    useOtsu = false,
    enableDenoise = false,
    otsuBias = 0,
    strokeExpand = 0
    , enableGaussian = false,
    gaussianRadius = null,
    gaussianSigma = null,
    enableBilateral = false,
    bilateralRadius = null,
    bilateralSigmaSpace = null,
    bilateralSigmaRange = null
  }) {
    // split charset into chunks
    const chunks = [];
    for (let i = 0; i < charset.length; i += chunkSize) chunks.push(charset.slice(i, i + chunkSize));

    let processedChars = 0;
    const totalChars = charset.length;

    const cpMap = new Map(); // cp -> entry with bmp Uint8Array

    let nextChunkIndex = 0;
    let workerMode = FIRMWARE_MODES.READPAPER;
    if (mode === FIRMWARE_MODES.EDC) {
      workerMode = FIRMWARE_MODES.EDC;
    } else if (mode === FIRMWARE_MODES.READPAPER_V3) {
      workerMode = FIRMWARE_MODES.READPAPER_V3;
    }

    return new Promise((resolve, reject) => {
      let active = 0;
      const workers = [];

      function startNext() {
        if (nextChunkIndex >= chunks.length) {
          if (active === 0) {
            // all done
            resolve(cpMap);
          }
          return;
        }
        const idx = nextChunkIndex++;
        const chunk = chunks[idx];
        active++;
        const w = new Worker('font_worker.js');
        workers.push(w);

        // send fontBuffer and chunk - do not transfer fontBuffer to allow reuse
        try {
          w.postMessage({ fontBuffer: fontBuffer, charset: chunk, size, whiteThreshold, grayThreshold, blackThreshold, mode: workerMode, smoothingPasses, useOtsu, enableDenoise, otsuBias, strokeExpand,
            enableGaussian, gaussianRadius, gaussianSigma, enableBilateral, bilateralRadius, bilateralSigmaSpace, bilateralSigmaRange });
        } catch (err) {
          // try sending a copy
          const cloned = fontBuffer.slice(0);
          w.postMessage({ fontBuffer: cloned, charset: chunk, size, whiteThreshold, grayThreshold, blackThreshold, mode: workerMode, smoothingPasses, useOtsu, enableDenoise, otsuBias, strokeExpand,
            enableGaussian, gaussianRadius, gaussianSigma, enableBilateral, bilateralRadius, bilateralSigmaSpace, bilateralSigmaRange }, [cloned]);
        }

        w.onmessage = (ev) => {
          const data = ev.data;
          if (data.progress) {
            return;
          }
          if (data.error) {
            appendLog('Worker 错误: ' + data.error);
            w.terminate(); active--; return reject(new Error(data.error));
          }

          // 如果 worker 提供 diagnostics，则打印前若干用于调试xo/yo/bbox
          try {
            if (data.diagnostics && Array.isArray(data.diagnostics) && data.diagnostics.length) {
              const short = data.diagnostics.slice(0, 20).map(d => ({ cp: d.cp, x: d.x, y: d.y, minX: d.minX, minY: d.minY, bw: d.bw, bh: d.bh, advancePx: d.advancePx, leftSideBearing: d.leftSideBearing }));
              if (window.__FONTGEN_DEBUG) appendLog('Worker diagnostics sample: ' + JSON.stringify(short));
            }
          } catch (e) { /* ignore diagnostics serialization errors */ }

          // data.entries has len fields; data.bitmap is ArrayBuffer of concatenated bitmaps
          const entries = data.entries;
          const bmpArr = new Uint8Array(data.bitmap);
          let off = 0;
          for (const e of entries) {
            const len = e.len || 0;
            let bmp = new Uint8Array(0);
            if (len > 0) {
              bmp = bmpArr.subarray(off, off + len);
            }
            off += len;
            // store entry with bmp copy
            const entry = {
              cp: e.cp & 0xFFFF,
              advance: e.advance & 0xFFFF,
              bw: e.bw & 0xFF,
              bh: e.bh & 0xFF,
              xo: e.xo,
              yo: e.yo,
              bmp: bmp.slice(0), // copy to detach from shared buffer
              missing: !!(e.missing)
            };

            // Merge policy:
            // - Never allow a missing/empty placeholder to overwrite an existing valid glyph.
            // - Allow a valid glyph to overwrite a previous missing placeholder.
            const prevEntry = cpMap.get(entry.cp) || null;
            const prevBmpLen = (prevEntry && prevEntry.bmp && prevEntry.bmp.length) ? prevEntry.bmp.length : 0;
            const newBmpLen = (entry.bmp && entry.bmp.length) ? entry.bmp.length : 0;
            const prevMissing = !!(prevEntry && prevEntry.missing);
            const newMissing = !!entry.missing || ((entry.advance === 0) && (newBmpLen === 0) && ((entry.bw === 0) || (entry.bh === 0)));
            if (prevEntry && !prevMissing && prevBmpLen > 0 && newMissing) {
              // keep the existing valid glyph
              processedChars++;
              if (updateProgress) {
                try { progressBar.value = processedChars; } catch (e) { /* ignore */ }
              }
              continue;
            }

            cpMap.set(entry.cp, entry);
            processedChars++;
            if (updateProgress) {
              try { progressBar.value = processedChars; } catch (e) { /* ignore */ }
            }
          }

          appendLog(`已处理 ${processedChars}/${totalChars}`);

          w.terminate(); active--; startNext();
        };

        w.onerror = (err) => { appendLog('Worker 异常: ' + err.message); w.terminate(); active--; reject(err); };
      }

      // init progress bar
      if (updateProgress && progressBar) {
        progressBar.max = totalChars; progressBar.value = 0;
      }

      // start initial workers
      const initial = Math.min(workerCount, chunks.length);
      for (let i = 0; i < initial; i++) startNext();
    });
  }

  // Build full charset similar to Python's build_charset (ASCII + GBK + Big5/Traditional + optional Japanese)
  async function loadPrecomputedCharset() {
    // Try to fetch precomputed charset JSON generated by tools/generate_charset_json.py
    // Try multiple likely locations to support different dev/build layouts and static servers.
    const candidates = [
      '../assets/charset_default.json',    // extension/pages -> extension/assets
      './assets/charset_default.json',     // same folder's assets
      './charset_default.json',           // same folder
      '/webapp/extension/assets/charset_default.json', // absolute path in dev server
      '/extension/assets/charset_default.json',       // alternate absolute
      '/assets/charset_default.json',     // root assets
      'assets/charset_default.json'
    ];
    for (const c of candidates) {
      try {
        const res = await fetch(c);
        if (!res.ok) continue;
        const j = await res.json();
        if (Array.isArray(j.chars)) return j.chars.map(cp => String.fromCodePoint(cp));
      } catch (e) {
        // ignore and try next
      }
    }
    return null;
  }

  async function buildFullCharset({ include_gbk = true, include_traditional = true, include_japanese = false, demoStr = '' } = {}) {
    // 优先尝试运行时 JS 生成器 (webapp/extension/generateCharset.js)
    try {
      const mod = await import('../generateCharset.js');
      const builder = mod.default || mod.buildCharset;
      if (typeof builder === 'function') {
        appendLog('🔧 使用运行时 JS 字符集生成器构建字符集...');
        const arr = await builder({ includeGBK: !!include_gbk, includeTraditional: !!include_traditional, includeJapanese: !!include_japanese });
        if (arr && arr.length) {
          const chars = Array.from(arr, cp => String.fromCodePoint(cp));
          let demo_added = 0;
          for (const ch of Array.from(demoStr || '')) {
            if (ch && !chars.includes(ch)) { chars.push(ch); demo_added++; }
          }
          if (demo_added > 0) appendLog(`  Demo 文本添加字符: ${demo_added} 个`);
          const unique = Array.from(new Set(chars));
          unique.sort((a, b) => a.codePointAt(0) - b.codePointAt(0));
          // 过滤BMP之外的字符
          const beforeFilter = unique.length;
          const bmpOnly = unique.filter(ch => ch.codePointAt(0) <= 0xFFFF);
          if (bmpOnly.length < beforeFilter) {
            appendLog(`⚠️ 过滤掉 ${beforeFilter - bmpOnly.length} 个补充平面字符（U+10000以上）`);
          }
          appendLog(`✅ 运行时生成完成，字符集大小: ${bmpOnly.length}`);
          return bmpOnly;
        }
      }
    } catch (e) {
      // ignore and fall through to try precomputed JSON
    }

    // Next, try to load the precomputed charset JSON exported by the Python generator.
    try {
      const pre = await loadPrecomputedCharset();
      if (pre && pre.length) {
        appendLog(`✅ 已从预计算 JSON 加载字符集 (共 ${pre.length} 字符)。`);
        // If demoStr provided, ensure its chars are present
        let demo_added = 0;
        for (const ch of Array.from(demoStr || '')) {
          if (ch && !pre.includes(ch)) { pre.push(ch); demo_added++; }
        }
        if (demo_added > 0) appendLog(`  Demo 文本添加字符: ${demo_added} 个`);
        // dedupe & sort
        const unique = Array.from(new Set(pre));
        unique.sort((a, b) => a.codePointAt(0) - b.codePointAt(0));
        // 过滤BMP之外的字符
        const beforeFilter = unique.length;
        const bmpOnly = unique.filter(ch => ch.codePointAt(0) <= 0xFFFF);
        if (bmpOnly.length < beforeFilter) {
          appendLog(`⚠️ 过滤掉 ${beforeFilter - bmpOnly.length} 个补充平面字符（U+10000以上）`);
        }
        appendLog(`✅ 构建完成，字符集大小: ${bmpOnly.length}`);
        return bmpOnly;
      }
    } catch (e) {
      // ignore and fall through to fallback
    }

    // 如果无法加载预计算文件，则退回到一个安全的本地构建（不尝试 TextDecoder），以保证最小可用字符集。
    appendLog('⚠️ 未能加载预计算字符集，回退到本地构建（仅包含ASCII和可选的Unicode范围）——请优先使用 Python 的 --export-charset 以获得完全一致性。');
    const chars = [];
    // ASCII printable
    for (let c = 0x20; c <= 0x7E; c++) chars.push(String.fromCharCode(c));

    // If traditional requested, include Unicode CJK ranges (without Big5 decoding)
    if (include_traditional) {
      const traditional_ranges = [[0x4E00, 0x9FFF], [0x3400, 0x4DBF], [0xF900, 0xFAFF]];
      for (const [start, end] of traditional_ranges) {
        for (let cp = start; cp <= end; cp++) {
          try { const ch = String.fromCharCode(cp); if (ch && ch.trim()) chars.push(ch); } catch (e) { }
        }
      }
    }

    // Japanese ranges
    if (include_japanese) {
      const japanese_ranges = [[0x3040, 0x309F], [0x30A0, 0x30FF], [0x4E00, 0x9FAF], [0x3400, 0x4DBF], [0xFF65, 0xFF9F], [0x31F0, 0x31FF], [0x3200, 0x32FF], [0x3300, 0x33FF]];
      for (const [s, e] of japanese_ranges) {
        for (let cp = s; cp <= e; cp++) {
          try { const ch = String.fromCharCode(cp); if (ch) chars.push(ch); } catch (e) { }
        }
      }
    }

    // special chars and demo
    const special = ['\u2022', '\u25A1', '\uFEFF'];
    for (const s of special) if (!chars.includes(s)) chars.push(s);
    let demo_added = 0;
    for (const ch of Array.from(demoStr || '')) if (ch && !chars.includes(ch)) { chars.push(ch); demo_added++; }
    if (demo_added > 0) appendLog(`  Demo 文本添加字符: ${demo_added} 个`);

    const unique = Array.from(new Set(chars));
    unique.sort((a, b) => a.codePointAt(0) - b.codePointAt(0));
    // 过滤BMP之外的字符
    const beforeFilter = unique.length;
    const bmpOnly = unique.filter(ch => ch.codePointAt(0) <= 0xFFFF);
    if (bmpOnly.length < beforeFilter) {
      appendLog(`⚠️ 过滤掉 ${beforeFilter - bmpOnly.length} 个补充平面字符（U+10000以上）`);
    }
    appendLog(`✅ 本地构建完成，字符集大小: ${bmpOnly.length}`);
    return bmpOnly;
  }

  // 解析自定义 Unicode 范围字符串（例如: 0x4E00-0x9FFF, 0x25A1）
  function parseCustomCharsetText(text) {
    const chars = new Set();
    if (!text || !text.trim()) return [];
    const pattern = /0x[0-9A-Fa-f]+\s*-\s*0x[0-9A-Fa-f]+|0x[0-9A-Fa-f]+/g;
    const matches = text.match(pattern);
    if (!matches) return [];
    for (let m of matches) {
      m = m.replace(/\s/g, '');
      if (m.indexOf('-') >= 0) {
        const [sHex, eHex] = m.split('-');
        try {
          let s = parseInt(sHex, 16);
          let e = parseInt(eHex, 16);
          if (isNaN(s) || isNaN(e)) continue;
          if (s > e) [s, e] = [e, s];
          if (s > 0xFFFF) continue;
          e = Math.min(e, 0xFFFF);
          for (let cp = s; cp <= e; cp++) {
            try { chars.add(String.fromCodePoint(cp)); } catch (e) { }
          }
        } catch (e) { continue; }
      } else {
        try {
          const cp = parseInt(m, 16);
          if (!isNaN(cp) && cp <= 0xFFFF) chars.add(String.fromCodePoint(cp));
        } catch (e) { }
      }
    }
    return Array.from(chars);
  }

  // 从字体文件读取 cmap 并构建全量字符集（使用 opentype.parse）
  async function buildCharsetFromFontFile(fontFile, maxCount = 65536) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = async () => {
        try {
          const buf = reader.result;
          // Start with common chars (match Python get_full_charset which prioritizes common set)
          let set = new Set();
          try {
            const common = await buildFullCharset({ include_gbk: true, include_traditional: true, include_japanese: false, demoStr: '' });
            for (const ch of common) {
              if (set.size >= maxCount) break;
              set.add(ch);
            }
          } catch (e) {
            // ignore and continue with empty common set
          }

          try {
            const font = opentype.parse(buf);
            const cmap = (font.tables && font.tables.cmap && font.tables.cmap.glyphIndexMap) ? font.tables.cmap.glyphIndexMap : null;
            if (cmap) {
              for (const key of Object.keys(cmap)) {
                try {
                  const glyphIndex = cmap[key];
                  if (!glyphIndex || glyphIndex === 0) continue; // skip missing glyph mapping
                  const cp = parseInt(key, 10);
                  if (isNaN(cp)) continue;
                  if (cp > 0xFFFF) continue;
                  if (!set.has(String.fromCodePoint(cp))) {
                    set.add(String.fromCodePoint(cp));
                  }
                  if (set.size >= maxCount) break;
                } catch (e) { }
              }
            }
          } catch (err) {
            // parsing cmap failed, fallthrough to finalize with whatever we have
          }

          // Always include specials
          set.add('\u2022'); set.add('\u25A1'); set.add('\uFEFF');
          const arr = Array.from(set);
          arr.sort((a, b) => a.codePointAt(0) - b.codePointAt(0));
          resolve(arr);
        } catch (e) { reject(e); }
      };
      reader.onerror = (ev) => { reject(new Error('无法读取字体文件')); };
      reader.readAsArrayBuffer(fontFile);
    });
  }

  function range(a, b) { const r = []; for (let i = a; i <= b; i++) r.push(i); return r; }

  function imgDataToBinary(bitmap, bw, bh, threshold) {
    // bitmap: grayscale Uint8Array length = bw*bh
    const bytesPerRow = Math.ceil(bw / 8);
    const out = new Uint8Array(bytesPerRow * bh);
    for (let y = 0; y < bh; y++) {
      for (let bx = 0; bx < bytesPerRow; bx++) {
        let byteVal = 0;
        for (let bit = 0; bit < 8; bit++) {
          const x = bx * 8 + bit;
          if (x >= bw) break;
          const idx = y * bw + x;
          const r = bitmap[idx];
          if (r < threshold) {
            byteVal |= (1 << (7 - bit));
          }
        }
        out[y * bytesPerRow + bx] = byteVal;
      }
    }
    return out;
  }

  // Render using WebWorker (font_worker.js) which uses opentype.js + OffscreenCanvas
  function renderWithWorker(fontFile, charset, size, whiteThreshold) {
    return new Promise((resolve, reject) => {
      const w = new Worker('font_worker.js');
      const reader = new FileReader();
      reader.onload = () => {
        const fontBuffer = reader.result;
        const useOtsu = (document.getElementById('use_otsu') && document.getElementById('use_otsu').checked) ? true : false;
        // otsu_denoise checkbox removed; keep denoise disabled by default
        const enableDenoise = false;
        const otsuBiasVal = (document.getElementById('otsu_bias') ? parseInt(document.getElementById('otsu_bias').value, 10) || 0 : 0);
        const strokeExpandVal = (document.getElementById('stroke_expand') ? parseInt(document.getElementById('stroke_expand').value, 10) || 0 : 0);
        const enableGaussianVal = (document.getElementById('enable_gaussian') && document.getElementById('enable_gaussian').checked) ? true : false;
        const gaussianRadiusVal = (document.getElementById('gaussian_radius') ? parseInt(document.getElementById('gaussian_radius').value, 10) || 1 : 1);
        const gaussianSigmaVal = (document.getElementById('gaussian_sigma') ? Number(document.getElementById('gaussian_sigma').value) || 1.0 : 1.0);
        const enableBilateralVal = (document.getElementById('enable_bilateral') && document.getElementById('enable_bilateral').checked) ? true : false;
        const bilateralRadiusVal = (document.getElementById('bilateral_radius') ? parseInt(document.getElementById('bilateral_radius').value, 10) || 2 : 2);
        const bilateralSigmaSpaceVal = (document.getElementById('bilateral_sigma_space') ? Number(document.getElementById('bilateral_sigma_space').value) || 2.0 : 2.0);
        const bilateralSigmaRangeVal = (document.getElementById('bilateral_sigma_range') ? Number(document.getElementById('bilateral_sigma_range').value) || 25 : 25);
        w.postMessage({ fontBuffer, charset, size, whiteThreshold, useOtsu, enableDenoise, otsuBias: otsuBiasVal, strokeExpand: strokeExpandVal,
          enableGaussian: enableGaussianVal, gaussianRadius: gaussianRadiusVal, gaussianSigma: gaussianSigmaVal,
          enableBilateral: enableBilateralVal, bilateralRadius: bilateralRadiusVal, bilateralSigmaSpace: bilateralSigmaSpaceVal, bilateralSigmaRange: bilateralSigmaRangeVal }, [fontBuffer]);
      };
      reader.onerror = (ev) => { reject(new Error('无法读取字体文件')); };
      reader.readAsArrayBuffer(fontFile);

      w.onmessage = (ev) => {
        const data = ev.data;
        if (data.progress) {
          progressBar.value = data.progress;
          return;
        }
        if (data.error) {
          reject(new Error(data.error));
          w.terminate();
          return;
        }
        // got entries + bitmap (ArrayBuffer transferred)
        const entries = data.entries;
        const bitmapBlob = new Uint8Array(data.bitmap);
        w.terminate();
        resolve({ entries, bitmapBlob });
      };

      w.onerror = (err) => { reject(err); w.terminate(); };
    });
  }

  function assembleBin(entries, bitmapBlob, fontHeight, formatVersion, familyName, styleName) {
    const entrySize = 20;
    const headerSize = 4 + 1 + 1 + 64 + 64;
    const charCount = entries.length;
    const textEncoder = new TextEncoder();
    let currentOffset = headerSize + charCount * entrySize;
    const totalSize = headerSize + charCount * entrySize + bitmapBlob.length;
    const buf = new ArrayBuffer(totalSize);
    const dv = new DataView(buf);
    let p = 0;
    dv.setUint32(p, charCount, true); p += 4;
    dv.setUint8(p, fontHeight & 0xFF); p += 1;
    dv.setUint8(p, formatVersion & 0xFF); p += 1;

    function writeName(s) {
      const str = String(s || '');
      // UTF-8-safe truncation to at most 63 bytes (reserve final byte for null)
      const parts = [];
      let used = 0;
      for (const ch of str) {
        const cb = textEncoder.encode(ch);
        if (used + cb.length > 63) break;
        parts.push(cb);
        used += cb.length;
      }
      // concat parts
      let out;
      if (parts.length === 0) out = new Uint8Array(0);
      else if (parts.length === 1) out = parts[0];
      else {
        out = new Uint8Array(used);
        let off = 0;
        for (const ppart of parts) { out.set(ppart, off); off += ppart.length; }
      }
      const view = new Uint8Array(buf, p, 64);
      view.fill(0);
      if (out && out.length) view.set(out, 0);
      p += 64;
    }

    writeName(familyName);
    writeName(styleName);

    for (const e of entries) {
      dv.setUint16(p, e.cp & 0xFFFF, true); p += 2;
      dv.setUint16(p, e.advance & 0xFFFF, true); p += 2;
      dv.setUint8(p, e.bw & 0xFF); p += 1;
      dv.setUint8(p, e.bh & 0xFF); p += 1;
      dv.setInt8(p, e.xo); p += 1;
      dv.setInt8(p, e.yo); p += 1;
      dv.setUint32(p, currentOffset, true); p += 4;
      const len = typeof e.len === 'number' ? e.len : (e.bmp ? e.bmp.length : 0);
      dv.setUint32(p, len, true); p += 4;
      dv.setUint32(p, 0, true); p += 4;
      currentOffset += len;
    }

    if (bitmapBlob.length) {
      const target = new Uint8Array(buf, p, bitmapBlob.length);
      target.set(bitmapBlob);
      p += bitmapBlob.length;
    }

    return new Blob([buf], { type: 'application/octet-stream' });
  }

  function assembleEdcBin(entries, bitmapBlob, fontHeight) {
    const entrySize = 20;
    const headerSize = 4 + 1; // charCount (uint32) + fontHeight (uint8)
    const charCount = entries.length;
    let bitmapLength = bitmapBlob ? bitmapBlob.length : 0;
    if (!bitmapLength) {
      bitmapLength = entries.reduce((acc, e) => acc + (e.bmp ? e.bmp.length : 0), 0);
    }
    const totalSize = headerSize + charCount * entrySize + bitmapLength;
    const buf = new ArrayBuffer(totalSize);
    const dv = new DataView(buf);
    let p = 0;

    dv.setUint32(p, charCount, true); p += 4;
    dv.setUint8(p, fontHeight & 0xFF); p += 1;

    let currentOffset = headerSize + charCount * entrySize;
    for (const e of entries) {
      const len = e.bmp ? e.bmp.length : 0;
      dv.setUint16(p, e.cp & 0xFFFF, true); p += 2;
      dv.setUint16(p, e.advance & 0xFFFF, true); p += 2;
      dv.setUint8(p, e.bw & 0xFF); p += 1;
      dv.setUint8(p, e.bh & 0xFF); p += 1;
      dv.setInt8(p, e.xo); p += 1;
      dv.setInt8(p, e.yo); p += 1;
      dv.setUint32(p, currentOffset, true); p += 4;
      dv.setUint32(p, len, true); p += 4;
      dv.setUint32(p, 0, true); p += 4;
      currentOffset += len;
    }

    const bitmapStart = headerSize + charCount * entrySize;
    if (bitmapLength > 0) {
      const target = new Uint8Array(buf, bitmapStart, bitmapLength);
      if (bitmapBlob && bitmapBlob.length === bitmapLength) {
        target.set(bitmapBlob);
      } else {
        let off = 0;
        for (const e of entries) {
          if (e.bmp && e.bmp.length) {
            target.set(e.bmp, off);
            off += e.bmp.length;
          }
        }
      }
    }

    return new Blob([buf], { type: 'application/octet-stream' });
  }

  function triggerDownload(blob, filename) {
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url; a.download = filename;
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 30000);
  }

  /**
   * 生成 PROGMEM 格式的 C++ 源代码（仅用于 ReadPaper 固件）
   * 此函数完全遵循 Python bin_to_progmem.py 的实现，直接将 .bin 文件作为字节数组输出
   * @param {Blob} binBlob - 完整的 .bin 文件 Blob
   * @param {string} familyName - 字体族名称
   * @param {string} styleName - 字体样式名称
   * @param {number} fontHeight - 字体高度
   * @param {number} charCount - 字符数量
   * @returns {Blob} C++ 源代码 Blob
   */
  async function assembleProgmemCpp(binBlob, familyName, styleName, fontHeight, charCount) {
    // 生成安全的变量名（移除非字母数字字符）
    const safeName = (familyName || 'Font').replace(/[^a-zA-Z0-9_]/g, '_');
    const safeStyle = (styleName || 'Regular').replace(/[^a-zA-Z0-9_]/g, '_');
    const variableName = 'progmem_font'; // 与 Python 默认值一致

    // 读取完整的 bin 文件数据
    const arrayBuffer = await binBlob.arrayBuffer();
    const fontData = new Uint8Array(arrayBuffer);
    const fileSize = fontData.length;

    let code = [];
    
    // 文件头注释（与 Python 版本一致）
    code.push('// 自动生成的 PROGMEM 字体数据文件');
    code.push(`// 源文件: ${safeName}_${safeStyle}_${fontHeight}px.bin`);
    code.push(`// 字体: ${familyName} ${styleName}`);
    code.push(`// 大小: ${fontHeight}px`);
    code.push(`// 字符数: ${charCount}`);
    code.push(`// 文件大小: ${fileSize} 字节 (${(fileSize/1024).toFixed(2)} KB)`);
    code.push('// ');
    code.push('// 警告：此文件由脚本自动生成，请勿手动编辑！');
    code.push(`// 生成命令: webapp font generator`);
    code.push('');
    
    // 包含头文件（与 Python 版本一致）
    code.push('#define PROGMEM_FONT_DATA_IMPL');
    code.push('#include "progmem_font_data.h"');
    code.push('');
    
    // 全局标志和大小（与 Python 版本一致）
    code.push('// 全局标志：PROGMEM 字体数据可用');
    code.push('const bool g_has_progmem_font = true;');
    code.push('');
    code.push('// 字体数据总大小');
    code.push(`const uint32_t g_progmem_font_size = ${fileSize};`);
    code.push('');
    
    // 字体数据数组（与 Python 版本一致）
    code.push('// 字体数据（存储在 Flash）');
    code.push(`const uint8_t g_${variableName}_data[] PROGMEM = {`);
    
    // 分块写入数据（每行16字节，与 Python 版本一致）
    const chunkSize = 16;
    for (let i = 0; i < fontData.length; i += chunkSize) {
      const chunk = fontData.slice(i, Math.min(i + chunkSize, fontData.length));
      const hexValues = Array.from(chunk).map(b => `0x${b.toString(16).padStart(2, '0').toUpperCase()}`).join(', ');
      
      // 添加注释标记位置（每160字节标记一次，与 Python 版本一致）
      if (i % (chunkSize * 10) === 0) {
        code.push(`    // Offset: 0x${i.toString(16).padStart(6, '0').toUpperCase()} (${i})`);
      }
      
      code.push(`    ${hexValues}${i + chunkSize < fontData.length ? ',' : ''}`);
    }
    
    code.push('};');
    code.push('');
    
    // 别名（与 Python 版本一致）
    code.push('// 别名：方便外部访问');
    code.push(`const uint8_t* const g_progmem_font_data = g_${variableName}_data;`);
    code.push('');

    const cppCode = code.join('\n');
    return new Blob([cppCode], { type: 'text/plain; charset=utf-8' });
  }

  function createBinStats(entries, bitmapBlob, binBlob) {
    // entries: array of finalEntries objects {cp, advance, bw, bh, xo, yo, bmp}
    const totalChars = entries.length;
    let totalBitmapBytes = 0;
    const lens = [];
    const bwbhCount = {};
    let maxLen = 0;
    let minLen = Number.MAX_SAFE_INTEGER;
    for (const e of entries) {
      const len = e.bmp ? e.bmp.length : 0;
      lens.push({ cp: e.cp, len, bw: e.bw, bh: e.bh, advance: e.advance });
      totalBitmapBytes += len;
      if (len > maxLen) maxLen = len;
      if (len < minLen) minLen = len;
      const key = `${e.bw}x${e.bh}`;
      bwbhCount[key] = (bwbhCount[key] || 0) + 1;
    }
    if (minLen === Number.MAX_SAFE_INTEGER) minLen = 0;

    // compute histogram buckets (by len ranges)
    const hist = {};
    for (const it of lens) {
      const b = it.len;
      const bucket = b === 0 ? '0' : (b <= 8 ? '1-8' : (b <= 32 ? '9-32' : (b <= 128 ? '33-128' : (b <= 512 ? '129-512' : '513+'))));
      hist[bucket] = (hist[bucket] || 0) + 1;
    }

    // sample top N largest
    lens.sort((a, b) => b.len - a.len);
    const top = lens.slice(0, 50);

    return {
      generatedAt: new Date().toISOString(),
      charCount: totalChars,
      totalBitmapBytes,
      binSizeBytes: binBlob.size || (binBlob.size === 0 ? 0 : (typeof binBlob.size === 'number' ? binBlob.size : null)),
      avgBytesPerChar: totalChars ? (totalBitmapBytes / totalChars) : 0,
      maxLen, minLen,
      histogram: hist,
      bwbhCount,
      topLargest: top,
      sampleFirst: lens.slice(0, 200),
    };
  }

  async function renderDemoPNG(entries, fontHeight, demoTextStr, fontFaceName, mode = currentFirmwareMode) {
    // Prefer rendering from `entries` (the assembled .bin entries) using strict 1-bit
    // semantics to simulate device output. If entries is missing or empty, fall
    // back to the previous FontFace/opentype rendering.
    const lines = (demoTextStr || '').split('\n');
    if (lines.length === 0 || demoTextStr === '') lines.push('示例文本');

    // Use the passed-in mode directly so V3 is preserved (don't coerce to READPAPER)
    const activeMode = mode;

    try {
      if (entries && entries.length > 0) {
        // Build quick lookup map by codepoint
        const entryMap = new Map(entries.map(e => [e.cp, e]));

        // helper to get space advance
        const spaceEnt = entryMap.get(32) || null;
        const spaceAdv = spaceEnt ? (spaceEnt.advance || Math.max(1, Math.floor(fontHeight / 4))) : Math.max(1, Math.floor(fontHeight / 4));

        // compute per-line metrics (width and line-height)
        let maxW = 0; let totalH = 0; const lineMetrics = [];
        for (const line of lines) {
          let lw = 0; let lh = 0;
          for (let i = 0; i < line.length; i++) {
            const ch = line[i]; const cp = ch.codePointAt(0);
            let ent = entryMap.get(cp);
            if (!ent) ent = entryMap.get(0x25A1) || null; // replacement
            let adv = ent ? (ent.advance || Math.max(1, Math.floor(fontHeight * 0.6))) : Math.max(1, Math.floor(fontHeight * 0.6));
            let bw = ent ? (ent.bw || 0) : 0; let bh = ent ? (ent.bh || 0) : 0;
            lw += adv;
            if (i !== line.length - 1) lw += spaceAdv;
            lh = Math.max(lh, (bh > 0 ? bh : fontHeight));
          }
          lineMetrics.push([lw, lh]);
          maxW = Math.max(maxW, lw);
          totalH += (lineMetrics[lineMetrics.length - 1][1] > 0 ? lineMetrics[lineMetrics.length - 1][1] : fontHeight);
        }

        if (maxW === 0) maxW = 200;
        if (totalH === 0) totalH = fontHeight * lines.length;

        const margin = Math.max(10, Math.floor(fontHeight));
        const canvas = document.createElement('canvas');
        const ctx = canvas.getContext('2d');
        // Add extra vertical padding based on max glyph height to avoid accidental clipping
        let maxGlyphBh = 0;
        for (const v of entryMap.values()) { if (v && v.bh && v.bh > maxGlyphBh) maxGlyphBh = v.bh; }
        const extraV = Math.max(0, maxGlyphBh - Math.floor(fontHeight / 4));
        const topExtra = Math.ceil(extraV / 2);
        const bottomExtra = Math.max(0, extraV - topExtra);
        canvas.width = Math.max(128, Math.ceil(maxW) + margin * 2);
        canvas.height = Math.max(64, Math.ceil(totalH) + margin * 2 + topExtra + bottomExtra);

        // white background
        ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, canvas.width, canvas.height);
        ctx.imageSmoothingEnabled = false;

        // 使用 RenderHelpers 进行位图解码
        let renderMode = 'readpaper';
        if (activeMode === FIRMWARE_MODES.EDC) {
          renderMode = 'edc';
        } else if (activeMode === FIRMWARE_MODES.READPAPER_V3) {
          renderMode = 'readpaper_v3';
        }

        // draw each line; shift starting y by topExtra to give room above glyphs
        let y = margin + (typeof topExtra === 'number' ? topExtra : 0);
        for (let li = 0; li < lines.length; li++) {
          const line = lines[li]; const lh = lineMetrics[li][1];
          let x = margin;
          for (let ci = 0; ci < line.length; ci++) {
            const ch = line[ci]; const cp = ch.codePointAt(0);
            let ent = entryMap.get(cp);
            if (!ent) ent = entryMap.get(0x25A1) || null;
            if (!ent) {
              const adv = Math.max(1, Math.floor(fontHeight * 0.6));
              x += adv; x += spaceAdv; continue;
            }

            const bw = ent.bw || 0; const bh = ent.bh || 0; const bmp = ent.bmp || ent.bitmap || null;
            if (bw > 0 && bh > 0 && bmp && bmp.length) {
              const bytes = bmp instanceof Uint8Array ? bmp : new Uint8Array(bmp);
              const imgData = window.RenderHelpers.decodeBitmap(bytes, bw, bh, ctx, renderMode);
              // vertical bottom align as Python demo, but clamp to canvas to avoid partial clipping
              let ty = Math.floor(y + (lh - bh));
              if (isFinite(ty)) {
                if (ty < 0) ty = 0;
                if (ty + bh > canvas.height) ty = Math.max(0, canvas.height - bh);
              } else {
                ty = Math.floor(y);
              }
              ctx.putImageData(imgData, Math.floor(x), ty);
            }

            const adv = ent.advance || Math.max(1, Math.floor(fontHeight * 0.6));
            x += Math.max(1, Math.floor(adv));
            x += spaceAdv;
          }
          y += Math.max(lh, fontHeight);
        }

        return new Promise((res) => canvas.toBlob(res, 'image/png'));
      }
    } catch (err) {
      // fallthrough to previous font-based rendering
      console.warn('renderDemoPNG: binary-demo render failed, falling back to font render:', err);
    }

    // Fallback: previous behavior (FontFace/opentype/canvas rendering)
    const canvas = document.createElement('canvas');
    const ctx = canvas.getContext('2d');
    // If a fontFaceName is provided and available, use canvas text rendering for demo
    if (fontFaceName) {
      ctx.font = `${fontHeight}px "${fontFaceName}"`;
      ctx.textBaseline = 'top';
      let maxW = 0; let totalH = 0;
      for (const line of lines) { const m = ctx.measureText(line); maxW = Math.max(maxW, Math.ceil(m.width)); totalH += Math.ceil(fontHeight * 1.2); }
      canvas.width = Math.max(128, maxW + 40); canvas.height = Math.max(64, totalH + 40);
      ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, canvas.width, canvas.height);
      ctx.fillStyle = '#000'; ctx.font = `${fontHeight}px "${fontFaceName}"`;
      let y = 20; for (const line of lines) { ctx.fillText(line, 20, y); y += Math.ceil(fontHeight * 1.2); }
      return new Promise((res) => canvas.toBlob(res, 'image/png'));
    }

    // Fallback: render using opentype.js from font buffer (no FontFace)
    if (!window.opentype) await ensureOpenTypeLoaded();
    try {
      const fb = window.__lastFontBufferForDemo;
      if (!fb) throw new Error('缺少字体二进制用于直接渲染 (no fontBuffer)');
      const font = window.opentype.parse(fb);
      // compute bounding box by summing path bounding boxes
      let maxW = 0; let totalH = 0;
      const paths = [];
      for (const line of lines) {
        const path = font.getPath(line, 0, 0, fontHeight);
        const bb = path.getBoundingBox ? path.getBoundingBox() : null;
        const w = bb ? Math.ceil(bb.x2 - bb.x1) : Math.ceil(line.length * fontHeight * 0.6);
        paths.push({ path, w, bb });
        maxW = Math.max(maxW, w);
        totalH += Math.ceil(fontHeight * 1.2);
      }
      canvas.width = Math.max(128, maxW + 40); canvas.height = Math.max(64, totalH + 40);
      ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, canvas.width, canvas.height);
      ctx.fillStyle = '#000';
      let y = 20;
      for (const p of paths) {
        ctx.save();
        ctx.translate(20, y);
        p.path.draw(ctx);
        ctx.restore();
        y += Math.ceil(fontHeight * 1.2);
      }
      return new Promise((res) => canvas.toBlob(res, 'image/png'));
    } catch (err) {
      appendLog('⚠️ Demo 渲染失败: ' + (err && err.message ? err.message : String(err)));
      // fallback to plain text
      ctx.font = `${fontHeight}px sans-serif`;
      ctx.textBaseline = 'top';
      let maxW = 0; let totalH = 0;
      for (const line of lines) { const m = ctx.measureText(line); maxW = Math.max(maxW, Math.ceil(m.width)); totalH += Math.ceil(fontHeight * 1.2); }
      canvas.width = Math.max(128, maxW + 40); canvas.height = Math.max(64, totalH + 40);
      ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, canvas.width, canvas.height);
      ctx.fillStyle = '#000'; ctx.font = `${fontHeight}px sans-serif`;
      let y = 20; for (const line of lines) { ctx.fillText(line, 20, y); y += Math.ceil(fontHeight * 1.2); }
      return new Promise((res) => canvas.toBlob(res, 'image/png'));
    }
  }

  // Simple plain-text demo renderer that never depends on opentype.js or FontFace
  async function renderDemoPlain(fontHeight, demoTextStr) {
    const lines = (demoTextStr || '').split('\n');
    if (lines.length === 0 || demoTextStr === '') lines.push('示例文本');
    const canvas = document.createElement('canvas');
    const ctx = canvas.getContext('2d');
    ctx.font = `${fontHeight}px sans-serif`;
    ctx.textBaseline = 'top';
    let maxW = 0; let totalH = 0;
    for (const line of lines) { const m = ctx.measureText(line); maxW = Math.max(maxW, Math.ceil(m.width)); totalH += Math.ceil(fontHeight * 1.2); }
    canvas.width = Math.max(128, maxW + 40); canvas.height = Math.max(64, totalH + 40);
    ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = '#000'; ctx.font = `${fontHeight}px sans-serif`;
    let y = 20; for (const line of lines) { ctx.fillText(line, 20, y); y += Math.ceil(fontHeight * 1.2); }
    return new Promise((res) => canvas.toBlob(res, 'image/png'));
  }

  // -------------------- Generation flow --------------------
  async function startGeneration() {
    if (!fontPathText.value) { showCard('请选择字体文件（浏览）', { type: 'error' }); return; }
    if (!outputPath.value) { showCard('请指定输出文件路径', { type: 'error' }); return; }
    try {
      if (logEl instanceof HTMLTextAreaElement || 'value' in logEl) logEl.value = '';
      else logEl.textContent = '';
    } catch (e) {
      try { logEl.textContent = ''; } catch (e) { /* ignore */ }
    }
    appendLog('开始生成');
    statusValue.textContent = '生成中...';
    generateBtn.disabled = true; cancelBtn.disabled = false; progressBar.value = 0;
    // lock configuration inputs while generation runs
    try { lockConfig(true); } catch (e) { }
    worker = { cancelled: false, running: true };

    const file = fontFileInput.files && fontFileInput.files[0];
    if (!file) { showCard('请通过右侧“浏览...”选择字体文件（不是仅输入文件名）', { type: 'error' }); return finishGeneration(1); }

    try {
      // Read font binary and always use binary fallback rendering (no FontFace/network registration)
      const fontBuffer = await file.arrayBuffer();
      let fontFaceName = null;
      try { window.__lastFontBufferForDemo = fontBuffer; } catch (e) { /* ignore */ }

      appendLog('准备字体与参数...');
      const fontNames = await getFontNamesFromFile(file);
      nameValue.textContent = fontNames.family;
      styleValue.textContent = fontNames.style || '(未知)';

      const isEdc = currentFirmwareMode === FIRMWARE_MODES.EDC;
      const isV3 = currentFirmwareMode === FIRMWARE_MODES.READPAPER_V3;
      const colorDepth = getColorDepthValue();
      const edcThresholds = getEdcThresholds();
      const v3Thresholds = getV3Thresholds();
      const whiteThreshold = isEdc ? edcThresholds.white : (isV3 ? v3Thresholds.white : colorDepth);
      const grayThreshold = isV3 ? v3Thresholds.gray : null; // V3 模式的灰色阈值
      const blackThreshold = isEdc ? edcThresholds.black : null;

      let firmwareLabel = 'ReadPaper';
      if (isEdc) firmwareLabel = 'EDC Book';
      else if (isV3) firmwareLabel = 'Read Paper V3';
      appendLog(`固件模式: ${firmwareLabel}`);

      // build charset according to UI options
      const useFull = !!(useFullCharset && useFullCharset.checked);
      const useCommon = !!(useCommonCharset && useCommonCharset.checked);
      const useCustom = !!(useCustomCharset && useCustomCharset.checked);
      const customText = (customUnicodeInput && customUnicodeInput.value) ? customUnicodeInput.value : '';
      let demoStrVal = demoText.value || '';
      if (demoEnable.checked && (!demoStrVal || demoStrVal.trim() === '')) demoStrVal = '行至水穷处，坐看云起时';
      appendLog('构建字符集 (可能很大，请耐心等待)...');
      let charset = [];
      if (useFull) {
        appendLog('🔧 使用字体 cmap 构建全量字符集（最高 65536）...');
        try {
          charset = await buildCharsetFromFontFile(file, 65536);
          // ensure demo chars
          for (const ch of Array.from(demoStrVal || '')) if (ch && !charset.includes(ch)) charset.push(ch);
          appendLog(`✅ 字体 cmap 构建完成，字符数: ${charset.length}`);
        } catch (e) {
          appendLog('⚠️ 从字体构建全量字符集失败，回退到预计算/本地构建: ' + (e && e.message ? e.message : String(e)));
          charset = await buildFullCharset({ include_gbk: true, include_traditional: true, include_japanese: false, demoStr: demoStrVal });
        }
      } else {
        // use common/custom options
        if (useCommon) {
          // build common via existing helper (GBK + Big5 fallback)
          const common = await buildFullCharset({ include_gbk: true, include_traditional: false, include_japanese: false, demoStr: demoStrVal });
          charset = charset.concat(common);
        }
        if (useCustom) {
          const parsed = parseCustomCharsetText(customText);
          if (parsed && parsed.length) {
            charset = charset.concat(parsed);
            appendLog(`✅ 添加自定义字符: ${parsed.length} 个`);
          }
        }
        // dedupe & sort
        charset = Array.from(new Set(charset));
        charset.sort((a, b) => a.codePointAt(0) - b.codePointAt(0));
        
        // 过滤掉BMP之外的字符以避免截断冲突
        const beforeBmpFilter = charset.length;
        charset = charset.filter(ch => ch.codePointAt(0) <= 0xFFFF);
        if (charset.length < beforeBmpFilter) {
          const filtered = beforeBmpFilter - charset.length;
          appendLog(`⚠️ 过滤掉 ${filtered} 个补充平面字符（U+10000以上），避免与BMP字符冲突`);
        }
      }
      appendLog(`总字符数: ${charset.length}`);

      // prepare progress bar for this run
      try {
        progressBar.max = charset.length;
        progressBar.value = 0;
      } catch (e) { /* ignore if progressBar missing */ }

      const size = clampSizeByMode(parseInt(sizeInput.value, 10) || getSizeRange().min);

      if (isEdc && blackThreshold !== null && blackThreshold <= whiteThreshold) {
        appendLog('❌ 配置错误: 黑色门限必须大于白色门限');
        showCard('黑色门限必须大于白色门限', { type: 'error' });
        return finishGeneration(1);
      }
      const chunkSize = Math.max(50, parseInt(document.getElementById('chunkSize').value, 10) || 800);
      const workerCount = Math.max(1, parseInt(document.getElementById('workerCount').value, 10) || 2);

      const thresholdLog = isEdc ? `white=${whiteThreshold}, black=${blackThreshold}` : `white=${whiteThreshold}`;
      appendLog(`开始分批处理: chunkSize=${chunkSize}, workers=${workerCount}, ${thresholdLog}`);
      // whether to compute per-glyph Otsu thresholds in the worker
      const useOtsu = (document.getElementById('use_otsu') && document.getElementById('use_otsu').checked) ? true : false;
      // otsu_denoise checkbox removed; keep denoise disabled by default
      const enableDenoise = false;
      const cpMap = await processChunks({
        fontBuffer,
        charset,
        size,
        whiteThreshold,
        grayThreshold,
        blackThreshold,
        chunkSize,
        workerCount,
        updateProgress: true,
        useOtsu,
        enableDenoise,
        otsuBias: (document.getElementById('otsu_bias') ? parseInt(document.getElementById('otsu_bias').value, 10) || 0 : 0),
        strokeExpand: (document.getElementById('stroke_expand') ? parseInt(document.getElementById('stroke_expand').value, 10) || 0 : 0),
        mode: currentFirmwareMode,
        smoothingPasses: (smoothingEnable && smoothingEnable.checked) ? (parseInt(smoothingPassesInput.value, 10) || 0) : 0
        , enableGaussian: (document.getElementById('enable_gaussian') && document.getElementById('enable_gaussian').checked) ? true : false,
        gaussianRadius: (document.getElementById('gaussian_radius') ? parseInt(document.getElementById('gaussian_radius').value, 10) || 1 : 1),
        gaussianSigma: (document.getElementById('gaussian_sigma') ? Number(document.getElementById('gaussian_sigma').value) || 1.0 : 1.0),
        enableBilateral: (document.getElementById('enable_bilateral') && document.getElementById('enable_bilateral').checked) ? true : false,
        bilateralRadius: (document.getElementById('bilateral_radius') ? parseInt(document.getElementById('bilateral_radius').value, 10) || 2 : 2),
        bilateralSigmaSpace: (document.getElementById('bilateral_sigma_space') ? Number(document.getElementById('bilateral_sigma_space').value) || 2.0 : 2.0),
        bilateralSigmaRange: (document.getElementById('bilateral_sigma_range') ? Number(document.getElementById('bilateral_sigma_range').value) || 25 : 25)
      });
      if (worker.cancelled) return finishGeneration(-1);

      appendLog('检查并补充字体 family/style 中的字符（若缺失）...');
      // ensure family/style chars present
      const ensureChars = Array.from((fontNames.family || '') + (fontNames.style || ''));
      const missing = [];
      for (const ch of ensureChars) {
        const cp = ch.codePointAt(0);
        if (cp >= 0x20 && !cpMap.has(cp)) missing.push(ch);
      }
      if (missing.length > 0) {
        appendLog(`需要渲染额外字符: ${missing.length} 个`);
        // render missing chars in a single small batch
        await processChunks({
          fontBuffer,
          charset: missing,
          size,
          whiteThreshold,
          blackThreshold,
          chunkSize: missing.length,
          workerCount: 1,
          updateProgress: false,
          useOtsu,
          enableDenoise,
          otsuBias: (document.getElementById('otsu_bias') ? parseInt(document.getElementById('otsu_bias').value, 10) || 0 : 0),
          strokeExpand: (document.getElementById('stroke_expand') ? parseInt(document.getElementById('stroke_expand').value, 10) || 0 : 0),
          mode: currentFirmwareMode,
          smoothingPasses: (smoothingEnable && smoothingEnable.checked) ? (parseInt(smoothingPassesInput.value, 10) || 0) : 0
        , enableGaussian: (document.getElementById('enable_gaussian') && document.getElementById('enable_gaussian').checked) ? true : false,
        gaussianRadius: (document.getElementById('gaussian_radius') ? parseInt(document.getElementById('gaussian_radius').value, 10) || 1 : 1),
        gaussianSigma: (document.getElementById('gaussian_sigma') ? Number(document.getElementById('gaussian_sigma').value) || 1.0 : 1.0),
        enableBilateral: (document.getElementById('enable_bilateral') && document.getElementById('enable_bilateral').checked) ? true : false,
        bilateralRadius: (document.getElementById('bilateral_radius') ? parseInt(document.getElementById('bilateral_radius').value, 10) || 2 : 2),
        bilateralSigmaSpace: (document.getElementById('bilateral_sigma_space') ? Number(document.getElementById('bilateral_sigma_space').value) || 2.0 : 2.0),
        bilateralSigmaRange: (document.getElementById('bilateral_sigma_range') ? Number(document.getElementById('bilateral_sigma_range').value) || 25 : 25)
        }).then((mmap) => {
          for (const [k, v] of mmap.entries()) cpMap.set(k, v);
        });
      }

      appendLog('合并并排序条目，准备组装 .bin 文件...');
      // build final entries: control chars 0x00-0x1F, then sorted codepoints >=0x20
      const controlChars = [];
      for (let cp = 0; cp < 0x20; cp++) controlChars.push({ cp, advance: 0, bw: 0, bh: 0, xo: 0, yo: 0, bmp: new Uint8Array(0) });

      const codepoints = Array.from(cpMap.keys())
        .filter(k => k >= 0x20)
        .filter((cp) => {
          const e = cpMap.get(cp);
          if (!e) return false;
          if (e.missing) return false;
          // Also drop "empty placeholder" entries (advance==0 and no bitmap).
          // Keep legitimate empty-bitmaps like space (advance>0).
          const bmpLen = (e.bmp && e.bmp.length) ? e.bmp.length : 0;
          if ((e.advance === 0) && (bmpLen === 0) && ((e.bw === 0) || (e.bh === 0))) return false;
          return true;
        })
        .sort((a, b) => a - b);
      const finalEntries = [...controlChars];
      for (const cp of codepoints) {
        const e = cpMap.get(cp);
        finalEntries.push({ cp: e.cp, advance: e.advance, bw: e.bw, bh: e.bh, xo: e.xo, yo: e.yo, bmp: e.bmp });
      }

      // concat bitmap data in same order
      let totalBmpLen = 0;
      for (const e of finalEntries) totalBmpLen += (e.bmp ? e.bmp.length : 0);
      const bitmapBlob = new Uint8Array(totalBmpLen);
      let off = 0;
      for (const e of finalEntries) {
        if (e.bmp && e.bmp.length) { bitmapBlob.set(e.bmp, off); off += e.bmp.length; }
      }

      const binBlob = isEdc
        ? assembleEdcBin(finalEntries, bitmapBlob, size)
        : assembleBin(finalEntries, bitmapBlob, size, isV3 ? 3 : 2, fontNames.family, fontNames.style);
      appendLog('触发下载...');
      triggerDownload(binBlob, outputPath.value || (fontNames.family + '.bin'));

      // 导出 PROGMEM C++ 文件（仅 ReadPaper 模式且用户勾选了选项）
      if (!isEdc && exportProgmemCheckbox && exportProgmemCheckbox.checked) {
        try {
          appendLog('生成 C++ PROGMEM 文件...');
          // 使用完整的 binBlob 和字体信息（与 Python bin_to_progmem.py 一致）
          const cppBlob = await assembleProgmemCpp(binBlob, fontNames.family, fontNames.style, size, finalEntries.length);
          const cppFilename = (outputPath.value || (fontNames.family + '.bin')).replace(/\.bin$/, '') + '_progmem.cpp';
          triggerDownload(cppBlob, cppFilename);
          appendLog('✅ 已导出 C++ PROGMEM 文件: ' + cppFilename);
        } catch (err) {
          appendLog('⚠️ 导出 C++ PROGMEM 失败: ' + (err && err.message ? err.message : String(err)));
        }
      }

      // export diagnostics/stats to help compare with Python output (only when debug enabled)
      if (window.__FONTGEN_DEBUG) {
        try {
          const stats = createBinStats(finalEntries, bitmapBlob, binBlob);
          const statsBlob = new Blob([JSON.stringify(stats, null, 2)], { type: 'application/json' });
          triggerDownload(statsBlob, (outputPath.value || (fontNames.family + '.bin')).replace(/\.bin$/, '') + '.stats.json');
          appendLog('已导出 .bin 统计信息 (.stats.json) 用于对比');
        } catch (err) {
          appendLog('⚠️ 导出统计信息失败: ' + (err && err.message ? err.message : String(err)));
        }
      }

      // Demo PNG export is intentionally silent when preview is generated; no user-facing log.

      appendLog('✅ 生成完成');
      finishGeneration(0);
    } catch (err) {
      console.error(err);
      appendLog('❌ 错误: ' + (err && err.message ? err.message : String(err)));
      finishGeneration(1);
    }
  }

  function finishGeneration(code) {
    worker.running = false; worker.cancelled = false; generateBtn.disabled = false; cancelBtn.disabled = true;
    // unlock configuration inputs now that generation finished
    try { lockConfig(false); } catch (e) { }
    if (code === 0) {
      statusValue.textContent = '生成完成'; showCard('字体文件生成完成', { type: 'success' });
    } else if (code === -1) {
      statusValue.textContent = '已取消';
    } else {
      statusValue.textContent = '生成失败'; showCard('生成失败，请查看日志', { type: 'error' });
    }
  }

  generateBtn.addEventListener('click', () => { startGeneration(); });


});

console.log('FontGenerator GUI 已加载（含浏览器端 .bin 生成实现）');
