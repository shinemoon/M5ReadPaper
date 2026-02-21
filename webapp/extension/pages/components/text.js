// Text component helper
(function(window){
  const TextComponent = {
    getConfigSchema() {
      return {
        type: 'text',
        text: '示例文本',
        fontSize: 24,
        fontFamily: 'Arial',
        textColor: 0,
        bgColor: 'transparent',
        align: 'center',
        rotation: 0
      };
    },
    renderPreview(comp, container) {
      if (!container) return;
      container.innerHTML = '';
      const el = document.createElement('div');
      el.className = 'text-render';
      el.textContent = comp.text || '预渲染文本';
      el.style.fontSize = (comp.fontSize || 24) + 'px';
      el.style.color = `rgb(${(comp.textColor||0)*17}, ${(comp.textColor||0)*17}, ${(comp.textColor||0)*17})`;
      el.style.background = comp.bgColor === 'transparent' ? 'transparent' : `rgb(${(comp.bgColor||0)*17}, ${(comp.bgColor||0)*17}, ${(comp.bgColor||0)*17})`;
      el.style.textAlign = comp.align || 'center';
      el.style.transform = `rotate(${comp.rotation||0}deg)`;
      container.appendChild(el);
    }
  };
  window.TextComponent = TextComponent;
})(window);
