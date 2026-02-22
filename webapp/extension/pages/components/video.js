// Video component helper
(function(window){
  const VideoComponent = {
    getConfigSchema() {
      return {
        type: 'video',
        src: '',
        rotation: 0
      };
    },
    renderPreview(comp, container) {
      if (!container) return;
      container.innerHTML = '';
      const el = document.createElement('div');
      el.className = 'video-render';
      el.textContent = comp.src ? `视频: ${comp.src}` : '视频预览';
      container.appendChild(el);
    }
  };
  window.VideoComponent = VideoComponent;
})(window);
