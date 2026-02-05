// 加载并渲染更新内容
async function loadWhatsNew() {
    try {
        const response = await fetch('../pages/whatsnew.json');
        if (!response.ok) {
            throw new Error('Failed to load whatsnew.json');
        }
        const data = await response.json();
        
        // 渲染更新内容
        const updateBody = document.getElementById('updateBody');
        updateBody.innerHTML = '';
        
        // 渲染每个更新section
        if (data.sections && Array.isArray(data.sections)) {
            data.sections.forEach(section => {
                const sectionDiv = document.createElement('div');
                sectionDiv.className = 'update-section';
                
                // 标题
                const title = document.createElement('h2');
                title.textContent = `${section.icon || ''} ${section.title}`;
                sectionDiv.appendChild(title);
                
                // 更新列表
                if (section.updates && section.updates.length > 0) {
                    const ul = document.createElement('ul');
                    ul.className = 'update-list';
                    
                    section.updates.forEach(update => {
                        const li = document.createElement('li');
                        li.innerHTML = `<strong>${update.category}</strong> - ${update.description}`;
                        ul.appendChild(li);
                    });
                    
                    sectionDiv.appendChild(ul);
                }
                
                updateBody.appendChild(sectionDiv);
            });
        }
        
        // 渲染公告（如果有）
        if (data.announcement) {
            const announcementDiv = document.createElement('div');
            announcementDiv.className = 'update-section';
            
            const title = document.createElement('h2');
            title.textContent = `${data.announcement.icon || '📰'} ${data.announcement.title || '消息提醒'}`;
            announcementDiv.appendChild(title);
            
            const ul = document.createElement('ul');
            ul.className = 'update-list';
            
            data.announcement.items.forEach(item => {
                const li = document.createElement('li');
                li.innerHTML = `<strong>${item.title}</strong> - ${item.content}`;
                ul.appendChild(li);
            });
            
            announcementDiv.appendChild(ul);
            updateBody.appendChild(announcementDiv);
        }
        
        // 更新固件信息
        const firmwareInfo = document.getElementById('firmwareInfo');
        if (firmwareInfo && data.latestFirmware) {
            firmwareInfo.innerHTML = `扩展对应最新固件: <b>${data.latestFirmware}</b>`;
        }
        
    } catch (error) {
        console.error('Failed to load whatsnew.json:', error);
        const updateBody = document.getElementById('updateBody');
        updateBody.innerHTML = '<div style="text-align:center; padding:40px; color:#d32f2f;">加载更新信息失败，请检查网络连接。</div>';
    }
}

// 获取并显示版本号
if (typeof chrome !== 'undefined' && chrome.runtime) {
    const manifest = chrome.runtime.getManifest();
    document.getElementById('versionBadge').textContent = 'v' + manifest.version;
}

// 页面加载时获取更新内容
document.addEventListener('DOMContentLoaded', loadWhatsNew);


// "稍后查看" - 关闭当前标签页
document.getElementById('btnLater').addEventListener('click', () => {
    if (typeof chrome !== 'undefined' && chrome.tabs) {
        chrome.tabs.getCurrent((tab) => {
            if (tab && tab.id) {
                chrome.tabs.remove(tab.id);
            } else {
                window.close();
            }
        });
    } else {
        window.close();
    }
});

// "开始使用" - 跳转到欢迎页
document.getElementById('btnStart').addEventListener('click', () => {
    if (typeof chrome !== 'undefined' && chrome.tabs) {
        // 在当前标签页打开欢迎页
        window.location.href = 'welcome.html';
    } else {
        window.location.href = 'welcome.html';
    }
});

// ESC 键关闭
document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
        document.getElementById('btnLater').click();
    }
});
