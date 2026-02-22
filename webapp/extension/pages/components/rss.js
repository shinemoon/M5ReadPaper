// RSS component helper
(function(window){
  const RssComponent = {
    getConfigSchema() {
      return {
        type: 'rss',
        text: 'https://example.com/feed.xml',
        fontSize: 24,
        fontFamily: '',
        textColor: 0,
        bgColor: 'transparent',
        align: 'left',
        rotation: 0,
        margin: 10
      };
    },
    renderPreview(comp, container) {
      if (!container) return;
      container.innerHTML = '';
      const el = document.createElement('div');
      el.className = 'rss-render';
      el.textContent = comp.text || 'RSS URL';
      el.style.fontSize = (comp.fontSize || 24) + 'px';
      el.style.color = `rgb(${(comp.textColor||0)*17}, ${(comp.textColor||0)*17}, ${(comp.textColor||0)*17})`;
      container.appendChild(el);
    }
  };
  window.RssComponent = RssComponent;
})(window);
