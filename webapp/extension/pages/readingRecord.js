// readingRecord.js - 阅读记录可视化

// Fake test data for debugging UI without API
const FAKE_TEST_DATA = {
    total: 3,
    processed: 3,
    records: [
        {
            book_path: "/sd/book/测试书籍1.txt",
            book_name: "测试书籍1.txt",
            total_hours: 45,
            total_minutes: 30,
            hourly_records: {
                "2026011907": 45, "2026011908": 60, "2026011909": 30,
                "2026011810": 50, "2026011811": 40, "2026011814": 55,
                "2026011715": 35, "2026011716": 45, "2026011720": 60,
                "2026011608": 40, "2026011609": 50, "2026011621": 30,
                "2026011507": 45, "2026011508": 55, "2026011522": 40,
                "2026011409": 50, "2026011410": 35, "2026011423": 45,
                "2026011307": 60, "2026011308": 40, "2026011315": 50,
                "2026010819": 55, "2026010820": 45, "2026010821": 35,
                "2026010109": 40, "2026010110": 50, "2026010122": 30,
                "2025123007": 45, "2025123008": 55, "2025123020": 40,
                "2025122109": 50, "2025122110": 35, "2025122114": 45,
                "2025120807": 60, "2025120808": 40, "2025120809": 30,
                "2025110519": 45, "2025110520": 50, "2025110521": 35,
                "2025100207": 55, "2025100208": 40, "2025100209": 45,
                "2025090810": 50, "2025090811": 60, "2025090814": 35,
                "2025081507": 45, "2025081508": 40, "2025081520": 50
            },
            daily_summary: {
                "20260119": 135, "20260118": 145, "20260117": 140,
                "20260116": 120, "20260115": 140, "20260114": 130,
                "20260113": 150, "20260108": 135, "20260101": 120,
                "20251230": 140, "20251221": 130, "20251208": 130,
                "20251105": 130, "20251002": 140, "20250908": 145,
                "20250815": 135
            },
            monthly_summary: {
                "202601": 1300, "202512": 650, "202511": 380,
                "202510": 420, "202509": 390, "202508": 350
            }
        },
        {
            book_path: "/sd/book/测试书籍2.txt",
            book_name: "测试书籍2.txt",
            total_hours: 28,
            total_minutes: 45,
            hourly_records: {
                "2026011910": 50, "2026011911": 45, "2026011914": 40,
                "2026011808": 55, "2026011809": 35, "2026011819": 50,
                "2026011710": 45, "2026011711": 40, "2026011721": 35,
                "2026011609": 50, "2026011610": 45, "2026011620": 40,
                "2026011508": 35, "2026011509": 55, "2026011521": 45,
                "2026011410": 50, "2026011411": 40, "2026011422": 35,
                "2026011308": 45, "2026011309": 50, "2026011316": 40
            },
            daily_summary: {
                "20260119": 135, "20260118": 140, "20260117": 120,
                "20260116": 135, "20260115": 135, "20260114": 125,
                "20260113": 135
            },
            monthly_summary: {
                "202601": 925
            }
        },
        {
            book_path: "/sd/book/测试书籍3.txt",
            book_name: "测试书籍3.txt",
            total_hours: 15,
            total_minutes: 20,
            hourly_records: {
                "2026011919": 40, "2026011920": 35, "2026011921": 30,
                "2026011807": 45, "2026011808": 40, "2026011822": 35,
                "2026011714": 50, "2026011715": 45, "2026011716": 40,
                "2026011607": 35, "2026011608": 45, "2026011619": 40
            },
            daily_summary: {
                "20260119": 105, "20260118": 120, "20260117": 135,
                "20260116": 120
            },
            monthly_summary: {
                "202601": 480
            }
        }
    ]
};

let aggregatedGlobalData = null;
let currentDisplayLimit = 60; // default show 60 days
const DISPLAY_STEP = 60;

