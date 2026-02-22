// Dynamic text (普通文本) component helper
(function(window){
  const DynamicTextComponent = {
    getConfigSchema() {
      return {
        type: 'dynamic_text',
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
      el.className = 'dynamic-text-render';
      el.textContent = comp.text || '普通文本（动态）';
      el.style.fontSize = (comp.fontSize || 24) + 'px';
      el.style.color = `rgb(${(comp.textColor||0)*17}, ${(comp.textColor||0)*17}, ${(comp.textColor||0)*17})`;
      el.style.background = comp.bgColor === 'transparent' ? 'transparent' : `rgb(${(comp.bgColor||0)*17}, ${(comp.bgColor||0)*17}, ${(comp.bgColor||0)*17})`;
      el.style.textAlign = comp.align || 'left';
      el.style.transform = `rotate(${comp.rotation||0}deg)`;
      container.appendChild(el);
    }
  };
  window.DynamicTextComponent = DynamicTextComponent;
})(window);
