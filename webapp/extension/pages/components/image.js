// Image component helper
(function(window){
  const ImageComponent = {
    getConfigSchema() {
      return {
        type: 'image',
        src: '',
        rotation: 0
      };
    },
    renderPreview(comp, container) {
      if (!container) return;
      container.innerHTML = '';
      const el = document.createElement('img');
      el.className = 'image-render';
      el.src = comp.src || '';
      el.alt = '图片预览';
      el.style.maxWidth = '100%';
      container.appendChild(el);
    }
  };
  window.ImageComponent = ImageComponent;
})(window);
