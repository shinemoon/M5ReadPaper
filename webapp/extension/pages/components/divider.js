// Divider component helper
(function(window){
  const DividerComponent = {
    getConfigSchema() {
      return {
        type: 'divider',
        lineColor: 0,
        lineStyle: 'solid',
        lineWidth: 2,
        rotation: 0
      };
    },
    renderPreview(comp, container) {
      if (!container) return;
      container.innerHTML = '';
      const el = document.createElement('div');
      el.className = 'divider-render';
      el.style.height = (comp.lineWidth || 2) + 'px';
      el.style.background = `rgb(${(comp.lineColor||0)*17}, ${(comp.lineColor||0)*17}, ${(comp.lineColor||0)*17})`;
      container.appendChild(el);
    }
  };
  window.DividerComponent = DividerComponent;
})(window);