(async function() {
    const urlParams = new URLSearchParams(window.location.search);
    const bookPath = urlParams.get('book');
    const booksPaths = urlParams.get('books');
    const allBooks = urlParams.get('all') === 'true';
    
    // Debug mode: if no parameters, use fake test data
    const debugMode = !bookPath && !booksPaths && !allBooks;
    
    let data;
    
    if (debugMode) {
        console.log('[DEBUG MODE] Using fake test data for UI debugging');
        // Show loading briefly for visual consistency
        document.getElementById('loadingIndicator').style.display = 'block';
        
        // Simulate API delay
        await new Promise(resolve => setTimeout(resolve, 500));
        
        data = FAKE_TEST_DATA;

        // Generate extended fake days (200 days) to test 'load more' pagination
        (function generateFakeDays(record, days) {
            const today = new Date();
            const monthlyAgg = {};
            for (let i = 0; i < days; i++) {
                const d = new Date(today);
                d.setDate(today.getDate() - i);
                const yyyy = d.getFullYear();
                const mm = String(d.getMonth() + 1).padStart(2, '0');
                const dd = String(d.getDate()).padStart(2, '0');
                const dateKey = `${yyyy}${mm}${dd}`;
                const monthKey = `${yyyy}${mm}`;

                // random minutes 0..120
                const minutes = Math.floor(Math.random() * 121);
                record.daily_summary[dateKey] = minutes;
                monthlyAgg[monthKey] = (monthlyAgg[monthKey] || 0) + minutes;

                // distribute minutes into 1-3 random hours
                let remaining = minutes;
                const parts = Math.min(3, Math.max(1, Math.floor(Math.random() * 3) + 1));
                for (let p = 0; p < parts; p++) {
                    const hour = String(Math.floor(Math.random() * 24)).padStart(2, '0');
                    const key = `${dateKey}${hour}`;
                    const val = (p === parts - 1) ? remaining : Math.floor(Math.random() * (remaining + 1));
                    if (val > 0) {
                        record.hourly_records[key] = (record.hourly_records[key] || 0) + val;
                        remaining -= val;
                    }
                }
            }
            record.monthly_summary = Object.assign(record.monthly_summary || {}, monthlyAgg);
        })(data.records[0], 200);
        
        // Update progress
        const progressFill = document.getElementById('progressFill');
        progressFill.style.width = '100%';
        
        // Hide loading, show content
        document.getElementById('loadingIndicator').style.display = 'none';
        document.getElementById('contentArea').style.display = 'block';
        
        // Update header info
        const recordInfo = document.getElementById('recordInfo');
        recordInfo.textContent = `[调试模式] 测试数据 (${data.total} 本书籍)`;
        recordInfo.style.color = '#ff6b6b';
    } else {
            let apiUrl = 'http://192.168.4.1/api/reading_records';

            // If caller explicitly requested local source (from data center), load from
            // local IndexedDB instead of wifi API. This preserves filemanager behavior
            // which continues to use the wifi API.
            const src = urlParams.get('src');
            if (src === 'local' && bookPath) {
                // We'll bypass api fetch and load records from local IndexedDB below.
            } else {
                if (bookPath) {
                    apiUrl += `?book=${encodeURIComponent(bookPath)}`;
                } else if (booksPaths) {
                    apiUrl += `?books=${encodeURIComponent(booksPaths)}`;
                }
            }
        // else: fetch all books (default)

        try {
            // Show loading
            document.getElementById('loadingIndicator').style.display = 'block';

            // If src=local (from data center) and a bookPath is provided, read from local IndexedDB
            if (src === 'local' && bookPath) {
                const DB_NAME = 'readpaper_data_center';
                const DB_VERSION = 1;
                const STORE = 'reading_records';
                function openDB() {
                    return new Promise((resolve, reject)=>{
                        const req = indexedDB.open(DB_NAME, DB_VERSION);
                        req.onerror = (e)=> reject(e.target.error || e);
                        req.onupgradeneeded = (e)=> resolve(e.target.result);
                        req.onsuccess = (e)=> resolve(e.target.result);
                    });
                }
                async function getAll() {
                    const db = await openDB();
                    return new Promise((resolve, reject)=>{
                        const tx = db.transaction(STORE, 'readonly');
                        const store = tx.objectStore(STORE);
                        const req = store.getAll();
                        req.onsuccess = ()=> resolve(req.result || []);
                        req.onerror = (e)=> reject(e.target.error || e);
                    });
                }
                try {
                    const all = await getAll();
                    const decoded = decodeURIComponent(bookPath || '');
                    const targetName = (decoded.split('/').pop() || '').toString();
                    const matched = all.filter(r=>{
                        const name = (r.bookname || r.book_name || (r.book_path? r.book_path.split('/').pop(): '') || '').toString();
                        return name === targetName || decodeURIComponent(name) === targetName;
                    });
                    console.log('[Local DB Filter]', { all: all.length, targetName, matched: matched.length, matchedRecords: matched });
                    data = { total: matched.length, records: matched };
                } catch(err) {
                    console.error('local DB read failed', err);
                    data = { total: 0, records: [] };
                }
            } else {
                // Fetch records from wifi API
                const response = await fetch(apiUrl);
                if (!response.ok) {
                    throw new Error(`HTTP ${response.status}: ${response.statusText}`);
                }
                data = await response.json();
            }
        
        // Update progress
        const progressFill = document.getElementById('progressFill');
        progressFill.style.width = '100%';

        // Hide loading, show content
        setTimeout(() => {
            document.getElementById('loadingIndicator').style.display = 'none';
            document.getElementById('contentArea').style.display = 'block';
        }, 300);

        // Helper function to remove file extension
        const removeFileExtension = (name) => {
            const lastDot = name.lastIndexOf('.');
            if (lastDot > 0) {
                return name.substring(0, lastDot);
            }
            return name;
        };
        
        // Helper function to truncate book name
        const truncateBookName = (name, maxLength = 10) => {
            // Remove extension first
            name = removeFileExtension(name);
            if (name.length <= maxLength) return name;
            return name.substring(0, maxLength) + '...';
        };

        // Update header info
        const recordInfo = document.getElementById('recordInfo');
        if (bookPath) {
            const bookName = bookPath.split('/').pop();
            recordInfo.textContent = `《${truncateBookName(bookName, 20)}》`;
        } else if (booksPaths) {
            const paths = booksPaths.split(',');
            const count = paths.length;
            if (count === 1) {
                const bookName = paths[0].split('/').pop();
                recordInfo.textContent = `《${truncateBookName(bookName, 20)}》`;
            } else if (count === 2) {
                const book1 = paths[0].split('/').pop();
                const book2 = paths[1].split('/').pop();
                recordInfo.textContent = `《${truncateBookName(book1)}》《${truncateBookName(book2)}》`;
            } else {
                const book1 = paths[0].split('/').pop();
                const book2 = paths[1].split('/').pop();
                recordInfo.textContent = `《${truncateBookName(book1)}》《${truncateBookName(book2)}》等${count}本书籍`;
            }
        } else {
            if (data.total === 1 && data.records && data.records.length > 0) {
                const bookName = data.records[0].book_name || data.records[0].book_path.split('/').pop();
                recordInfo.textContent = `《${truncateBookName(bookName, 20)}》`;
            } else if (data.total === 2 && data.records && data.records.length >= 2) {
                const book1 = data.records[0].book_name || data.records[0].book_path.split('/').pop();
                const book2 = data.records[1].book_name || data.records[1].book_path.split('/').pop();
                recordInfo.textContent = `《${truncateBookName(book1)}》《${truncateBookName(book2)}》`;
            } else if (data.total > 2 && data.records && data.records.length >= 2) {
                const book1 = data.records[0].book_name || data.records[0].book_path.split('/').pop();
                const book2 = data.records[1].book_name || data.records[1].book_path.split('/').pop();
                recordInfo.textContent = `《${truncateBookName(book1)}》《${truncateBookName(book2)}》等${data.total}本书籍`;
            } else {
                recordInfo.textContent = `${data.total}本书籍`;
            }
        }

        } catch (error) {
            console.error('Failed to load reading records:', error);
            document.getElementById('loadingIndicator').style.display = 'none';
            const errorMsg = document.getElementById('errorMessage');
            errorMsg.textContent = `加载失败: ${error.message}`;
            errorMsg.style.display = 'block';
            return;
        }
    }
    
    // Process records (common for both debug and real mode)
    if (!data.records || data.records.length === 0) {
        showNoData();
        return;
    }

    // Aggregate all records
    const aggregatedData = aggregateRecords(data.records);
    aggregatedGlobalData = aggregatedData;
    currentDisplayLimit = 60;

    console.log('[Data Loaded]', {
        totalMinutes: aggregatedData.totalReadingMinutes,
        days: Object.keys(aggregatedData.dailyTotal).length,
        months: Object.keys(aggregatedData.monthlyTotal).length,
        hourlyDist: aggregatedData.hourlyDistribution
    });

    // Update main title with total reading time
    const totalHours = Math.floor(aggregatedData.totalReadingMinutes / 60);
    const totalMinutesRemainder = aggregatedData.totalReadingMinutes % 60;
    const mainTitle = document.getElementById('mainTitle');
    if (mainTitle) {
        mainTitle.textContent = `ReadPaper: 总阅读时间 ${totalHours} 小时 ${totalMinutesRemainder} 分`;
    }

    // Render visualizations (delay to ensure DOM layout is complete)
    renderLast7Days(aggregatedData);
    renderLast6Months(aggregatedData);
    setTimeout(() => renderHourlyDistribution(aggregatedData), 100);
    renderFullRecordList(aggregatedData);

    // bind load more
    const loadMoreBtn = document.getElementById('loadMoreBtn');
    if (loadMoreBtn) {
        loadMoreBtn.addEventListener('click', () => {
            currentDisplayLimit += DISPLAY_STEP;
            renderFullRecordList(aggregatedGlobalData);
        });
    }

})();

