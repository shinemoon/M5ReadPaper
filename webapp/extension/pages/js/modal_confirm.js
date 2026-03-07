// modal_confirm.js — 全局优雅确认弹窗替代原生 window.confirm
(function(){
  if(window.niceConfirm) return; // 避免重复定义
  function buildModal(){
    const overlay = document.createElement('div');
    overlay.className = 'rp-confirm-overlay';
    overlay.style.cssText = 'position:fixed;inset:0;z-index:9999;display:flex;align-items:center;justify-content:center;font-family:inherit;';

    const backdrop = document.createElement('div');
    backdrop.className='rp-confirm-backdrop';
    backdrop.style.cssText='position:absolute;inset:0;background:rgba(0,0,0,.42);backdrop-filter:blur(2px);';

    const box = document.createElement('div');
    box.className='rp-confirm-box card';
    box.style.cssText='position:relative;max-width:440px;width:92%;background:#fff;border-radius:10px;box-shadow:0 10px 28px rgba(0,0,0,.22);padding:1.1rem .95rem .9rem;display:flex;flex-direction:column;gap:.9rem;';

    const titleEl = document.createElement('div');
    titleEl.className='rp-confirm-title';
    titleEl.style.cssText='font-size:16px;font-weight:600;line-height:1.4;display:flex;align-items:center;gap:.5rem;';
    const icon = document.createElement('span');
    icon.textContent='⚠️'; icon.style.fontSize='18px'; icon.style.lineHeight='1';
    titleEl.appendChild(icon);
    const titleText = document.createElement('span'); titleText.textContent='请确认'; titleEl.appendChild(titleText);

    const msgEl = document.createElement('div');
    msgEl.className='rp-confirm-message';
    msgEl.style.cssText='white-space:pre-wrap;font-size:14px;line-height:1.6;max-height:40vh;overflow:auto;padding-right:4px;';

    const actions = document.createElement('div');
    actions.style.cssText='display:flex;justify-content:flex-end;gap:.6rem;margin-top:.4rem;';
    const btnCancel = document.createElement('button');
    btnCancel.type='button'; btnCancel.className='button outline'; btnCancel.textContent='取消';
    const btnOk = document.createElement('button');
    btnOk.type='button'; btnOk.className='button primary'; btnOk.textContent='确定';
    actions.appendChild(btnCancel); actions.appendChild(btnOk);

    // accessibility roles
    box.setAttribute('role','dialog');
    box.setAttribute('aria-modal','true');
    box.setAttribute('aria-labelledby','rp-confirm-title');
    box.setAttribute('aria-describedby','rp-confirm-message');
    titleEl.id='rp-confirm-title'; msgEl.id='rp-confirm-message';

    box.appendChild(titleEl); box.appendChild(msgEl); box.appendChild(actions);
    overlay.appendChild(backdrop); overlay.appendChild(box);
    return {overlay, backdrop, box, titleText, msgEl, btnOk, btnCancel};
  }

  // focus trap minimal
  function trapFocus(container){
    const focusables = Array.from(container.querySelectorAll('button,[href],input,textarea,select,[tabindex]:not([tabindex="-1"])'));
    if(!focusables.length) return;
    const first = focusables[0]; const last = focusables[focusables.length-1];
    function onKey(e){
      if(e.key==='Tab'){
        if(e.shiftKey && document.activeElement===first){ e.preventDefault(); last.focus(); }
        else if(!e.shiftKey && document.activeElement===last){ e.preventDefault(); first.focus(); }
      }
    }
    container.addEventListener('keydown', onKey);
    return ()=> container.removeEventListener('keydown', onKey);
  }

  window.niceConfirm = function(message, options={}){
    return new Promise(resolve=>{
      const {overlay, box, titleText, msgEl, btnOk, btnCancel} = buildModal();
      const title = options.title || '请确认';
      titleText.textContent = title;
      msgEl.textContent = String(message||'');
      document.body.appendChild(overlay);

      // enter / esc handlers
      function cleanup(val){ try{ overlay.remove(); }catch(e){} document.removeEventListener('keydown', onKey); if(untrap) untrap(); resolve(!!val); }
      function onKey(e){
        if(e.key==='Escape'){ cleanup(false); }
        else if(e.key==='Enter'){ if(document.activeElement===btnCancel){ cleanup(false); } else { cleanup(true); } }
      }
      document.addEventListener('keydown', onKey);
      btnCancel.onclick = ()=> cleanup(false);
      btnOk.onclick = ()=> cleanup(true);
      const untrap = trapFocus(box);
      // initial focus
      setTimeout(()=> btnOk.focus(), 10);
    });
  };

  // 提供兼容旧接口 showConfirm
  window.showConfirm = function(message, title){ return window.niceConfirm(message,{title}); };

  // 输入弹窗：返回 Promise<string|null>，用户取消时 resolve(null)
  window.nicePrompt = function(message, defaultValue='', options={}) {
    return new Promise(resolve => {
      const overlay = document.createElement('div');
      overlay.style.cssText = 'position:fixed;inset:0;z-index:9999;display:flex;align-items:center;justify-content:center;font-family:inherit;';
      const backdrop = document.createElement('div');
      backdrop.style.cssText = 'position:absolute;inset:0;background:rgba(0,0,0,.42);backdrop-filter:blur(2px);';
      const box = document.createElement('div');
      box.className = 'card';
      box.style.cssText = 'position:relative;max-width:440px;width:92%;background:#fff;border-radius:10px;box-shadow:0 10px 28px rgba(0,0,0,.22);padding:1.1rem .95rem .9rem;display:flex;flex-direction:column;gap:.9rem;';
      box.setAttribute('role','dialog'); box.setAttribute('aria-modal','true');

      const titleEl = document.createElement('div');
      titleEl.style.cssText = 'font-size:16px;font-weight:600;line-height:1.4;display:flex;align-items:center;gap:.5rem;';
      titleEl.innerHTML = '<span style="font-size:18px">✏️</span><span>' + (options.title || '输入') + '</span>';

      const msgEl = document.createElement('div');
      msgEl.style.cssText = 'font-size:14px;line-height:1.6;white-space:pre-wrap;';
      msgEl.textContent = message || '';

      const input = document.createElement('input');
      input.type = 'text';
      input.value = defaultValue || '';
      input.style.cssText = 'width:100%;box-sizing:border-box;padding:.4rem .6rem;font-size:14px;border:1px solid #ccc;border-radius:6px;outline:none;';

      const actions = document.createElement('div');
      actions.style.cssText = 'display:flex;justify-content:flex-end;gap:.6rem;margin-top:.2rem;';
      const btnCancel = document.createElement('button');
      btnCancel.type = 'button'; btnCancel.className = 'button outline'; btnCancel.textContent = '取消';
      const btnOk = document.createElement('button');
      btnOk.type = 'button'; btnOk.className = 'button primary'; btnOk.textContent = '确定';
      actions.appendChild(btnCancel); actions.appendChild(btnOk);

      box.appendChild(titleEl); box.appendChild(msgEl); box.appendChild(input); box.appendChild(actions);
      overlay.appendChild(backdrop); overlay.appendChild(box);
      document.body.appendChild(overlay);

      function cleanup(val) { try { overlay.remove(); } catch(e) {} resolve(val); }
      btnCancel.onclick = () => cleanup(null);
      btnOk.onclick = () => { const v = input.value.trim(); cleanup(v.length ? v : null); };
      overlay.addEventListener('keydown', e => {
        if (e.key === 'Escape') { cleanup(null); }
        else if (e.key === 'Enter' && document.activeElement !== btnCancel) {
          const v = input.value.trim(); cleanup(v.length ? v : null);
        }
      });
      setTimeout(() => { input.focus(); input.select(); }, 10);
    });
  };
})();
