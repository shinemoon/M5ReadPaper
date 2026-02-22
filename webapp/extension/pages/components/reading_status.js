// Reading status component helper
(function(window){
  const ReadingStatusComponent = {
    getConfigSchema() {
      return {
        type: 'reading_status',
        text: '',
        fontSize: 24,
        fontFamily: '',
        textColor: 0,
        bgColor: 15,
        align: 'left',
        rotation: 0
      };
    },
    renderPreview(comp, container) {
      if (!container) return;
      container.innerHTML = '';
      const el = document.createElement('div');
      el.className = 'reading-status-render';
      el.textContent = comp.text || '阅读状态（示例）';
      el.style.fontSize = (comp.fontSize || 24) + 'px';
      el.style.color = `rgb(${(comp.textColor||0)*17}, ${(comp.textColor||0)*17}, ${(comp.textColor||0)*17})`;
      el.style.background = comp.bgColor === 'transparent' ? 'transparent' : `rgb(${(comp.bgColor||0)*17}, ${(comp.bgColor||0)*17}, ${(comp.bgColor||0)*17})`;
      el.style.textAlign = comp.align || 'left';
      el.style.transform = `rotate(${comp.rotation||0}deg)`;
      container.appendChild(el);
    }
  };
  window.ReadingStatusComponent = ReadingStatusComponent;
})(window);