function showNoData() {
    document.getElementById('contentArea').innerHTML = '<div class="no-data">暂无阅读记录</div>';
    document.getElementById('contentArea').style.display = 'block';
}

function aggregateRecords(records) {
    const hourlyByDate = {}; // YYYYMMDD -> { HH: minutes }
    const dailyTotal = {}; // YYYYMMDD -> total minutes
    const monthlyTotal = {}; // YYYYMM -> total minutes
    const hourlyDistribution = new Array(24).fill(0); // 0-23 hours
    
    // Get total time from .bm file (via API's total_hours and total_minutes)
    // Sum up from all records
    let totalReadingMinutes = 0;

    for (const record of records) {
        // Use total_hours and total_minutes from .bm file if available
        if (record.total_hours !== undefined && record.total_minutes !== undefined) {
            totalReadingMinutes += record.total_hours * 60 + record.total_minutes;
        }
        
        // Process hourly records
        for (const [timestamp, minutes] of Object.entries(record.hourly_records || {})) {
            if (timestamp.length >= 10) {
                const date = timestamp.substring(0, 8); // YYYYMMDD
                const hour = parseInt(timestamp.substring(8, 10), 10); // HH
                if (!hourlyByDate[date]) {
                    hourlyByDate[date] = {};
                }
                hourlyByDate[date][hour] = (hourlyByDate[date][hour] || 0) + minutes;
                hourlyDistribution[hour] += minutes;
            }
        }

        // Process daily summary
        for (const [date, minutes] of Object.entries(record.daily_summary || {})) {
            dailyTotal[date] = (dailyTotal[date] || 0) + minutes;
        }

        // Process monthly summary
        for (const [month, minutes] of Object.entries(record.monthly_summary || {})) {
            monthlyTotal[month] = (monthlyTotal[month] || 0) + minutes;
        }
    }

    return {
        byDate: dailyTotal,  // Alias for compatibility
        byMonth: monthlyTotal,  // Alias for compatibility
        byHour: hourlyDistribution,  // Alias for compatibility
        hourlyByDate,
        dailyTotal,
        monthlyTotal,
        hourlyDistribution,
        totalReadingMinutes
    };
}

