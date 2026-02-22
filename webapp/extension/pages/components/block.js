// Block component module for preview hooks (optional)
(function(){
  window.BlockComponent = {
    renderPreview: function(comp, container) {
      // Simple inline preview: show a small rectangle with chosen gray and opacity
      container.innerHTML = '';
      const w = 120;
      const h = Math.max(40, Math.round(40 * (comp.height || 1)));
      const canvas = document.createElement('canvas');
      canvas.width = w;
      canvas.height = h;
      canvas.style.width = w + 'px';
      canvas.style.height = h + 'px';
      const ctx = canvas.getContext('2d');
      // 兼容性处理：确保 blockColor 为 0..15，opacity 为 0..1
      let blockIdx = comp.blockColor;
      if (blockIdx === undefined && comp.config && comp.config.blockColor !== undefined) blockIdx = comp.config.blockColor;
      blockIdx = parseInt(blockIdx);
      if (isNaN(blockIdx)) blockIdx = 0;
      blockIdx = Math.max(0, Math.min(15, blockIdx));
      const gray = blockIdx * 17;

      let opacity = comp.opacity;
      if (opacity === undefined && comp.config && comp.config.opacity !== undefined) opacity = comp.config.opacity;
      if (opacity === undefined) opacity = 1;
      opacity = parseFloat(opacity);
      if (isNaN(opacity)) opacity = 1;
      opacity = Math.max(0, Math.min(1, opacity));
      ctx.fillStyle = `rgba(${gray}, ${gray}, ${gray}, ${opacity})`;
      ctx.fillRect(0, 0, w, h);
      container.appendChild(canvas);
    }
  };
})();
