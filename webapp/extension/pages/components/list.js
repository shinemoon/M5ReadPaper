// List component helper
(function(window){
  const ListComponent = {
    getConfigSchema() {
      return {
        type: 'list',
        text: '项目1;项目2;项目3',
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
      el.className = 'list-render';
      const items = (comp.text || '').split(';').filter(Boolean);
      items.forEach(it => {
        const itemEl = document.createElement('div');
        itemEl.textContent = it;
        itemEl.style.marginBottom = (comp.margin || 10) + 'px';
        el.appendChild(itemEl);
      });
      el.style.fontSize = (comp.fontSize || 24) + 'px';
      el.style.color = `rgb(${(comp.textColor||0)*17}, ${(comp.textColor||0)*17}, ${(comp.textColor||0)*17})`;
      container.appendChild(el);
    }
  };
  window.ListComponent = ListComponent;
})(window);