function renderLast7Days(data) {
    // 获取所有有阅读记录的日期，按时间排序（从旧到新）
    const allDates = Object.keys(data.dailyTotal)
        .filter(dateStr => data.dailyTotal[dateStr] > 0)
        .sort();
    
    // 取最后7天（实际有阅读的最后7天，可能不连续）
    const last7Days = allDates.slice(-7).map(dateStr => ({
        date: dateStr,
        minutes: data.dailyTotal[dateStr]
    }));

    const totalMinutes = last7Days.reduce((sum, day) => sum + day.minutes, 0);
    const daysWithReading = last7Days.length; // 都是有阅读的天数
    const avgMinutes = daysWithReading > 0 ? Math.round(totalMinutes / daysWithReading) : 0;

    document.getElementById('last7DaysTotal').textContent = totalMinutes;
    document.getElementById('last7DaysAvg').textContent = avgMinutes;
    document.getElementById('last7DaysDays').textContent = daysWithReading;

    // Render bars
    const barsContainer = document.getElementById('last7DaysBars');
    barsContainer.innerHTML = '';
    const maxMinutes = Math.max(...last7Days.map(d => d.minutes), 1);

    for (const day of last7Days) {
        const bar = document.createElement('div');
        bar.className = 'day-bar';
        const height = (day.minutes / maxMinutes) * 100;
        bar.style.height = `${height}%`;
        bar.title = `${day.date}: ${day.minutes}分钟`;
        
        // 显示日期标签
        const label = document.createElement('div');
        label.className = 'day-bar-label';
        label.textContent = day.date.substring(4, 6) + '-' + day.date.substring(6, 8); // MM-DD format
        bar.appendChild(label);

        // 显示分钟数
        const topVal = document.createElement('div');
        topVal.className = 'bar-top-label';
        topVal.textContent = `${day.minutes}m`;
        bar.appendChild(topVal);

        barsContainer.appendChild(bar);
    }
}

