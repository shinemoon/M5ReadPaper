// Clock component helper
(function(window){
  const ClockComponent = {
    getConfigSchema() {
      return {
        type: 'clock',
        format: 'HH:mm',
        fontSize: 36,
        textColor: 0,
        bgColor: 'transparent',
        rotation: 0
      };
    },
    renderPreview(comp, container) {
      if (!container) return;
      container.innerHTML = '';
      const el = document.createElement('div');
      el.className = 'clock-render';
      el.textContent = (new Date()).toLocaleTimeString();
      el.style.fontSize = (comp.fontSize || 36) + 'px';
      el.style.color = `rgb(${(comp.textColor||0)*17}, ${(comp.textColor||0)*17}, ${(comp.textColor||0)*17})`;
      container.appendChild(el);
    }
  };
  window.ClockComponent = ClockComponent;
})(window);
