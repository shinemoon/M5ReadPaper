// Barcode component helper
(function(window){
  const BarcodeComponent = {
    getConfigSchema() {
      return {
        type: 'barcode',
        text: '0123456789',
        rotation: 0
      };
    },
    renderPreview(comp, container) {
      if (!container) return;
      container.innerHTML = '';
      const el = document.createElement('div');
      el.className = 'barcode-render';
      el.textContent = comp.text || '0123456789';
      container.appendChild(el);
    }
  };
  window.BarcodeComponent = BarcodeComponent;
})(window);