function renderLast6Months(data) {
    // 获取所有有阅读记录的月份，按时间排序（从旧到新）
    const allMonths = Object.keys(data.monthlyTotal)
        .filter(monthStr => data.monthlyTotal[monthStr] > 0)
        .sort();
    
    // 取最后6个月（实际有阅读的最后6个月，可能不连续）
    const last6Months = allMonths.slice(-6).map(monthStr => ({
        month: monthStr,
        minutes: data.monthlyTotal[monthStr]
    }));

    const totalMinutes = last6Months.reduce((sum, month) => sum + month.minutes, 0);
    const totalHours = Math.round(totalMinutes / 60);
    const monthsWithReading = last6Months.length; // 都是有阅读的月份
    const avgHours = monthsWithReading > 0 ? Math.round(totalMinutes / monthsWithReading / 60) : 0;

    document.getElementById('last6MonthsTotal').textContent = totalHours;
    document.getElementById('last6MonthsAvg').textContent = avgHours;
    document.getElementById('last6MonthsMonths').textContent = monthsWithReading;

    // Render bars
    const barsContainer = document.getElementById('last6MonthsBars');
    barsContainer.innerHTML = '';
    const maxMinutes = Math.max(...last6Months.map(m => m.minutes), 1);

    for (const month of last6Months) {
        const bar = document.createElement('div');
        bar.className = 'day-bar';
        const height = (month.minutes / maxMinutes) * 100;
        bar.style.height = `${height}%`;
        bar.title = `${month.month}: ${Math.round(month.minutes / 60)}小时`;
        
        // 显示月份标签（YY-MM格式）
        const label = document.createElement('div');
        label.className = 'day-bar-label';
        // month.month 格式为 YYYYMM，转换为 YY-MM
        const year = month.month.substring(2, 4); // YY
        const mon = month.month.substring(4, 6);  // MM
        label.textContent = `${year}-${mon}`;
        bar.appendChild(label);
        
        // 显示小时数
        const topVal = document.createElement('div');
        topVal.className = 'bar-top-label';
        topVal.textContent = `${Math.round(month.minutes/60)}h`;
        bar.appendChild(topVal);

        barsContainer.appendChild(bar);
    }
}

