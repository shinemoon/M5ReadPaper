// HuangLi component helper for webapp pages
// Provides minimal interface similar to other components so it can be plugged into editors.
(function(window){
  const HuangLi = {
    // 返回用于编辑器的配置字段说明（与普通文本一致）
    getConfigSchema() {
      return {
        type: 'huangli',
        text: '',
        fontSize: 24,
        fontFamily: '',
        textColor: 0,
        bgColor: 15,
        align: 'left',
        rotation: 0
      };
    },

    // 渲染预览（在编辑器中可调用），container 为 DOM 节点
    renderPreview(comp, container) {
      if (!container) return;
      container.innerHTML = '';
      const el = document.createElement('div');
      el.className = 'huangli-render';
      el.textContent = comp.text || '黄历：示例信息';
      el.style.fontSize = (comp.fontSize || 24) + 'px';
      el.style.color = `rgb(${(comp.textColor||0)*17}, ${(comp.textColor||0)*17}, ${(comp.textColor||0)*17})`;
      el.style.background = comp.bgColor === 'transparent' ? 'transparent' : `rgb(${(comp.bgColor||0)*17}, ${(comp.bgColor||0)*17}, ${(comp.bgColor||0)*17})`;
      el.style.transform = `rotate(${comp.rotation||0}deg)`;
      container.appendChild(el);
    }
  };

  window.HuangLiComponent = HuangLi;
})(window);
