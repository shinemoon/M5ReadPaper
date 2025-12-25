// Basic client-side image preprocess: load -> draw -> apply threshold/dither -> output
(function(){
    const fileInput = document.getElementById('fileInput');
    const srcCanvas = document.getElementById('srcCanvas');
    const outCanvas = document.getElementById('outCanvas');
    const ctxSrc = srcCanvas.getContext('2d');
    const ctxOut = outCanvas.getContext('2d');
    // offscreen original buffer: keep original resized pixels here so each reprocess uses unmodified source
    const origCanvas = document.createElement('canvas');
    const ctxOrig = origCanvas.getContext('2d');

    const scaleRange = document.getElementById('scaleRange');
    const scaleVal = document.getElementById('scaleVal');
    const thresholdRange = document.getElementById('thresholdRange');
    const thresholdVal = document.getElementById('thresholdVal');
    const ditherMode = document.getElementById('ditherMode');
    const grayscaleMode = document.getElementById('grayscaleMode');
    const grayLevels = document.getElementById('grayLevels');
    const backgroundFill = document.getElementById('backgroundFill');
    const magicTolerance = document.getElementById('magicTolerance');
    const magicToleranceVal = document.getElementById('magicToleranceVal');
    const magicModeBtn = document.getElementById('magicModeBtn');
    const magicUndoBtn = document.getElementById('magicUndoBtn');
    const outWidth = document.getElementById('outWidth');
    const outHeight = document.getElementById('outHeight');
    const resetSize = document.getElementById('resetSize');
    const applyBtn = document.getElementById('applyBtn');
    const downloadBtn = document.getElementById('downloadBtn');

    let img = new Image();
    // Pan state: offsetX/Y represent the position of processed image within the target output frame
    // (user drags the image to position it within the output box)
    let panOffsetX = 0, panOffsetY = 0;
    let isDragging = false;
    let dragStartX = 0, dragStartY = 0;
    let currentScale = 1.0; // Current display scale (independent of scaleRange for initial sizing)
    let isMagicMode = false;

    let transparentMask = null;
    let maskWidth = 0;
    let maskHeight = 0;
    let maskHistory = [];
    const MAGIC_HISTORY_LIMIT = 20;
    
    // 调试模式开关
    let debugMode = false;
    window.showmethemoney = function(enable) {
        debugMode = enable !== false;
        console.log(debugMode ? '🔧 调试模式已开启' : '🔒 调试模式已关闭');
        if(debugMode) {
            console.log('提示: 调用 showmethemoney(false) 可关闭调试模式');
        }
    };

    const checkerPatternCanvas = document.createElement('canvas');
    checkerPatternCanvas.width = checkerPatternCanvas.height = 16;
    const checkerCtx = checkerPatternCanvas.getContext('2d');
    checkerCtx.fillStyle = '#d0d0d0';
    checkerCtx.fillRect(0,0,16,16);
    checkerCtx.fillStyle = '#f0f0f0';
    checkerCtx.fillRect(0,0,8,8);
    checkerCtx.fillRect(8,8,8,8);

    function updateScaleLabel(){ scaleVal.textContent = scaleRange.value + '%'; }
    function updateThresholdLabel(){ thresholdVal.textContent = thresholdRange.value; }
    function updateMagicToleranceLabel(){ magicToleranceVal.textContent = magicTolerance.value; }

    function updateUndoState(){
        if(magicUndoBtn){
            magicUndoBtn.disabled = maskHistory.length === 0;
        }
    }

    function clearMaskHistory(){
        maskHistory.length = 0;
        updateUndoState();
    }

    function pushMaskHistory(snapshot){
        maskHistory.push(snapshot);
        if(maskHistory.length > MAGIC_HISTORY_LIMIT){
            maskHistory.shift();
        }
        updateUndoState();
    }

    function resetMask(w, h){
        maskWidth = w;
        maskHeight = h;
        transparentMask = new Uint8Array(w * h);
        clearMaskHistory();
    }

    function ensureMask(w, h){
        if(!transparentMask || maskWidth !== w || maskHeight !== h){
            resetMask(w, h);
        }
    }

    function fillPreviewBackground(ctx, ow, oh, fillColor){
        ctx.clearRect(0, 0, ow, oh);
        if(fillColor === 'transparent'){
            const pattern = ctx.createPattern(checkerPatternCanvas, 'repeat');
            if(pattern){
                ctx.fillStyle = pattern;
                ctx.fillRect(0, 0, ow, oh);
            }
        } else {
            ctx.fillStyle = fillColor;
            ctx.fillRect(0, 0, ow, oh);
        }
    }

    function setCanvasCursor(){
        if(isMagicMode){
            srcCanvas.style.cursor = 'crosshair';
        } else if(isDragging){
            srcCanvas.style.cursor = 'grabbing';
        } else if(img && img.width > 0){
            srcCanvas.style.cursor = 'grab';
        } else {
            srcCanvas.style.cursor = 'default';
        }
    }

    function toggleMagicMode(){
        isMagicMode = !isMagicMode;
        isDragging = false;
        magicModeBtn.classList.toggle('btn-primary', isMagicMode);
        setCanvasCursor();
    }

    function getCanvasCoords(e){
        const rect = srcCanvas.getBoundingClientRect();
        
        // canvas的逻辑尺寸
        const canvasWidth = srcCanvas.width;
        const canvasHeight = srcCanvas.height;
        
        // canvas元素的显示尺寸（CSS尺寸）
        const displayWidth = rect.width;
        const displayHeight = rect.height;
        
        // 计算canvas内容的实际显示尺寸和位置（考虑object-fit: contain）
        const canvasAspect = canvasWidth / canvasHeight;
        const displayAspect = displayWidth / displayHeight;
        
        let contentWidth, contentHeight, contentOffsetX, contentOffsetY;
        
        if (canvasAspect > displayAspect) {
            // canvas更宽，以宽度为准，高度居中
            contentWidth = displayWidth;
            contentHeight = displayWidth / canvasAspect;
            contentOffsetX = 0;
            contentOffsetY = (displayHeight - contentHeight) / 2;
        } else {
            // canvas更高，以高度为准，宽度居中
            contentHeight = displayHeight;
            contentWidth = displayHeight * canvasAspect;
            contentOffsetX = (displayWidth - contentWidth) / 2;
            contentOffsetY = 0;
        }
        
        // 鼠标在canvas元素上的位置
        const mouseX = e.clientX - rect.left;
        const mouseY = e.clientY - rect.top;
        
        // 鼠标相对于实际内容区域的位置
        const relativeX = mouseX - contentOffsetX;
        const relativeY = mouseY - contentOffsetY;
        
        // 转换为canvas逻辑坐标
        const x = (relativeX / contentWidth) * canvasWidth;
        const y = (relativeY / contentHeight) * canvasHeight;
        
        // 调试：显示坐标转换细节
        if(debugMode) console.log('📍 getCanvasCoords 坐标转换 (考虑object-fit):');
        if(debugMode) {
            console.log('   浏览器坐标:', { clientX: e.clientX, clientY: e.clientY });
            console.log('   canvas元素矩形:', { left: rect.left, top: rect.top, width: displayWidth, height: displayHeight });
            console.log('   canvas逻辑尺寸:', { width: canvasWidth, height: canvasHeight });
            console.log('   canvas宽高比:', canvasAspect.toFixed(3), 'vs 显示区域宽高比:', displayAspect.toFixed(3));
            console.log('   实际内容显示尺寸:', { width: contentWidth, height: contentHeight });
            console.log('   内容偏移量(object-fit居中):', { offsetX: contentOffsetX, offsetY: contentOffsetY });
            console.log('   鼠标在canvas元素上:', { mouseX, mouseY });
            console.log('   鼠标在实际内容上:', { relativeX, relativeY });
            console.log('   转换后的canvas逻辑坐标:', { x, y });
            
            if (relativeX < 0 || relativeY < 0 || relativeX > contentWidth || relativeY > contentHeight) {
                console.log('   ⚠️ 警告: 点击位置在canvas内容区域外（点击在黑边或padding上）');
            }
        }
        
        return { x, y };
    }

    scaleRange.addEventListener('input', ()=> { updateScaleLabel(); drawSrc(); });
    thresholdRange.addEventListener('input', ()=> { updateThresholdLabel(); applyProcess(); });
    magicTolerance.addEventListener('input', () => { updateMagicToleranceLabel(); });
    magicModeBtn.addEventListener('click', (e)=>{ e.preventDefault(); if(outCanvas.width){ toggleMagicMode(); }});
    magicUndoBtn.addEventListener('click', (e)=>{ e.preventDefault(); undoMagicSelection(); });

    fileInput.addEventListener('change', (ev) => {
        const f = ev.target.files[0];
        if(!f) return;
        const url = URL.createObjectURL(f);
        img.onload = () => {
            URL.revokeObjectURL(url);
            // Reset state on new image
            panOffsetX = 0; panOffsetY = 0;
            currentScale = 1.0;
            scaleRange.value = 100;
            updateScaleLabel();
            updateMagicToleranceLabel();
            drawSrc();
        };
        img.src = url;
    });

    function drawSrc(){
        if(!img || img.width===0) return;

        // Determine BASE scaled image size (fit inside target output, preserving aspect ratio)
        // This is the "100%" reference size
        const ow = parseInt(outWidth.value) || 540;
        const oh = parseInt(outHeight.value) || 960;
        const srcW = img.width, srcH = img.height;
        
        // Calculate base size that fits inside output dimensions
        const sx = ow / srcW;
        const sy = oh / srcH;
        const baseScale = Math.min(sx, sy);
        const baseW = Math.max(1, Math.round(srcW * baseScale));
        const baseH = Math.max(1, Math.round(srcH * baseScale));

        // Apply user's scale percentage on top of base size
        currentScale = scaleRange.value / 100;
        const w = Math.max(1, Math.round(baseW * currentScale));
        const h = Math.max(1, Math.round(baseH * currentScale));

        // draw the resized original into the offscreen buffer
        origCanvas.width = w; origCanvas.height = h;
        ctxOrig.clearRect(0,0,w,h);
        ctxOrig.drawImage(img, 0,0,w,h);
        
        // Set outCanvas to match processed size for internal use
        outCanvas.width = w; outCanvas.height = h;

        resetMask(w, h);
        magicModeBtn.classList.remove('btn-primary');
        isMagicMode = false;
        setCanvasCursor();
        
        // auto apply preview (will draw into srcCanvas at target output size)
        applyProcess();
    }

    function applyProcess(){
        if(!img || img.width===0) return;
        const w = origCanvas.width, h = origCanvas.height;
        const src = ctxOrig.getImageData(0,0,w,h);
        const out = ctxOut.createImageData(w,h);
        const threshold = parseInt(thresholdRange.value);
        const gray = grayscaleMode.value;
        const levels = parseInt(grayLevels.value) || 16;
        const dither = ditherMode.value;

        // copy + process
        if(dither === 'floyd'){
            // simple Floyd–Steinberg dithering on luminance
            const lum = new Float32Array(w*h);
            for(let y=0;y<h;y++){
                for(let x=0;x<w;x++){
                    const i = (y*w+x)*4;
                    const r = src.data[i], g = src.data[i+1], b = src.data[i+2];
                    let v = 0.2126*r + 0.7152*g + 0.0722*b;
                    if(gray === 'none') v = (r+g+b)/3;
                    lum[y*w+x] = v;
                }
            }
            for(let y=0;y<h;y++){
                for(let x=0;x<w;x++){
                    const idx = y*w+x;
                    const oldv = lum[idx];
                    let newv;
                    if(levels === 2){
                        newv = oldv < threshold ? 0 : 255;
                    } else {
                        const q = Math.round((levels - 1) * (oldv / 255));
                        newv = (levels > 1) ? (q * (255 / (levels - 1))) : oldv;
                    }
                    const err = oldv - newv;
                    lum[idx] = newv;
                    if(x+1 < w) lum[idx+1] += err * 7/16;
                    if(x-1 >=0 && y+1 < h) lum[idx + w -1] += err * 3/16;
                    if(y+1 < h) lum[idx + w] += err * 5/16;
                    if(x+1 < w && y+1 < h) lum[idx + w +1] += err * 1/16;
                }
            }
            for(let y=0;y<h;y++){
                for(let x=0;x<w;x++){
                    const i = (y*w+x)*4;
                    const v = (levels === 2) ? (lum[y*w+x] < 128 ? 0 : 255) : Math.max(0, Math.min(255, Math.round(lum[y*w+x])));
                    out.data[i]=out.data[i+1]=out.data[i+2]=v;
                    out.data[i+3]=255;
                }
            }
        } else {
            for(let i=0;i<src.data.length;i+=4){
                const r = src.data[i], g = src.data[i+1], b = src.data[i+2];
                let lumv = 0.2126*r + 0.7152*g + 0.0722*b;
                if(gray === 'none') lumv = (r+g+b)/3;
                let v;
                if(levels === 2){
                    v = lumv < threshold ? 0 : 255;
                } else {
                    const q = Math.round((levels - 1) * (lumv / 255));
                    v = (levels > 1) ? (q * (255 / (levels - 1))) : lumv;
                }
                out.data[i]=out.data[i+1]=out.data[i+2]=Math.max(0, Math.min(255, Math.round(v)));
                out.data[i+3]=255;
            }
        }

        ensureMask(w, h);
        if(transparentMask){
            for(let i=0;i<w*h;i++){
                if(transparentMask[i]){
                    out.data[i*4 + 3] = 0;
                }
            }
            
            // 调试：统计实际有多少像素被设置为透明
            if(debugMode) {
                let transparentCount = 0;
                let minTransX = w, maxTransX = -1, minTransY = h, maxTransY = -1;
                for(let y = 0; y < h; y++){
                    for(let x = 0; x < w; x++){
                        const idx = y * w + x;
                        if(transparentMask[idx]){
                            transparentCount++;
                            minTransX = Math.min(minTransX, x);
                            maxTransX = Math.max(maxTransX, x);
                            minTransY = Math.min(minTransY, y);
                            maxTransY = Math.max(maxTransY, y);
                        }
                    }
                }
                if(transparentCount > 0){
                    console.log('🎭 透明mask应用统计:');
                    console.log('   总透明像素数:', transparentCount);
                    console.log('   透明区域边界框:', {
                        minX: minTransX, maxX: maxTransX,
                        minY: minTransY, maxY: maxTransY,
                        width: maxTransX - minTransX + 1,
                        height: maxTransY - minTransY + 1
                    });
                }
            }
        }

        // Store processed image in outCanvas
        ctxOut.putImageData(out,0,0);
        
        // Now render the preview: show target output frame with processed image positioned by panOffset
        const ow = parseInt(outWidth.value) || 540;
        const oh = parseInt(outHeight.value) || 960;
        
        // Set srcCanvas to target output size
        srcCanvas.width = ow;
        srcCanvas.height = oh;
        
        const fillColor = backgroundFill.value || '#ffffff';

        // Fill with selected background color
        fillPreviewBackground(ctxSrc, ow, oh, fillColor);
        
        // Draw processed image at panOffset position
        // Clamp panOffset so image stays reasonably within bounds (allow partial visibility)
        const maxOffsetX = ow;
        const maxOffsetY = oh;
        const minOffsetX = -w;
        const minOffsetY = -h;
        panOffsetX = Math.max(minOffsetX, Math.min(maxOffsetX, panOffsetX));
        panOffsetY = Math.max(minOffsetY, Math.min(maxOffsetY, panOffsetY));
        
        // 调试：显示绘制信息
        if(debugMode) {
            console.log('🖼️ applyProcess 绘制信息:');
            console.log('   真实图片尺寸(outCanvas):', { width: w, height: h });
            console.log('   预览画布尺寸(srcCanvas):', { width: ow, height: oh });
            console.log('   真实图片在预览画布上的位置:', { x: panOffsetX, y: panOffsetY });
            console.log('   真实图片在预览画布上占据的区域:');
            console.log('      X: ', panOffsetX, '至', panOffsetX + w);
            console.log('      Y: ', panOffsetY, '至', panOffsetY + h);
        }
        
        ctxSrc.drawImage(outCanvas, 0, 0, w, h, panOffsetX, panOffsetY, w, h);
        
        // 调试：绘制绿色边框显示实际透明区域
        if(debugMode && transparentMask){
            const imageData = ctxOut.getImageData(0, 0, w, h);
            const data = imageData.data;
            let minVisX = w, maxVisX = -1, minVisY = h, maxVisY = -1;
            let visibleTransparentCount = 0;
            
            for(let y = 0; y < h; y++){
                for(let x = 0; x < w; x++){
                    const idx = y * w + x;
                    const alphaIdx = idx * 4 + 3;
                    // 检查outCanvas中实际的alpha值
                    if(data[alphaIdx] === 0){
                        visibleTransparentCount++;
                        minVisX = Math.min(minVisX, x);
                        maxVisX = Math.max(maxVisX, x);
                        minVisY = Math.min(minVisY, y);
                        maxVisY = Math.max(maxVisY, y);
                    }
                }
            }
            
            if(visibleTransparentCount > 0){
                if(debugMode) {
                    console.log('✅ 实际渲染的透明区域（从outCanvas读取）:');
                    console.log('   透明像素数:', visibleTransparentCount);
                    console.log('   边界框:', {
                        minX: minVisX, maxX: maxVisX,
                        minY: minVisY, maxY: maxVisY,
                        width: maxVisX - minVisX + 1,
                        height: maxVisY - minVisY + 1
                    });
                    
                    // 绘制绿色虚线边框显示所有透明区域
                    ctxSrc.strokeStyle = 'lime';
                    ctxSrc.lineWidth = 2;
                    ctxSrc.setLineDash([5, 5]); // 虚线模式
                    ctxSrc.strokeRect(
                        minVisX + panOffsetX,
                        minVisY + panOffsetY,
                        maxVisX - minVisX + 1,
                        maxVisY - minVisY + 1
                    );
                    ctxSrc.setLineDash([]); // 恢复实线模式
                    console.log('🟢 已在预览画布上绘制绿色虚线边框标记所有透明区域');
                }
            }
        }
        
        // Draw a subtle border around the output frame to indicate boundaries
        ctxSrc.strokeStyle = '#ccc';
        ctxSrc.lineWidth = 1;
        ctxSrc.strokeRect(0, 0, ow, oh);
    }

    function applyMagicWandAt(canvasX, canvasY){
        const w = outCanvas.width;
        const h = outCanvas.height;
        if(w === 0 || h === 0) return;

        const imgX = Math.floor(canvasX - panOffsetX);
        const imgY = Math.floor(canvasY - panOffsetY);
        
        // 调试信息：打印魔法棒点击坐标
        if(debugMode) {
            console.log('=== 魔法棒点击调试信息 ===');
            console.log('1. 点击预览图片位置:', { x: canvasX, y: canvasY });
            console.log('2. 换算成真实图片位置:', { x: imgX, y: imgY });
            console.log('   真实图片尺寸:', { width: w, height: h });
            console.log('   预览图片尺寸:', { width: srcCanvas.width, height: srcCanvas.height });
            console.log('   平移偏移量:', { panOffsetX, panOffsetY });
        }
        
        if(imgX < 0 || imgY < 0 || imgX >= w || imgY >= h) {
            if(debugMode) {
                console.log('   ❌ 点击位置超出真实图片范围，忽略');
                console.log('========================');
            }
            return;
        }

        ensureMask(w, h);
        
        // 检查点击的像素是否已经透明，如果是则不处理
        const pixelIndex = imgY * w + imgX;
        const isAlreadyTransparent = transparentMask && transparentMask[pixelIndex] === 1;
        
        if(isAlreadyTransparent) {
            if(debugMode) {
                console.log('=== 魔法棒点击调试信息 ===');
                console.log('⚠️ 点击的区域已经是透明的，忽略此次操作');
                console.log('   点击位置:', { x: imgX, y: imgY });
                console.log('========================');
            }
            return; // 不处理已透明区域
        }
        
        const prevMask = transparentMask ? transparentMask.slice() : null;
        const toleranceVal = parseInt(magicTolerance.value, 10);
        const tolerance = Number.isFinite(toleranceVal) ? toleranceVal : 32;
        const tolSq = tolerance * tolerance;

        const imageData = ctxOut.getImageData(0,0,w,h);
        const data = imageData.data;
        const targetIdx = (imgY * w + imgX) * 4;
        const targetR = data[targetIdx];
        const targetG = data[targetIdx + 1];
        const targetB = data[targetIdx + 2];
        const targetA = data[targetIdx + 3];
        
        if(debugMode) {
            console.log('3. 魔法棒处理参数:');
            console.log('   处理点在真实图片的位置:', { x: imgX, y: imgY });
            console.log('   outCanvas原始像素颜色 RGBA:', { r: targetR, g: targetG, b: targetB, a: targetA });
            console.log('   ⚠️ 注意: 魔法棒基于outCanvas原始颜色，不是用户看到的预览');
            console.log('   容差值:', tolerance);
            
            // 检查周围像素的颜色，帮助理解为什么某些区域没有被选中
            console.log('   周围像素颜色采样:');
            const sampleOffsets = [
                {dx: 0, dy: -1, label: '上'},
                {dx: 1, dy: 0, label: '右'},
                {dx: 0, dy: 1, label: '下'},
                {dx: -1, dy: 0, label: '左'}
            ];
            sampleOffsets.forEach(({dx, dy, label}) => {
                const sx = imgX + dx;
                const sy = imgY + dy;
                if(sx >= 0 && sx < w && sy >= 0 && sy < h){
                    const sidx = (sy * w + sx) * 4;
                    const sr = data[sidx];
                    const sg = data[sidx + 1];
                    const sb = data[sidx + 2];
                    const dr = sr - targetR;
                    const dg = sg - targetG;
                    const db = sb - targetB;
                    const distSq = dr*dr + dg*dg + db*db;
                    const dist = Math.sqrt(distSq);
                    const withinTolerance = distSq <= tolSq;
                    console.log(`     ${label}: RGB(${sr},${sg},${sb}) 距离=${dist.toFixed(1)} ${withinTolerance ? '✓在容差内' : '✗超出容差'}`);
                }
            });
            
            console.log('========================');
        }

        const visited = new Uint8Array(w * h);
        const stack = [[imgX, imgY]];
        let changed = false;
        let pixelsProcessed = 0;
        let pixelsMarked = 0;
        let pixelsRejected = 0; // 访问了但因超出容差被拒绝的像素
        let minX = imgX, maxX = imgX, minY = imgY, maxY = imgY;

        while(stack.length){
            const [x, y] = stack.pop();
            if(x < 0 || y < 0 || x >= w || y >= h) continue;
            const idx = y * w + x;
            if(visited[idx]) continue;
            visited[idx] = 1;
            pixelsProcessed++;
            const dataIdx = idx * 4;
            const dr = data[dataIdx] - targetR;
            const dg = data[dataIdx + 1] - targetG;
            const db = data[dataIdx + 2] - targetB;
            const distSq = dr*dr + dg*dg + db*db;
            if(distSq > tolSq){
                pixelsRejected++;
                continue;
            }
            if(!transparentMask[idx]){
                transparentMask[idx] = 1;
                changed = true;
                pixelsMarked++;
                minX = Math.min(minX, x);
                maxX = Math.max(maxX, x);
                minY = Math.min(minY, y);
                maxY = Math.max(maxY, y);
            }
            stack.push([x + 1, y], [x - 1, y], [x, y + 1], [x, y - 1]);
        }

        if(debugMode) {
            console.log('4. 魔法棒执行结果:');
            console.log('   访问像素数:', pixelsProcessed);
            console.log('   标记为透明的像素数:', pixelsMarked);
            console.log('   因超出容差被拒绝的像素数:', pixelsRejected);
            if(pixelsMarked > 0){
                console.log('   影响区域在真实图片中的范围:', {
                    minX, maxX, minY, maxY,
                    width: maxX - minX + 1,
                    height: maxY - minY + 1
                });
                const previewMinX = minX + panOffsetX;
                const previewMaxX = maxX + panOffsetX;
                const previewMinY = minY + panOffsetY;
                const previewMaxY = maxY + panOffsetY;
                console.log('   ⚠️ 影响区域在预览画布(逻辑像素)上的位置:', {
                    previewMinX, previewMaxX, previewMinY, previewMaxY
                });
                
                // 计算在浏览器屏幕上的实际显示位置
                const rect = srcCanvas.getBoundingClientRect();
                const scaleX = rect.width / srcCanvas.width;
                const scaleY = rect.height / srcCanvas.height;
                const screenMinX = Math.round(rect.left + previewMinX * scaleX);
                const screenMaxX = Math.round(rect.left + previewMaxX * scaleX);
                const screenMinY = Math.round(rect.top + previewMinY * scaleY);
                const screenMaxY = Math.round(rect.top + previewMaxY * scaleY);
                
                console.log('   🖥️ 影响区域在浏览器屏幕上的位置(CSS像素):', {
                    screenMinX, screenMaxX, screenMinY, screenMaxY,
                    '相对canvas左边缘': previewMinX * scaleX + 'px 至 ' + previewMaxX * scaleX + 'px'
                });
                console.log('   🎯 用户应该在预览画布上看到消除的区域:');
                console.log('      canvas逻辑像素 X范围:', previewMinX, '至', previewMaxX);
                console.log('      canvas逻辑像素 Y范围:', previewMinY, '至', previewMaxY);
                console.log('      相对canvas左边缘(显示像素):', (previewMinX * scaleX).toFixed(1) + 'px 至 ' + (previewMaxX * scaleX).toFixed(1) + 'px');
                console.log('   ❓ 如果您看到的区域与上述范围不符，说明存在坐标转换BUG！');
            } else {
                console.log('   ⚠️ 没有像素被标记为透明');
            }
            console.log('   是否触发重绘:', changed);
            
            // 提供建议
            if(pixelsMarked < 100 && pixelsRejected > pixelsMarked * 2){
                console.log('   💡 建议: 标记的像素很少，但拒绝的像素较多。可能需要增加容差值。');
            } else if(pixelsMarked === 0 && pixelsRejected > 0){
                console.log('   💡 建议: 没有像素被标记，但有', pixelsRejected, '个像素因颜色差异被拒绝。请增加容差值。');
            }
            
            console.log('========================');
            console.log('💡 提示: 如果魔法棒行为不符合预期，可能是因为:');
            console.log('   1. 点击了已透明区域（看不到但算法仍处理原始颜色）');
            console.log('   2. 容差值设置不合适（当前:', tolerance, '）');
            console.log('   3. 图像预览和实际处理图存在差异');
            console.log('   4. 图像经过抖动/二值化处理，相邻像素颜色差异大');
            console.log('========================\n');
        }

        if(changed){
            if(prevMask) pushMaskHistory(prevMask);
            // 在重绘前保存影响区域和点击位置，用于绘制调试标记
            const debugInfo = pixelsMarked > 0 ? {
                rect: {
                    x: minX + panOffsetX,
                    y: minY + panOffsetY,
                    width: maxX - minX + 1,
                    height: maxY - minY + 1
                },
                clickPoint: {
                    x: imgX + panOffsetX,
                    y: imgY + panOffsetY
                }
            } : null;
            applyProcess();
            
            // applyProcess会重绘画布，现在在上面绘制调试标记
            if(debugMode && debugInfo){
                // 绘制红色边框标记本次操作影响区域
                ctxSrc.strokeStyle = 'red';
                ctxSrc.lineWidth = 3;
                ctxSrc.strokeRect(debugInfo.rect.x, debugInfo.rect.y, debugInfo.rect.width, debugInfo.rect.height);
                
                // 绘制青色十字标记点击位置（canvas逻辑坐标）
                ctxSrc.strokeStyle = 'cyan';
                ctxSrc.lineWidth = 2;
                const crossSize = 15;
                const cx = debugInfo.clickPoint.x;
                const cy = debugInfo.clickPoint.y;
                ctxSrc.beginPath();
                ctxSrc.moveTo(cx - crossSize, cy);
                ctxSrc.lineTo(cx + crossSize, cy);
                ctxSrc.moveTo(cx, cy - crossSize);
                ctxSrc.lineTo(cx, cy + crossSize);
                ctxSrc.stroke();
                
                // 绘制点击位置的圆圈
                ctxSrc.beginPath();
                ctxSrc.arc(cx, cy, 8, 0, Math.PI * 2);
                ctxSrc.stroke();
                
                // 添加文字标签
                ctxSrc.fillStyle = 'cyan';
                ctxSrc.font = '12px monospace';
                ctxSrc.fillText(`(${Math.round(cx)},${Math.round(cy)})`, cx + 12, cy - 12);
                
                console.log('🔴 已在预览画布上绘制红色边框标记本次影响区域:', debugInfo.rect);
                console.log('🔵 已在预览画布上绘制青色十字标记点击位置:', debugInfo.clickPoint);
                console.log('');
                console.log('⚠️⚠️⚠️ 如果青色十字不在你实际点击的位置，说明坐标转换有BUG！');
                console.log('请检查canvas的CSS样式、transform、position等属性。');
            }
        }
    }

    function undoMagicSelection(){
        if(maskHistory.length === 0) return;
        const previous = maskHistory.pop();
        transparentMask = previous;
        maskWidth = outCanvas.width;
        maskHeight = outCanvas.height;
        updateUndoState();
        applyProcess();
    }

    // Real-time preview: update when controls change
    ditherMode.addEventListener('change', applyProcess);
    grayscaleMode.addEventListener('change', applyProcess);
    grayLevels.addEventListener('change', applyProcess);
    backgroundFill.addEventListener('change', applyProcess);
    outWidth.addEventListener('input', drawSrc);
    outHeight.addEventListener('input', drawSrc);
    resetSize.addEventListener('click', (e)=>{ e.preventDefault(); outWidth.value='540'; outHeight.value='960'; panOffsetX=0; panOffsetY=0; drawSrc(); });

    if(applyBtn){
        applyBtn.addEventListener('click', (e)=>{ applyProcess(); });
    }

    // Mouse drag to position image within target output frame
    srcCanvas.addEventListener('mousedown', (e)=>{
        if(!img || img.width===0) return;
        if(isMagicMode){
            e.preventDefault();
            
            // 保存原始浏览器坐标用于调试
            const rawClientX = e.clientX;
            const rawClientY = e.clientY;
            
            const { x, y } = getCanvasCoords(e);
            if(debugMode) console.log('🖱️ 鼠标点击事件 - 预览画布坐标:', { x, y });
            
            // 在canvas上绘制一个临时标记显示浏览器认为的点击位置
            // 直接在canvas坐标系中绘制，不经过任何转换
            const rect = srcCanvas.getBoundingClientRect();
            const directX = (rawClientX - rect.left) / rect.width * srcCanvas.width;
            const directY = (rawClientY - rect.top) / rect.height * srcCanvas.height;
            
            if(debugMode) {
                console.log('🎯 直接坐标计算（备用方法）:', { directX, directY });
                console.log('   与getCanvasCoords的差异:', { 
                    deltaX: Math.abs(directX - x),
                    deltaY: Math.abs(directY - y)
                });
            }
            
            applyMagicWandAt(x, y);
            return;
        }
        isDragging = true;
        const rect = srcCanvas.getBoundingClientRect();
        dragStartX = e.clientX - rect.left;
        dragStartY = e.clientY - rect.top;
        setCanvasCursor();
    });
    
    srcCanvas.addEventListener('mousemove', (e)=>{
        if(isMagicMode || !isDragging) return;
        const rect = srcCanvas.getBoundingClientRect();
        const mx = e.clientX - rect.left;
        const my = e.clientY - rect.top;
        // Calculate delta in canvas coordinates
        const scaleX = srcCanvas.width / rect.width;
        const scaleY = srcCanvas.height / rect.height;
        const dx = (mx - dragStartX) * scaleX;
        const dy = (my - dragStartY) * scaleY;
        panOffsetX += dx;
        panOffsetY += dy;
        dragStartX = mx;
        dragStartY = my;
        applyProcess();
    });
    
    srcCanvas.addEventListener('mouseup', ()=>{ 
        isDragging = false; 
        setCanvasCursor();
    });
    
    srcCanvas.addEventListener('mouseleave', ()=>{ 
        isDragging = false;
        setCanvasCursor();
    });
    
    srcCanvas.addEventListener('mouseenter', ()=>{
        setCanvasCursor();
    });
    // Touch events for mobile: map single-finger drag to pan
    srcCanvas.addEventListener('touchstart', (e)=>{
        if(!img || img.width===0) return;
        // only handle single-touch
        if(e.touches.length !== 1) return;
        // prevent page scrolling while interacting with canvas
        e.preventDefault();
        const t = e.touches[0];
        if(isMagicMode){
            const { x, y } = getCanvasCoords(t);
            console.log('👆 触摸事件 - 预览画布坐标:', { x, y });
            applyMagicWandAt(x, y);
            return;
        }
        isDragging = true;
        const rect = srcCanvas.getBoundingClientRect();
        dragStartX = t.clientX - rect.left;
        dragStartY = t.clientY - rect.top;
        setCanvasCursor();
    }, { passive: false });

    srcCanvas.addEventListener('touchmove', (e)=>{
        if(!img || img.width===0) return;
        if(isMagicMode || !isDragging) return;
        if(e.touches.length !== 1) return;
        e.preventDefault();
        const t = e.touches[0];
        const rect = srcCanvas.getBoundingClientRect();
        const mx = t.clientX - rect.left;
        const my = t.clientY - rect.top;
        const scaleX = srcCanvas.width / rect.width;
        const scaleY = srcCanvas.height / rect.height;
        const dx = (mx - dragStartX) * scaleX;
        const dy = (my - dragStartY) * scaleY;
        panOffsetX += dx;
        panOffsetY += dy;
        dragStartX = mx;
        dragStartY = my;
        applyProcess();
    }, { passive: false });

    srcCanvas.addEventListener('touchend', (e)=>{
        // stop dragging on touch end
        isDragging = false;
        setCanvasCursor();
    });

    srcCanvas.addEventListener('touchcancel', (e)=>{
        isDragging = false;
        setCanvasCursor();
    });
    setCanvasCursor();

    downloadBtn.addEventListener('click', ()=>{
        if(outCanvas.width===0) return;
        const ow = parseInt(outWidth.value) || 540;
        const oh = parseInt(outHeight.value) || 960;
        
        // Create final output canvas at exact target size
        const finalCanvas = document.createElement('canvas');
        finalCanvas.width = ow;
        finalCanvas.height = oh;
        const fctx = finalCanvas.getContext('2d');
        
        // Fill with selected background color to match preview (allow transparent)
        const exportFill = backgroundFill.value || '#ffffff';
        if(exportFill === 'transparent'){
            fctx.clearRect(0, 0, ow, oh);
        } else {
            fctx.fillStyle = exportFill;
            fctx.fillRect(0, 0, ow, oh);
        }
        
        // Draw processed image at the same position as shown in preview
        fctx.drawImage(outCanvas, 0, 0, outCanvas.width, outCanvas.height, 
                       panOffsetX, panOffsetY, outCanvas.width, outCanvas.height);
        
        const a = document.createElement('a');
        a.href = finalCanvas.toDataURL('image/png');
        a.download = 'pichandle_result.png';
        document.body.appendChild(a);
        a.click();
        a.remove();
    });

    // initial labels
    updateScaleLabel(); updateThresholdLabel(); updateMagicToleranceLabel(); updateUndoState();
    
    // Sync right panel card height with left panel card height on desktop
    function syncCardHeights() {
        if (window.innerWidth >= 769) {
            const leftCard = document.querySelector('.panel-left .card');
            const rightCard = document.querySelector('.panel-right .card');
            if (leftCard && rightCard) {
                rightCard.style.height = leftCard.offsetHeight + 'px';
            }
        } else {
            const rightCard = document.querySelector('.panel-right .card');
            if (rightCard) {
                rightCard.style.height = '';
            }
        }
    }
    
    // Run on load and resize
    window.addEventListener('load', syncCardHeights);
    window.addEventListener('resize', syncCardHeights);
    // Also sync after image loads (may change left card height)
    const originalDrawSrc = drawSrc;
    drawSrc = function() {
        originalDrawSrc();
        setTimeout(syncCardHeights, 0);
    };
})();
