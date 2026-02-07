// data_center.js — 数据中心（IndexedDB 存储 + 假数据调试）
(function(){
  const DB_NAME = 'readpaper_data_center';
  const DB_VERSION = 1;
  const STORE = 'reading_records';

  // 当为 true 时，不使用真实 IndexedDB，而使用内存假数据源
  window.__DATA_CENTER_DEBUG = window.__DATA_CENTER_DEBUG || false;

  // Pagination and sorting state
  let currentPage = 1;
  const pageSize = 20;
  let sortBy = 'timestamp'; // 'timestamp', 'readingTime', 'bookname'
  let sortOrder = 'desc'; // 'asc' or 'desc'

  // override global showmethemoney to control this page's debug flag
  window.showmethemoney = function(enable) {
    window.__DATA_CENTER_DEBUG = !!enable;
    const ds = document.getElementById('debugSection');
    if (ds) {
      ds.classList.toggle('hidden', !window.__DATA_CENTER_DEBUG);
      ds.setAttribute('aria-hidden', !window.__DATA_CENTER_DEBUG);
    }
    render();
    console.log('[data_center] showmethemoney ->', enable);
  };

  // --- IndexedDB helpers ---
  function openDB() {
    return new Promise((resolve, reject) => {
      const req = indexedDB.open(DB_NAME, DB_VERSION);
      req.onerror = (e) => reject(e.target.error || e);
      req.onupgradeneeded = (e) => {
        const db = e.target.result;
        if (!db.objectStoreNames.contains(STORE)) {
          const os = db.createObjectStore(STORE, { keyPath: 'id', autoIncrement: true });
          os.createIndex('bookname', 'bookname', { unique: false });
          os.createIndex('timestamp', 'timestamp', { unique: false });
        }
      };
      req.onsuccess = (e) => resolve(e.target.result);
    });
  }

  async function mergeRecordToDB(record) {
    const db = await openDB();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, 'readwrite');
      const store = tx.objectStore(STORE);
      const index = store.index('bookname');
      
      // Normalize record
      const bookname = record.bookname || record.book_name || (record.book_path ? record.book_path.split('/').pop() : '');
      if (!bookname) {
        resolve(); // Skip if no valid bookname
        return;
      }
      
      const newRecord = {
        book_path: record.book_path || `/sd/book/${bookname}`,
        book_name: bookname,
        bookname: bookname,
        total_hours: record.total_hours || 0,
        total_minutes: record.total_minutes || 0,
        hourly_records: record.hourly_records || {},
        daily_summary: record.daily_summary || {},
        monthly_summary: record.monthly_summary || {},
        timestamp: record.timestamp || Date.now()
      };
      
      // Check if record exists
      const getReq = index.get(bookname);
      getReq.onsuccess = () => {
        const existing = getReq.result;
        if (existing) {
          // Merge hourly_records: for each hour, keep the maximum value (capped at 60)
          const mergedHourly = {...(existing.hourly_records || {})};
          for (const [hour, minutes] of Object.entries(newRecord.hourly_records || {})) {
            const existingMin = mergedHourly[hour] || 0;
            const newMin = Math.min(Number(minutes) || 0, 60); // Cap at 60
            mergedHourly[hour] = Math.max(existingMin, newMin);
          }
          
          // Merge daily_summary: calculate from hourly records
          const mergedDaily = {};
          for (const [hour, minutes] of Object.entries(mergedHourly)) {
            if (hour.length >= 8) {
              const day = hour.substring(0, 8);
              mergedDaily[day] = (mergedDaily[day] || 0) + (Number(minutes) || 0);
            }
          }
          
          // Merge monthly_summary: sum up from daily
          const mergedMonthly = {};
          for (const [day, minutes] of Object.entries(mergedDaily)) {
            const month = day.substring(0, 6); // YYYYMM
            mergedMonthly[month] = (mergedMonthly[month] || 0) + minutes;
          }
          
          // Calculate total time from merged hourly data
          const totalMinutes = Object.values(mergedHourly).reduce((sum, min) => sum + (Number(min) || 0), 0);
          const totalHours = Math.floor(totalMinutes / 60);
          const remainingMinutes = totalMinutes % 60;
          
          const mergedRecord = {
            id: existing.id,
            book_path: existing.book_path || newRecord.book_path,
            book_name: bookname,
            bookname: bookname,
            total_hours: totalHours,
            total_minutes: remainingMinutes,
            hourly_records: mergedHourly,
            daily_summary: mergedDaily,
            monthly_summary: mergedMonthly,
            timestamp: Date.now()
          };
          
          store.put(mergedRecord);
          resolve();
        } else {
          // Add new record - ensure hourly records don't exceed 60 minutes
          const cappedHourly = {};
          for (const [hour, minutes] of Object.entries(newRecord.hourly_records || {})) {
            cappedHourly[hour] = Math.min(Number(minutes) || 0, 60);
          }
          newRecord.hourly_records = cappedHourly;
          store.add(newRecord);
          resolve();
        }
      };
      getReq.onerror = (e) => reject(e.target.error || e);
      tx.onerror = (e) => reject(e.target.error || e);
    });
  }

  async function addRecordToDB(record) {
    const db = await openDB();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, 'readwrite');
      const store = tx.objectStore(STORE);
      // ensure fields
      const rec = Object.assign({}, record);
      if (!rec.bookname && rec.book_name) rec.bookname = rec.book_name;
      if (!rec.timestamp) rec.timestamp = Date.now();
      store.add(rec).onsuccess = (e) => resolve(e.target.result);
      tx.onerror = (e) => reject(e.target.error || e);
    });
  }

  async function getAllFromDB() {
    const db = await openDB();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, 'readonly');
      const store = tx.objectStore(STORE);
      const req = store.getAll();
      req.onsuccess = () => resolve(req.result || []);
      req.onerror = (e) => reject(e.target.error || e);
    });
  }

  async function clearDB() {
    const db = await openDB();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, 'readwrite');
      const store = tx.objectStore(STORE);
      const req = store.clear();
      req.onsuccess = () => resolve();
      req.onerror = (e) => reject(e.target.error || e);
    });
  }

  async function deleteById(id) {
    const db = await openDB();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, 'readwrite');
      const store = tx.objectStore(STORE);
      const req = store.delete(Number(id));
      req.onsuccess = () => resolve();
      req.onerror = (e) => reject(e.target.error || e);
    });
  }

  // --- Fake data source for debug mode ---
  const FAKE_DB = [];
  async function seedFakeData(count = 20) {
    // 真实写入IndexedDB数据库（先清空，然后插入真实的假数据）
    await clearDB();
    const now = Date.now();
    
    // 真实阅读时段分布（符合人类作息规律）
    const readingTimeSlots = [
      { start: 6, end: 8, weight: 0.15 },   // 早晨起床后（6-8点）
      { start: 8, end: 10, weight: 0.08 },  // 上午通勤/早晨（8-10点）
      { start: 10, end: 12, weight: 0.05 }, // 上午工作间隙
      { start: 12, end: 14, weight: 0.12 }, // 午休时段
      { start: 14, end: 17, weight: 0.05 }, // 下午茶时间
      { start: 17, end: 19, weight: 0.08 }, // 下班通勤
      { start: 19, end: 21, weight: 0.25 }, // 晚饭后黄金时段
      { start: 21, end: 23, weight: 0.18 }, // 睡前阅读
      { start: 23, end: 24, weight: 0.04 }  // 深夜
    ];
    
    const bookGenres = ['小说', '技术', '历史', '哲学', '科幻', '散文', '传记', '经济'];
    
    for (let i = 0; i < count; i++) {
      const genre = bookGenres[Math.floor(Math.random() * bookGenres.length)];
      const bname = `${genre}_${String.fromCharCode(65 + Math.floor(Math.random() * 26))}${i+1}.txt`;
      
      // 生成逼真的阅读记录
      const hourlyRecords = {};
      const dailySummary = {};
      const monthlySummary = {};
      
      // 阅读周期：2-6个月前开始
      const monthsBack = Math.floor(Math.random() * 5) + 2;
      // 活跃阅读天数：15-90天（更符合实际完成一本书的时间）
      const readingDays = Math.floor(Math.random() * 76) + 15;
      
      let accumulatedMinutes = 0;
      const daysInPeriod = monthsBack * 30;
      const usedDates = new Set(); // 存储已使用的日期字符串
      const usedDaysAgo = []; // 存储已使用的天数偏移量，用于连续阅读逻辑
      
      for (let day = 0; day < readingDays; day++) {
        // 随机分布在整个时间段内，但有聚集效应（连续阅读更常见）
        let daysAgo;
        if (usedDaysAgo.length > 0 && Math.random() > 0.3) {
          // 70%概率接近上次阅读日期（连续阅读）
          const lastDaysAgo = usedDaysAgo[usedDaysAgo.length - 1];
          // 往前1-5天
          daysAgo = Math.max(0, lastDaysAgo - Math.floor(Math.random() * 5) - 1);
        } else {
          // 30%概率随机分布
          daysAgo = Math.floor(Math.random() * daysInPeriod);
        }
        
        const date = new Date(now - daysAgo * 24 * 60 * 60 * 1000);
        const year = date.getFullYear();
        const month = String(date.getMonth() + 1).padStart(2, '0');
        const dayOfMonth = String(date.getDate()).padStart(2, '0');
        const dateStr = `${year}${month}${dayOfMonth}`; // YYYYMMDD
        const monthStr = `${year}${month}`; // YYYYMM
        const dayOfWeek = date.getDay();
        
        // 避免重复日期
        if (usedDates.has(dateStr)) continue;
        usedDates.add(dateStr);
        usedDaysAgo.push(daysAgo);
        
        let dailyMinutes = 0;
        
        // 周末阅读更多，时间更灵活
        const isWeekend = dayOfWeek === 0 || dayOfWeek === 6;
        const readingSessionsPerDay = isWeekend 
          ? Math.floor(Math.random() * 3) + 2  // 周末2-4次
          : Math.floor(Math.random() * 2) + 1; // 工作日1-2次
        
        // 模拟真实的阅读会话
        for (let session = 0; session < readingSessionsPerDay; session++) {
          // 按权重随机选择时段
          const rand = Math.random();
          let cumulative = 0;
          let selectedSlot = readingTimeSlots[0];
          
          for (const slot of readingTimeSlots) {
            cumulative += slot.weight;
            if (rand < cumulative) {
              selectedSlot = slot;
              break;
            }
          }
          
          // 在时段内随机选择小时
          const slotRange = selectedSlot.end - selectedSlot.start;
          const hour = selectedSlot.start + Math.floor(Math.random() * slotRange);
          const timestamp = dateStr + String(hour).padStart(2, '0'); // YYYYMMDDHH
          
          // 真实的阅读时长分布：大多数20-60分钟，偶尔长时间阅读
          let minutes;
          const r = Math.random();
          if (r < 0.6) {
            minutes = Math.floor(Math.random() * 40) + 20; // 60%概率：20-59分钟
          } else if (r < 0.9) {
            minutes = Math.floor(Math.random() * 60) + 60; // 30%概率：1-2小时
          } else {
            minutes = Math.floor(Math.random() * 120) + 120; // 10%概率：2-4小时（周末长时间阅读）
          }
          
          // 工作日限制最长阅读时间
          if (!isWeekend && minutes > 90) {
            minutes = Math.floor(Math.random() * 30) + 60;
          }
          
          // 累加到hourly记录（同一天同一小时可能重复记录）
          hourlyRecords[timestamp] = (hourlyRecords[timestamp] || 0) + minutes;
          dailyMinutes += minutes;
        }
        
        if (dailyMinutes > 0) {
          dailySummary[dateStr] = dailyMinutes;
          monthlySummary[monthStr] = (monthlySummary[monthStr] || 0) + dailyMinutes;
          accumulatedMinutes += dailyMinutes;
        }
      }
      
      const rec = {
        book_path: `/sd/book/${bname}`,
        book_name: bname,
        bookname: bname,
        total_hours: Math.floor(accumulatedMinutes / 60),
        total_minutes: accumulatedMinutes % 60,
        hourly_records: hourlyRecords,
        daily_summary: dailySummary,
        monthly_summary: monthlySummary,
        timestamp: now - Math.floor(Math.random() * daysInPeriod * 24 * 60 * 60 * 1000)
      };
      
      // 真实写入IndexedDB
      await addRecordToDB(rec);
    }
    
    console.log(`[seedFakeData] 已向真实数据库写入 ${count} 条逼真的阅读记录`);
  }

  // --- UI rendering ---
  async function loadRecords() {
    if (window.__DATA_CENTER_DEBUG) {
      return FAKE_DB.slice();
    }
    try {
      const all = await getAllFromDB();
      return all;
    } catch (e) {
      console.error('loadRecords error', e);
      return [];
    }
  }

  function formatTime(ts) {
    try {
      const d = new Date(Number(ts));
      return d.toLocaleString();
    } catch (e) { return String(ts); }
  }

  async function render() {
    const container = document.getElementById('recordsContainer');
    const summary = document.getElementById('listSummary');
    const chartCanvas = document.getElementById('summaryChart');
    if (!container || !summary) return;
    
    // Save scroll positions before render
    const savedScrollTop = container.scrollTop;
    const savedWindowScrollY = window.scrollY;
    
    container.innerHTML = '加载中…';
    const records = await loadRecords();

    const q = (document.getElementById('searchInput') || {}).value || '';
    let filtered = q ? records.filter(r => (r.bookname||r.book_name||'').indexOf(q) !== -1) : records;
    
    // Sort filtered records
    filtered.sort((a, b) => {
      let valA, valB;
      if (sortBy === 'timestamp') {
        valA = a.timestamp || a._timestamp || 0;
        valB = b.timestamp || b._timestamp || 0;
      } else if (sortBy === 'readingTime') {
        valA = (a.total_hours || 0) * 60 + (a.total_minutes || 0);
        valB = (b.total_hours || 0) * 60 + (b.total_minutes || 0);
      } else if (sortBy === 'bookname') {
        valA = (a.bookname || a.book_name || '');
        valB = (b.bookname || b.book_name || '');
        // Use Chinese pinyin sorting with localeCompare
        return sortOrder === 'asc' 
          ? valA.localeCompare(valB, 'zh-CN', { sensitivity: 'base' })
          : valB.localeCompare(valA, 'zh-CN', { sensitivity: 'base' });
      }
      return sortOrder === 'asc' ? valA - valB : valB - valA;
    });
    
    // Calculate pagination
    const totalPages = Math.ceil(filtered.length / pageSize);
    if (currentPage > totalPages) currentPage = Math.max(1, totalPages);
    const startIdx = (currentPage - 1) * pageSize;
    const endIdx = Math.min(startIdx + pageSize, filtered.length);
    const pagedRecords = filtered.slice(startIdx, endIdx);
    
    // Get month range filter (convert from YYYY-MM to YYYYMM format)
    const monthRangeStartRaw = (document.getElementById('monthRangeStart') || {}).value || '';
    const monthRangeEndRaw = (document.getElementById('monthRangeEnd') || {}).value || '';
    const monthRangeStart = monthRangeStartRaw ? monthRangeStartRaw.replace(/-/g, '') : '';
    const monthRangeEnd = monthRangeEndRaw ? monthRangeEndRaw.replace(/-/g, '') : '';

    // Calculate summary stats
    let totalBooks = filtered.length;
    let totalHours = 0;
    let totalMinutes = 0;
    const hourlyDistribution = new Array(24).fill(0); // Overall 0-23 hourly distribution
    const monthlyTotal = {}; // 'YYYYMM' -> total minutes
    
    filtered.forEach(r => {
      totalHours += r.total_hours || 0;
      totalMinutes += r.total_minutes || 0;
      
      // Aggregate hourly distribution (all months combined)
      if (r.hourly_records) {
        for (const [ts, mins] of Object.entries(r.hourly_records)) {
          if (ts.length >= 10) {
            const hour = parseInt(ts.substring(8, 10), 10);
            hourlyDistribution[hour] += mins;
          }
        }
      }
      
      // Aggregate monthly totals
      if (r.monthly_summary) {
        for (const [month, mins] of Object.entries(r.monthly_summary)) {
          monthlyTotal[month] = (monthlyTotal[month] || 0) + mins;
        }
      }
    });
    totalHours += Math.floor(totalMinutes / 60);
    totalMinutes %= 60;
    
    // Filter and limit months
    let allMonths = Object.keys(monthlyTotal).sort();
    
    // Apply user-specified range if provided
    if (monthRangeStart && monthRangeEnd) {
      allMonths = allMonths.filter(m => m >= monthRangeStart && m <= monthRangeEnd);
    }
    
    // Limit to most recent 12 months if no range specified or result exceeds 12
    if ((!monthRangeStart || !monthRangeEnd) && allMonths.length > 12) {
      allMonths = allMonths.slice(-12);
    }
    
    // Create filtered monthlyTotal for display
    const filteredMonthlyTotal = {};
    allMonths.forEach(m => {
      filteredMonthlyTotal[m] = monthlyTotal[m];
    });

    // Calculate earliest and latest reading dates
    let earliestDate = null;
    let latestDate = null;
    filtered.forEach(r => {
      if (r.daily_summary) {
        const dates = Object.keys(r.daily_summary).filter(d => r.daily_summary[d] > 0).sort();
        if (dates.length > 0) {
          const first = dates[0];
          const last = dates[dates.length - 1];
          if (!earliestDate || first < earliestDate) earliestDate = first;
          if (!latestDate || last > latestDate) latestDate = last;
        }
      }
    });

    // Format dates for display (YYYYMMDD -> YYYY-MM-DD)
    const formatDate = (dateStr) => {
      if (!dateStr || dateStr.length !== 8) return '无';
      return `${dateStr.substring(0, 4)}-${dateStr.substring(4, 6)}-${dateStr.substring(6, 8)}`;
    };

    // Render summary text
    if (chartCanvas) {
      chartCanvas.innerHTML = `
        <div style="font-size:18px; font-weight:bold; color:#333;">书籍总数: <span style="color:#4CAF50;">${totalBooks}</span> 本</div>
        <div style="font-size:18px; font-weight:bold; color:#333;">总阅读时间: <span style="color:#2196F3;">${totalHours}</span> 小时 <span style="color:#666;">${totalMinutes}</span> 分钟</div>
        <div style="font-size:16px; color:#666; margin-top:10px;">最早阅读: <span style="color:#FF9800;">${formatDate(earliestDate)}</span></div>
        <div style="font-size:16px; color:#666;">最后阅读: <span style="color:#FF9800;">${formatDate(latestDate)}</span></div>
      `;
    }

    // Render hourly distribution chart (curve, like readingRecord)
    const hourlyCanvas = document.getElementById('hourlyChart');
    if (hourlyCanvas) {
      const ctx = hourlyCanvas.getContext('2d');
      
      // Make canvas responsive to container width
      const container = hourlyCanvas.parentElement;
      if (container && container.clientWidth > 0) {
        hourlyCanvas.width = container.clientWidth;
      }
      
      const width = hourlyCanvas.width;
      const height = hourlyCanvas.height;
      ctx.clearRect(0, 0, width, height);
      
      const padding = 50;
      const chartWidth = width - padding * 2;
      const chartHeight = height - padding * 2;
      const maxMinutes = Math.max(...hourlyDistribution, 1);
      const primaryColor = '#2196F3';
      
      // Draw axes
      ctx.strokeStyle = '#ccc';
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(padding, padding);
      ctx.lineTo(padding, padding + chartHeight);
      ctx.lineTo(padding + chartWidth, padding + chartHeight);
      ctx.stroke();
      
      // Hour labels (every 3 hours)
      ctx.fillStyle = '#666';
      ctx.font = '10px Arial';
      ctx.textAlign = 'center';
      for (let i = 0; i <= 24; i += 3) {
        const x = padding + (chartWidth / 24) * i;
        ctx.fillText(i.toString(), x, padding + chartHeight + 15);
      }
      
      // Y-axis label
      ctx.save();
      ctx.translate(15, padding + chartHeight / 2);
      ctx.rotate(-Math.PI / 2);
      ctx.textAlign = 'center';
      ctx.fillText('阅读时长(分钟)', 0, 0);
      ctx.restore();
      
      // Y-axis scale markers (5 ticks from 0 to maxMinutes)
      ctx.fillStyle = '#666';
      ctx.font = '10px Arial';
      ctx.textAlign = 'right';
      for (let i = 0; i <= 5; i++) {
        const value = Math.round((maxMinutes / 5) * i);
        const y = padding + chartHeight - (chartHeight / 5) * i;
        ctx.fillText(value.toString(), padding - 5, y + 3);
        // Draw tick line
        ctx.strokeStyle = '#eee';
        ctx.beginPath();
        ctx.moveTo(padding, y);
        ctx.lineTo(padding + chartWidth, y);
        ctx.stroke();
      }
      
      // Draw curve
      ctx.strokeStyle = primaryColor;
      ctx.lineWidth = 2;
      ctx.beginPath();
      for (let hour = 0; hour < 24; hour++) {
        const x = padding + (chartWidth / 24) * hour + (chartWidth / 48);
        const y = padding + chartHeight - (hourlyDistribution[hour] / maxMinutes) * chartHeight;
        if (hour === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();
      
      // Draw points
      ctx.fillStyle = primaryColor;
      for (let hour = 0; hour < 24; hour++) {
        const x = padding + (chartWidth / 24) * hour + (chartWidth / 48);
        const y = padding + chartHeight - (hourlyDistribution[hour] / maxMinutes) * chartHeight;
        ctx.beginPath();
        ctx.arc(x, y, 3, 0, Math.PI * 2);
        ctx.fill();
      }
      
      // Fill area
      ctx.fillStyle = 'rgba(33, 150, 243, 0.1)';
      ctx.beginPath();
      ctx.moveTo(padding, padding + chartHeight);
      for (let hour = 0; hour < 24; hour++) {
        const x = padding + (chartWidth / 24) * hour + (chartWidth / 48);
        const y = padding + chartHeight - (hourlyDistribution[hour] / maxMinutes) * chartHeight;
        ctx.lineTo(x, y);
      }
      ctx.lineTo(padding + chartWidth, padding + chartHeight);
      ctx.closePath();
      ctx.fill();
    }
    
    // Render monthly distribution chart (bar chart)
    const monthlyCanvas = document.getElementById('monthlyChart');
    if (monthlyCanvas) {
      const ctx = monthlyCanvas.getContext('2d');
      const width = monthlyCanvas.width;
      const height = monthlyCanvas.height;
      ctx.clearRect(0, 0, width, height);
      
      const months = Object.keys(filteredMonthlyTotal).sort();
      if (months.length === 0) {
        ctx.fillStyle = '#999';
        ctx.font = '16px Arial';
        ctx.textAlign = 'center';
        ctx.fillText('暂无月度数据', width / 2, height / 2);
      } else {
        const padding = 60;
        const chartWidth = width - padding * 2;
        const chartHeight = height - padding * 2;
        const barWidth = chartWidth / months.length - 10;
        const maxValueMinutes = Math.max(...Object.values(filteredMonthlyTotal));
        const maxValueHours = maxValueMinutes / 60;
        const scale = maxValueHours > 0 ? chartHeight / maxValueHours : 1;
        
        // Draw axes
        ctx.strokeStyle = '#ccc';
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(padding, padding);
        ctx.lineTo(padding, padding + chartHeight);
        ctx.lineTo(padding + chartWidth, padding + chartHeight);
        ctx.stroke();
        
        // Y-axis scale markers (5 ticks from 0 to maxValueHours)
        ctx.fillStyle = '#666';
        ctx.font = '10px Arial';
        ctx.textAlign = 'right';
        for (let i = 0; i <= 5; i++) {
          const value = Math.round((maxValueHours / 5) * i * 10) / 10; // 1 decimal place
          const y = padding + chartHeight - (chartHeight / 5) * i;
          ctx.fillText(value.toFixed(1) + 'h', padding - 5, y + 3);
          // Draw tick line
          ctx.strokeStyle = '#eee';
          ctx.beginPath();
          ctx.moveTo(padding, y);
          ctx.lineTo(padding + chartWidth, y);
          ctx.stroke();
        }
        
        // Draw bars
        ctx.fillStyle = '#4CAF50';
        months.forEach((month, i) => {
          const x = padding + (chartWidth / months.length) * i + 5;
          const valueHours = filteredMonthlyTotal[month] / 60;
          const barHeight = valueHours * scale;
          const y = padding + chartHeight - barHeight;
          ctx.fillRect(x, y, barWidth, barHeight);
          
          // Month label
          ctx.fillStyle = '#666';
          ctx.font = '10px Arial';
          ctx.textAlign = 'center';
          ctx.fillText(month, x + barWidth / 2, padding + chartHeight + 15);
          
          // Value label
          ctx.fillText(valueHours.toFixed(1) + 'h', x + barWidth / 2, y - 5);
          ctx.fillStyle = '#4CAF50';
        });
        
        // Y-axis label
        ctx.fillStyle = '#666';
        ctx.save();
        ctx.translate(15, padding + chartHeight / 2);
        ctx.rotate(-Math.PI / 2);
        ctx.textAlign = 'center';
        ctx.fillText('阅读时长(小时)', 0, 0);
        ctx.restore();
      }
    }

    //summary.textContent = `共 ${filtered.length} 条记录，总阅读时间: ${totalHours}小时${totalMinutes}分钟 （${window.__DATA_CENTER_DEBUG ? '调试假数据' : 'IndexedDB'}） | 第 ${currentPage}/${totalPages} 页`;
    summary.textContent = `共 ${filtered.length} 条记录`;
    if (filtered.length === 0) {
      container.innerHTML = '<div class="no-data">暂无记录</div>';
      document.getElementById('paginationContainer').innerHTML = '';
      return;
    }

    const table = document.createElement('table');
    table.style.width = '100%';
    table.style.borderCollapse = 'collapse';
    const thead = document.createElement('thead');
    thead.innerHTML = '<tr><th style="text-align:left">ID</th><th style="text-align:left">书名</th><th style="text-align:left" class="time-column">时间</th><th style="text-align:left">摘要</th><th></th></tr>';
    table.appendChild(thead);
    const tbody = document.createElement('tbody');

    pagedRecords.forEach(r => {
      const tr = document.createElement('tr');
      tr.style.borderTop = '1px solid #eee';
      const idCell = document.createElement('td');
      idCell.textContent = r.id || '';
      const nameCell = document.createElement('td');
      const bookNameText = r.bookname || r.book_name || (r.book_path ? r.book_path.split('/').pop() : '—');
      // create clickable link to readingRecord.html showing this book's records
      const a = document.createElement('a');
      a.textContent = bookNameText;
      // determine book path param if available, otherwise use book name
      const bookPathParam = encodeURIComponent(r.book_path || (`/sd/book/${bookNameText}`));
      // mark source as local so readingRecord can choose local DB instead of wifi API
      a.href = `readingRecord.html?book=${bookPathParam}&src=local`;
      a.style.cursor = 'pointer';
      nameCell.appendChild(a);
      const timeCell = document.createElement('td');
      timeCell.className = 'time-column';
      timeCell.textContent = formatTime(r.timestamp || r._timestamp || '—');
      const summaryCell = document.createElement('td');
      summaryCell.textContent = (r.total_hours ? `${r.total_hours}h ` : '') + (r.total_minutes ? `${r.total_minutes}m` : '—');
      const actionCell = document.createElement('td');
      actionCell.style.textAlign = 'center';
      const delBtn = document.createElement('button');
      delBtn.className = 'button is-small outline';
      delBtn.innerHTML = '×'; // Delete icon
      delBtn.style.fontSize = '18px';
      delBtn.style.lineHeight = '1';
      delBtn.style.padding = '2px 8px';
      delBtn.style.color = '#d32f2f';
      delBtn.title = '删除记录';
      delBtn.addEventListener('click', async () => {
        if (window.__DATA_CENTER_DEBUG) {
          const idx = FAKE_DB.indexOf(r);
          if (idx >= 0) FAKE_DB.splice(idx,1);
          render();
        } else {
          if (!r.id) return alert('记录无 id，无法删除');
          await deleteById(r.id);
          render();
        }
      });
      actionCell.appendChild(delBtn);

      tr.appendChild(idCell);
      tr.appendChild(nameCell);
      tr.appendChild(timeCell);
      tr.appendChild(summaryCell);
      tr.appendChild(actionCell);
      tbody.appendChild(tr);
    });

    table.appendChild(tbody);
    container.innerHTML = '';
    container.appendChild(table);
    
    // Render modern pagination controls
    const paginationContainer = document.getElementById('paginationContainer');
    paginationContainer.innerHTML = '';
    paginationContainer.className = 'modern-pagination';
    
    if (totalPages > 1) {
      // Previous button
      const prevBtn = document.createElement('button');
      prevBtn.className = 'pg-btn';
      prevBtn.innerHTML = '‹';
      prevBtn.title = '上一页';
      prevBtn.disabled = currentPage === 1;
      prevBtn.addEventListener('click', () => {
        if (currentPage > 1) {
          currentPage--;
          render();
        }
      });
      paginationContainer.appendChild(prevBtn);
      
      // Page number buttons
      const pageButtons = [];
      const maxVisiblePages = 7; // Show max 7 page buttons
      
      if (totalPages <= maxVisiblePages) {
        // Show all pages
        for (let i = 1; i <= totalPages; i++) {
          pageButtons.push(i);
        }
      } else {
        // Smart pagination with ellipsis
        if (currentPage <= 4) {
          // Near start: 1 2 3 4 5 ... last
          for (let i = 1; i <= 5; i++) pageButtons.push(i);
          pageButtons.push('...');
          pageButtons.push(totalPages);
        } else if (currentPage >= totalPages - 3) {
          // Near end: 1 ... last-4 last-3 last-2 last-1 last
          pageButtons.push(1);
          pageButtons.push('...');
          for (let i = totalPages - 4; i <= totalPages; i++) pageButtons.push(i);
        } else {
          // Middle: 1 ... current-1 current current+1 ... last
          pageButtons.push(1);
          pageButtons.push('...');
          pageButtons.push(currentPage - 1);
          pageButtons.push(currentPage);
          pageButtons.push(currentPage + 1);
          pageButtons.push('...');
          pageButtons.push(totalPages);
        }
      }
      
      pageButtons.forEach(page => {
        if (page === '...') {
          const ellipsis = document.createElement('span');
          ellipsis.className = 'pg-ellipsis';
          ellipsis.textContent = '•••';
          paginationContainer.appendChild(ellipsis);
        } else {
          const pageBtn = document.createElement('button');
          pageBtn.className = 'pg-btn' + (page === currentPage ? ' active' : '');
          pageBtn.textContent = page;
          pageBtn.addEventListener('click', () => {
            currentPage = page;
            render();
          });
          paginationContainer.appendChild(pageBtn);
        }
      });
      
      // Next button
      const nextBtn = document.createElement('button');
      nextBtn.className = 'pg-btn';
      nextBtn.innerHTML = '›';
      nextBtn.title = '下一页';
      nextBtn.disabled = currentPage === totalPages;
      nextBtn.addEventListener('click', () => {
        if (currentPage < totalPages) {
          currentPage++;
          render();
        }
      });
      paginationContainer.appendChild(nextBtn);
      
      // Jump section (only show if many pages)
      if (totalPages > 10) {
        const jumpSection = document.createElement('div');
        jumpSection.className = 'pg-jump';
        
        const jumpInput = document.createElement('input');
        jumpInput.type = 'number';
        jumpInput.min = '1';
        jumpInput.max = totalPages.toString();
        jumpInput.placeholder = '页码';
        jumpInput.addEventListener('keypress', (e) => {
          if (e.key === 'Enter') {
            const page = parseInt(jumpInput.value, 10);
            if (page >= 1 && page <= totalPages) {
              currentPage = page;
              render();
            }
          }
        });
        
        const jumpBtn = document.createElement('button');
        jumpBtn.className = 'button is-small outline';
        jumpBtn.textContent = 'GO';
        jumpBtn.addEventListener('click', () => {
          const page = parseInt(jumpInput.value, 10);
          if (page >= 1 && page <= totalPages) {
            currentPage = page;
            render();
          }
        });
        
        jumpSection.appendChild(jumpInput);
        jumpSection.appendChild(jumpBtn);
        paginationContainer.appendChild(jumpSection);
      }
    } else {
      paginationContainer.style.display = 'none';
    }
    
    // Restore scroll positions to prevent jumping
    requestAnimationFrame(() => {
      container.scrollTop = savedScrollTop;
      window.scrollTo(0, savedWindowScrollY);
    });
  }

  // --- wire UI ---
  function setupUI() {
    const debugToggle = document.getElementById('debugToggle');
    const seedFake = document.getElementById('seedFake');
    const clearBtn = document.getElementById('clearDb');
    const exportBtn = document.getElementById('exportBtn');
    const importBtn = document.getElementById('importBtn');
    const importFile = document.getElementById('importFile');
    const searchInput = document.getElementById('searchInput');
    const applyMonthRange = document.getElementById('applyMonthRange');
    const clearMonthRange = document.getElementById('clearMonthRange');

    function updateDebugVisibility() {
      const ds = document.getElementById('debugSection');
      const enabled = !!(debugToggle && debugToggle.checked) || !!window.__DATA_CENTER_DEBUG;
      if (ds) {
        ds.classList.toggle('hidden', !enabled);
        ds.setAttribute('aria-hidden', !enabled);
      }
      if (seedFake) seedFake.style.display = enabled ? '' : 'none';
      if (clearBtn) clearBtn.style.display = enabled ? '' : 'none';
    }

    if (debugToggle) {
      debugToggle.checked = !!window.__DATA_CENTER_DEBUG;
      debugToggle.addEventListener('change', () => {
        window.__DATA_CENTER_DEBUG = !!debugToggle.checked;
        // call global helper
        if (window.showmethemoney) window.showmethemoney(!!debugToggle.checked);
        updateDebugVisibility();
        render();
      });
    }
    
    if (applyMonthRange) {
      applyMonthRange.addEventListener('click', () => {
        render();
      });
    }
    
    if (clearMonthRange) {
      clearMonthRange.addEventListener('click', () => {
        document.getElementById('monthRangeStart').value = '';
        document.getElementById('monthRangeEnd').value = '';
        render();
      });
    }
    
    // Sort controls
    const sortBySelect = document.getElementById('sortBy');
    const sortOrderBtn = document.getElementById('sortOrderBtn');
    if (sortBySelect) {
      sortBySelect.addEventListener('change', () => {
        sortBy = sortBySelect.value;
        currentPage = 1; // Reset to first page when sorting changes
        render();
      });
    }
    if (sortOrderBtn) {
      sortOrderBtn.addEventListener('click', () => {
        sortOrder = sortOrder === 'desc' ? 'asc' : 'desc';
        sortOrderBtn.textContent = sortOrder === 'desc' ? '↓' : '↑';
        currentPage = 1; // Reset to first page when order changes
        render();
      });
    }
    
    if (seedFake) seedFake.addEventListener('click', async () => { await seedFakeData(30); render(); });
    if (clearBtn) clearBtn.addEventListener('click', async () => { if (confirm('确定清空数据库？此操作不可恢复')) { await clearDB(); render(); } });
    if (exportBtn) exportBtn.addEventListener('click', async () => {
      const data = await loadRecords();
      const blob = new Blob([JSON.stringify(data, null, 2)], {type:'application/json'});
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url; a.download = 'readpaper_records.json'; document.body.appendChild(a); a.click(); a.remove(); URL.revokeObjectURL(url);
    });
    
    if (importBtn && importFile) {
      importBtn.addEventListener('click', () => {
        importFile.click();
      });
      
      importFile.addEventListener('change', async (e) => {
        const file = e.target.files[0];
        if (!file) return;
        
        try {
          const text = await file.text();
          let records = JSON.parse(text);
          
          // Handle both array and object with records property
          if (!Array.isArray(records)) {
            if (records.records && Array.isArray(records.records)) {
              records = records.records;
            } else {
              throw new Error('无效的JSON格式，应为记录数组');
            }
          }
          
          if (records.length === 0) {
            alert('文件中没有记录');
            return;
          }
          
          const confirmed = confirm(`即将导入 ${records.length} 条记录，重复的记录将按书名合并。确认继续？`);
          if (!confirmed) return;
          
          let imported = 0;
          let failed = 0;
          
          for (const record of records) {
            try {
              await mergeRecordToDB(record);
              imported++;
            } catch (err) {
              console.error('Failed to import record:', record, err);
              failed++;
            }
          }
          
          alert(`导入完成！\n成功: ${imported} 条\n失败: ${failed} 条`);
          render();
        } catch (err) {
          console.error('Import error:', err);
          alert('导入失败: ' + err.message);
        } finally {
          importFile.value = ''; // Reset file input
        }
      });
    }
    if (searchInput) searchInput.addEventListener('input', () => {
      currentPage = 1; // Reset to first page when search changes
      render();
    });
  }

  // initialize
  document.addEventListener('DOMContentLoaded', async () => {
    setupUI();
    // if debug flag initially true, seed some fake data so UI isn't empty
    if (window.__DATA_CENTER_DEBUG) {
      await seedFakeData(20);
      // ensure debug UI visible on load
      const ds = document.getElementById('debugSection');
      if (ds) { ds.classList.remove('hidden'); ds.setAttribute('aria-hidden', 'false'); }
      const seedFake = document.getElementById('seedFake');
      const clearBtn = document.getElementById('clearDb');
      if (seedFake) seedFake.style.display = '';
      if (clearBtn) clearBtn.style.display = '';
    }
    await render();
  });

})();
