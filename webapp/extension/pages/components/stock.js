// Stock component helper
(function(window){
  const StockComponent = {
    getConfigSchema() {
      return {
        type: 'stock',
        text: '000001.SZ;600519.SH',
        fontSize: 24,
        fontFamily: '',
        textColor: 0,
        bgColor: 'transparent',
        align: 'left',
        rotation: 0
      };
    },
    renderPreview(comp, container) {
      if (!container) return;
      container.innerHTML = '';
      // 简化预览：不渲染详细的 stock-render，仅显示占位文本
      const placeholder = document.createElement('div');
      placeholder.className = 'component-stock-placeholder';
      placeholder.textContent = '股票组件预览';
      placeholder.style.fontSize = (comp.fontSize || 24) + 'px';
      placeholder.style.color = `rgb(${(comp.textColor||0)*17}, ${(comp.textColor||0)*17}, ${(comp.textColor||0)*17})`;
      container.appendChild(placeholder);
    }
  };
  window.StockComponent = StockComponent;
})(window);
