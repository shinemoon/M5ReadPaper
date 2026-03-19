function createUpdateItem(update) {
    const li = document.createElement('li');
    const strong = document.createElement('strong');
    strong.textContent = update.category || '更新';

    li.appendChild(strong);
    li.appendChild(document.createTextNode(` - ${update.description || ''}`));
    return li;
}

function renderSections(container, sections) {
    if (!sections || !Array.isArray(sections) || sections.length === 0) {
        return;
    }

    sections.forEach(section => {
        const sectionDiv = document.createElement('div');
        sectionDiv.className = 'update-section';

        const title = document.createElement('h2');
        title.textContent = `${section.icon || ''} ${section.title || ''}`.trim();
        sectionDiv.appendChild(title);

        if (section.updates && section.updates.length > 0) {
            const ul = document.createElement('ul');
            ul.className = 'update-list';

            section.updates.forEach(update => {
                ul.appendChild(createUpdateItem(update));
            });

            sectionDiv.appendChild(ul);
        }

        container.appendChild(sectionDiv);
    });
}

function renderAnnouncement(container, announcement) {
    if (!announcement || !announcement.items || announcement.items.length === 0) {
        return;
    }

    const announcementDiv = document.createElement('div');
    announcementDiv.className = 'update-section';

    const title = document.createElement('h2');
    title.textContent = `${announcement.icon || '📰'} ${announcement.title || '消息提醒'}`;
    announcementDiv.appendChild(title);

    const ul = document.createElement('ul');
    ul.className = 'update-list';

    announcement.items.forEach(item => {
        const li = document.createElement('li');
        const strong = document.createElement('strong');
        strong.textContent = item.title || '公告';
        li.appendChild(strong);
        li.appendChild(document.createTextNode(` - ${item.content || ''}`));
        ul.appendChild(li);
    });

    announcementDiv.appendChild(ul);
    container.appendChild(announcementDiv);
}

function renderReleaseCard(updateBody, release) {
    const card = document.createElement('section');
    card.className = 'history-release';

    const cardHeader = document.createElement('button');
    cardHeader.type = 'button';
    cardHeader.className = 'history-release-header';
    cardHeader.setAttribute('aria-expanded', 'false');

    const title = document.createElement('h3');
    title.textContent = `v${release.version || '未知版本'}`;
    cardHeader.appendChild(title);

    const meta = document.createElement('div');
    meta.className = 'history-release-meta';
    const dateText = release.date ? `发布日期: ${release.date}` : '发布日期: -';
    const fwText = release.latestFirmware ? `对应固件: ${release.latestFirmware}` : '对应固件: -';
    meta.textContent = `${dateText} · ${fwText}`;
    cardHeader.appendChild(meta);

    const arrow = document.createElement('span');
    arrow.className = 'history-release-arrow';
    arrow.textContent = '▾';
    cardHeader.appendChild(arrow);

    card.appendChild(cardHeader);

    const content = document.createElement('div');
    content.className = 'history-release-content';

    renderSections(content, release.sections);
    renderAnnouncement(content, release.announcement);

    card.appendChild(content);

    cardHeader.addEventListener('click', () => {
        const isExpanded = card.classList.toggle('expanded');
        cardHeader.setAttribute('aria-expanded', isExpanded ? 'true' : 'false');
    });

    updateBody.appendChild(card);
}

// 加载并渲染更新内容
async function loadWhatsNew() {
    try {
        const response = await fetch('../pages/whatsnew.json');
        if (!response.ok) {
            throw new Error('Failed to load whatsnew.json');
        }

        const data = await response.json();
        const updateBody = document.getElementById('updateBody');
        updateBody.innerHTML = '';

        const latestTitle = document.createElement('h2');
        latestTitle.className = 'timeline-title';
        latestTitle.textContent = '当前版本更新';
        updateBody.appendChild(latestTitle);

        renderSections(updateBody, data.sections);
        renderAnnouncement(updateBody, data.announcement);

        const history = Array.isArray(data.history) ? data.history : [];
        if (history.length > 0) {
            const historyTitle = document.createElement('h2');
            historyTitle.className = 'timeline-title';
            historyTitle.textContent = '完整历史记录';
            updateBody.appendChild(historyTitle);

            history.forEach(release => renderReleaseCard(updateBody, release));
        }

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