function renderHourlyDistribution(data) {
    console.log('[renderHourlyDistribution] Starting render...');
    
    const canvas = document.getElementById('hourlyChart');
    if (!canvas) {
        console.error('[renderHourlyDistribution] hourlyChart canvas not found');
        return;
    }
    
    console.log('[renderHourlyDistribution] Canvas element found:', canvas);
    
    const ctx = canvas.getContext('2d');
    
    // Get current theme color
    const rootStyles = getComputedStyle(document.documentElement);
    const primaryColor = rootStyles.getPropertyValue('--primary-color').trim();
    
    console.log('[renderHourlyDistribution] Primary color:', primaryColor);
    
    // Set canvas size
    const container = canvas.parentElement;
    console.log('[renderHourlyDistribution] Container width:', container.clientWidth);
    
    // If container width is 0, retry after a delay
    if (container.clientWidth === 0) {
        console.warn('[renderHourlyDistribution] Container width is 0, retrying in 200ms...');
        setTimeout(() => renderHourlyDistribution(data), 200);
        return;
    }
    
    // ensure canvas uses full container width and a taller height to avoid scrollbars
    canvas.width = container.clientWidth;
    canvas.style.width = '100%';
    canvas.height = 280;

    console.log('[renderHourlyDistribution] Canvas dimensions:', canvas.width, 'x', canvas.height);

    const padding = 50;
    const width = canvas.width - padding * 2;
    const height = canvas.height - padding * 2;

    // Clear canvas
    ctx.clearRect(0, 0, canvas.width, canvas.height);

    // Find max value
    const maxMinutes = Math.max(...data.hourlyDistribution, 1);

    console.log('[renderHourlyDistribution] Data:', {
        hourlyDistribution: data.hourlyDistribution,
        maxMinutes: maxMinutes
    });

    // Update Y labels in DOM as Max/Min
    const hourlyTop = document.getElementById('hourlyYTop');
    const hourlyMid = document.getElementById('hourlyYMid');
    // compute min non-zero for clarity
    const nonZero = data.hourlyDistribution.filter(v => v > 0);
    const minVal = nonZero.length > 0 ? Math.min(...nonZero) : 0;
    if (hourlyTop) hourlyTop.textContent = `Max: ${Math.round(maxMinutes)}min`;
    if (hourlyMid) hourlyMid.textContent = `Min: ${Math.round(minVal)}min`;

    console.log('[renderHourlyDistribution] Starting to draw...');

    // Draw axes
    ctx.strokeStyle = '#ccc';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(padding, padding);
    ctx.lineTo(padding, padding + height);
    ctx.lineTo(padding + width, padding + height);
    ctx.stroke();

    console.log('[renderHourlyDistribution] Axes drawn');

    // Draw hour labels every 3 hours
    ctx.fillStyle = '#666';
    ctx.font = '10px Arial';
    ctx.textAlign = 'center';
    for (let i = 0; i <= 24; i += 3) {
        const x = padding + (width / 24) * i;
        ctx.fillText(i.toString(), x, padding + height + 15);
    }

    console.log('[renderHourlyDistribution] Hour labels drawn');

    // Draw y-axis label
    ctx.save();
    ctx.translate(15, padding + height / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.textAlign = 'center';
    ctx.fillText('阅读时长(分钟)', 0, 0);
    ctx.restore();

    // Draw max and min tick markers on canvas left side
    ctx.fillStyle = '#666';
    ctx.font = '12px Arial';
    ctx.textAlign = 'right';
    // max value at top
    ctx.fillText(String(Math.round(maxMinutes)), padding - 8, padding + 6);
    // min value at bottom area (use previously computed minVal)
    ctx.fillText(String(Math.round(minVal)), padding - 8, padding + height - 4);

    // Draw curve using theme color
    ctx.strokeStyle = primaryColor;
    ctx.lineWidth = 2;
    ctx.beginPath();

    for (let hour = 0; hour < 24; hour++) {
        const x = padding + (width / 24) * hour + (width / 48);
        const y = padding + height - (data.hourlyDistribution[hour] / maxMinutes) * height;
        
        if (hour === 0) {
            ctx.moveTo(x, y);
        } else {
            ctx.lineTo(x, y);
        }
    }
    ctx.stroke();

    // Draw points using theme color
    ctx.fillStyle = primaryColor;
    for (let hour = 0; hour < 24; hour++) {
        const x = padding + (width / 24) * hour + (width / 48);
        const y = padding + height - (data.hourlyDistribution[hour] / maxMinutes) * height;
        
        ctx.beginPath();
        ctx.arc(x, y, 3, 0, Math.PI * 2);
        ctx.fill();
    }

    // Draw fill area with theme color and transparency
    // Convert hex to rgba
    const hexToRgb = (hex) => {
        const result = /^#?([a-f\d]{2})([a-f\d]{2})([a-f\d]{2})$/i.exec(hex);
        return result ? {
            r: parseInt(result[1], 16),
            g: parseInt(result[2], 16),
            b: parseInt(result[3], 16)
        } : {r: 240, g: 79, b: 15}; // fallback to orange
    };
    const rgb = hexToRgb(primaryColor);
    ctx.fillStyle = `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, 0.1)`;
    ctx.beginPath();
    ctx.moveTo(padding, padding + height);
    for (let hour = 0; hour < 24; hour++) {
        const x = padding + (width / 24) * hour + (width / 48);
        const y = padding + height - (data.hourlyDistribution[hour] / maxMinutes) * height;
        ctx.lineTo(x, y);
    }
    ctx.lineTo(padding + width, padding + height);
    ctx.closePath();
    ctx.fill();
    
    console.log('[renderHourlyDistribution] Rendering complete!');
}

function renderFullRecordList(data) {
    const listContainer = document.getElementById('recordList');
    listContainer.innerHTML = '';
    // Sort dates in descending order and filter days that have reading minutes
    const filteredDates = Object.keys(data.dailyTotal).filter(d => (data.dailyTotal[d] || 0) > 0).sort().reverse();

    if (filteredDates.length === 0) {
        listContainer.innerHTML = '<div class="no-data">暂无记录</div>';
        document.getElementById('loadMoreWrap').style.display = 'none';
        return;
    }

    // Determine slice according to currentDisplayLimit
    const displayDates = filteredDates.slice(0, currentDisplayLimit);

    for (const date of displayDates) {
        const totalMinutes = data.dailyTotal[date];
        const item = document.createElement('div');
        item.className = 'record-item';

        const dateHeader = document.createElement('div');
        dateHeader.className = 'record-date';
        dateHeader.textContent = `${formatDisplayDate(date)} - ${totalMinutes}分钟 (${Math.round(totalMinutes / 60 * 10) / 10}小时)`;
        item.appendChild(dateHeader);

        // Hourly breakdown
        if (data.hourlyByDate[date]) {
            const hoursDiv = document.createElement('div');
            hoursDiv.className = 'record-hours';

            for (let hour = 0; hour < 24; hour++) {
                const minutes = data.hourlyByDate[date][hour] || 0;
                const bar = document.createElement('div');
                bar.className = 'hour-bar';
                
                let opacity = 1;
                if (minutes > 0) {
                    bar.classList.add('has-reading');
                    opacity = Math.max(0.3, Math.min(minutes / 60, 1));
                    bar.style.opacity = opacity;
                }

                const label = document.createElement('div');
                label.className = 'hour-label';
                // 不再在有阅读记录时添加 'h' 后缀，统一显示小时数字
                label.textContent = `${hour}`;
                
                // If opacity < 0.4, use black text for better readability
                if (minutes > 0 && opacity < 0.4) {
                    label.style.color = 'white';
                }
                
                bar.appendChild(label);

                if (minutes > 0) {
                    bar.title = `${hour}:00 - ${minutes}分钟`;
                }

                hoursDiv.appendChild(bar);
            }

            item.appendChild(hoursDiv);
        }

        listContainer.appendChild(item);
    }

    // Load more control
    const loadMoreWrap = document.getElementById('loadMoreWrap');
    if (filteredDates.length > displayDates.length) {
        loadMoreWrap.style.display = 'block';
        const remain = filteredDates.length - displayDates.length;
        const btn = document.getElementById('loadMoreBtn');
        if (btn) btn.textContent = `获取更多 (${Math.min(DISPLAY_STEP, remain)} / ${remain})`;
    } else {
        loadMoreWrap.style.display = 'none';
    }
}

function formatDateToYYYYMMDD(date) {
    const year = date.getFullYear();
    const month = String(date.getMonth() + 1).padStart(2, '0');
    const day = String(date.getDate()).padStart(2, '0');
    return `${year}${month}${day}`;
}

function formatDateToYYYYMM(date) {
    const year = date.getFullYear();
    const month = String(date.getMonth() + 1).padStart(2, '0');
    return `${year}${month}`;
}

function formatDisplayDate(yyyymmdd) {
    if (yyyymmdd.length !== 8) return yyyymmdd;
    const year = yyyymmdd.substring(0, 4);
    const month = yyyymmdd.substring(4, 6);
    const day = yyyymmdd.substring(6, 8);
    return `${year}-${month}-${day}`;
}

// ==================== Theme Management ====================

const THEMES = [
    { name: 'orange', color: '#f04f0f', label: '橙红' },
    { name: 'blue', color: '#1976d2', label: '蓝色' },
    { name: 'green', color: '#2e7d32', label: '绿色' },
    { name: 'purple', color: '#7b1fa2', label: '紫色' },
    { name: 'teal', color: '#00796b', label: '青色' }
];

let currentThemeIndex = 0;

function applyTheme(themeColor) {
    document.documentElement.style.setProperty('--primary-color', themeColor);
    
    // Update all primary color elements
    const elements = [
        '.back-btn', '.load-more-btn', '.action-btn',
        '.summary-section h2', '.stat-value',
        '.day-bar', '.hour-bar.has-reading',
        '.progress-fill', '.record-item'
    ];
    
    // Re-render hourly chart with new color if data is available
    if (aggregatedGlobalData && aggregatedGlobalData.hourlyDistribution) {
        renderHourlyDistribution(aggregatedGlobalData);
    }
    
    // For dynamic updates, we'll use inline styles or class changes
    // Store theme in localStorage
    localStorage.setItem('readingRecordTheme', themeColor);
}

function toggleTheme() {
    currentThemeIndex = (currentThemeIndex + 1) % THEMES.length;
    const theme = THEMES[currentThemeIndex];
    applyTheme(theme.color);
    
    // Show toast notification
    showToast(`已切换至${theme.label}主题`);
}

function loadTheme() {
    const savedTheme = localStorage.getItem('readingRecordTheme');
    if (savedTheme) {
        applyTheme(savedTheme);
        // Find theme index
        const index = THEMES.findIndex(t => t.color === savedTheme);
        if (index >= 0) currentThemeIndex = index;
    } else {
        applyTheme(THEMES[0].color);
    }
}

function showToast(message) {
    const toast = document.createElement('div');
    toast.style.cssText = `
        position: fixed;
        top: 20px;
        right: 20px;
        background: rgba(0,0,0,0.8);
        color: white;
        padding: 12px 20px;
        z-index: 10000;
        font-size: 0.95rem;
        box-shadow: 0 4px 12px rgba(0,0,0,0.3);
        animation: slideIn 0.3s ease;
    `;
    toast.textContent = message;
    document.body.appendChild(toast);
    
    setTimeout(() => {
        toast.style.animation = 'slideOut 0.3s ease';
        setTimeout(() => toast.remove(), 300);
    }, 2000);
}

// ==================== Image Export ====================

async function generateExportImage(mode) {
    // Check if data is loaded
    if (!aggregatedGlobalData) {
        showToast('数据未加载，请稍后再试');
        return;
    }
    
    // Check if html2canvas is loaded
    if (typeof html2canvas === 'undefined') {
        showToast('图片生成库未加载，请刷新页面重试');
        return;
    }
    
    console.log('[Export Image] Mode:', mode);
    
    // Create a container for export
    const exportContainer = document.createElement('div');
    exportContainer.style.cssText = `
        position: fixed;
        left: -9999px;
        top: 0;
        width: 1200px;
        background: #f5f5f5;
        padding: 0;
    `;
    document.body.appendChild(exportContainer);
    
    try {
        // Get the header
        const header = document.querySelector('.header');
        const sections = document.querySelectorAll('.summary-section');
        
        if (!header || sections.length === 0) {
            showToast('页面元素未准备好，请稍后再试');
            return;
        }
        
        // Clone header
        const headerClone = header.cloneNode(true);
        // Hide header actions (buttons) in export
        const headerActions = headerClone.querySelector('.header-actions');
        if (headerActions) {
            headerActions.style.display = 'none';
        }
        exportContainer.appendChild(headerClone);
        
        // Determine which sections to include based on mode
        let sectionsToInclude = [];
        switch(mode) {
            case 'brief':
                // 简单: section[0] + section[1] (最近7天 + 最近6个月)
                sectionsToInclude = [0, 1];
                break;
            case 'standard':
                // 标准: section[0] + section[1] + section[2] (最近7天 + 最近6个月 + 24小时)
                sectionsToInclude = [0, 1, 2];
                break;
            case 'detailed':
                // 详细: section[0] + section[1] + section[2] + section[3] (当前可见的所有record-item)
                sectionsToInclude = [0, 1, 2, 3];
                break;
        }
        
        // Clone sections
        sectionsToInclude.forEach(index => {
            if (sections[index]) {
                const sectionClone = sections[index].cloneNode(true);
                
                // For detailed mode, hide load more button in export
                if (mode === 'detailed' && index === 3) {
                    const loadMoreWrap = sectionClone.querySelector('.load-more-wrap');
                    if (loadMoreWrap) loadMoreWrap.remove();
                }
                
                // Copy canvas content if exists (for 24-hour chart)
                const originalCanvas = sections[index].querySelector('canvas');
                const clonedCanvas = sectionClone.querySelector('canvas');
                if (originalCanvas && clonedCanvas) {
                    const ctx = clonedCanvas.getContext('2d');
                    clonedCanvas.width = originalCanvas.width;
                    clonedCanvas.height = originalCanvas.height;
                    ctx.drawImage(originalCanvas, 0, 0);
                }
                
                exportContainer.appendChild(sectionClone);
            }
        });
        
        // Wait a bit for styles to apply
        await new Promise(resolve => setTimeout(resolve, 100));
        
        // Generate image using html2canvas
        const canvas = await html2canvas(exportContainer, {
            backgroundColor: '#f5f5f5',
            scale: 2, // Higher quality
            logging: false,
            useCORS: true,
            allowTaint: true
        });
        
        // Download
        canvas.toBlob(blob => {
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = `阅读记录_${mode}_${Date.now()}.png`;
            a.click();
            URL.revokeObjectURL(url);
            showToast('图片已下载');
        });
        
    } catch (error) {
        console.error('Export failed:', error);
        showToast('图片生成失败: ' + error.message);
    } finally {
        // Clean up
        exportContainer.remove();
    }
}


// ==================== Event Listeners ====================

document.addEventListener('DOMContentLoaded', () => {
    // Adjust back button based on source
    const urlParams = new URLSearchParams(window.location.search);
    const src = urlParams.get('src');
    const backBtn = document.querySelector('.back-btn');
    if (backBtn) {
        if (src === 'local') {
            backBtn.href = 'data_center.html';
            backBtn.textContent = '← 返回数据中心';
        } else {
            backBtn.href = 'filemanager.html';
            backBtn.textContent = '← 返回文件管理';
        }
    }
    
    // Load theme on startup
    loadTheme();
    
    // Theme toggle button
    const themeBtn = document.getElementById('themeToggleBtn');
    if (themeBtn) {
        themeBtn.addEventListener('click', toggleTheme);
    }
    
    // Export button
    const exportBtn = document.getElementById('exportBtn');
    const exportModal = document.getElementById('exportModal');
    const modalCloseBtn = document.getElementById('modalCloseBtn');
    
    if (exportBtn && exportModal) {
        exportBtn.addEventListener('click', () => {
            exportModal.classList.add('active');
        });
    }
    
    if (modalCloseBtn && exportModal) {
        modalCloseBtn.addEventListener('click', () => {
            exportModal.classList.remove('active');
        });
        
        // Close on overlay click
        exportModal.addEventListener('click', (e) => {
            if (e.target === exportModal) {
                exportModal.classList.remove('active');
            }
        });
    }
    
    // Export options
    const exportOptions = document.querySelectorAll('.export-option');
    exportOptions.forEach(option => {
        option.addEventListener('click', () => {
            const mode = option.getAttribute('data-mode');
            exportModal.classList.remove('active');
            showToast('正在生成图片...');
            setTimeout(() => generateExportImage(mode), 100);
        });
    });
});
