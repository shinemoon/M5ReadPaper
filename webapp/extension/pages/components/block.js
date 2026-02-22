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
      const gray = ((comp.blockColor !== undefined ? comp.blockColor : 0) * 17);
      const opacity = (comp.opacity !== undefined ? comp.opacity : 0.5);
      ctx.fillStyle = `rgba(${gray}, ${gray}, ${gray}, ${opacity})`;
      ctx.fillRect(0, 0, w, h);
      container.appendChild(canvas);
    }
  };
})();
