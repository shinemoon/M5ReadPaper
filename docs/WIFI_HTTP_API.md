# ReaderPaper — WiFi 热点 HTTP API（Web 前端开发者参考）

- **说明**: 本文档面向前端工程师，汇总设备中用于 Web 前端交互的全部 HTTP 接口（由 `WiFiHotspotManager` 和 `ApiRouter` 提供）。包含请求方法、必需/可选参数、响应格式、示例（curl / fetch / XHR）以及前端实现注意事项。
- **设备默认热点地址**: `http://192.168.4.1`（在 AP 模式下，`WiFiHotspotManager::getIPAddress()` 返回热点地址）

---

## 通用注意

**CORS**  
服务端会在所有 API 路由上自动添加以下响应头：
```
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, OPTIONS, DELETE
Access-Control-Allow-Headers: Content-Type, X-Requested-With
```

**OPTIONS 预检**  
所有路由都有对应的 `HTTP_OPTIONS` 处理，返回 `204 No Content`。浏览器跨域复杂请求会自动触发预检，前端无需额外处理。

**辅助路由**  
- `/favicon.ico` → 204（避免浏览器 404 日志）

**通用错误码**

| 状态码 | 含义 |
|--------|------|
| 200 | 成功（返回 JSON 或文件流） |
| 204 | 无内容（用于 OPTIONS 预检） |
| 400 | 客户端错误（缺少参数、非法路径、重名等） |
| 404 | 资源未找到 |
| 408 | 上传超时（300 秒）|
| 413 | 文件/Chunk 过大 |
| 500 | 服务器内部错误（写入/重命名失败） |
| 507 | 存储或内存不足 |

---

## 接口列表

| # | 路径 | 方法 | 功能 |
|---|------|------|------|
| 1 | `/list` `/list/book` `/list/font` `/list/image` `/list/screenshot` | GET | 获取文件列表 |
| 2 | `/upload` | GET / POST | 上传文件 |
| 3 | `/download` | GET | 下载文件 |
| 4 | `/delete` | GET | 删除文件（及伴随文件） |
| 5 | `/rename` | GET | 重命名书籍（及伴随文件）|
| 6 | `/sync_time` | POST | 同步设备时间 |
| 7 | `/heartbeat` | GET | 健康检查 / 版本信息 |
| 8 | `/api/device_guide` `/api/guide` | GET | 设备管理页面 guide（模块开关+文件页签能力，`/api/guide` 为归一化别名） |
| 9 | `/api/reading_records` | GET | 查询阅读记录 |
| 10 | `/api/webdav_config` | GET / POST | 读写 WebDAV 配置 |
| 11 | `/api/wifi_config` | GET / POST | 读写 WiFi 连接配置 |
| 12 | `/api/update_display` | POST | 单次推送显示内容（PNG + RDT） |
| 13 | `/api/update_display_start` | POST | 分块推送：初始化 |
| 14 | `/api/update_display_chunk` | POST | 分块推送：追加数据块 |
| 15 | `/api/update_display_commit` | POST | 分块推送：提交（原子替换） |
| 16 | `/api/advconfig` | GET / POST | 读写高级可定制配置项 |

---

## 1) 获取文件列表 — `/list`

**方法**: GET

**路径变体**:
- `GET /list` — 列出所有文件
- `GET /list/book` — 仅列出书籍（`/book/` 目录，过滤非 `.txt`）
- `GET /list/font` — 仅列出字体
- `GET /list/image` — 仅列出图片
- `GET /list/screenshot` — 仅列出截图

**Query 参数（可选）**:
- `page` (整数) — 页码（从 1 开始），与 `perPage` 配对使用
- `perPage` (整数) — 每页条目数；两个参数均大于 0 时启用分页

**响应**:

无分页时，返回 JSON 数组：
```json
[
  {
    "name": "书名（最长约 60 字符，超长追加 ...）",
    "type": "file",
    "size": 123456,
    "isCurrent": 0,
    "isIdxed": 1,
    "path": "/book/example.txt"
  }
]
```

启用分页时，返回对象：
```json
{ "total": 42, "page": 1, "perPage": 20, "files": [ ... ] }
```

**示例**:
```bash
curl "http://192.168.4.1/list/book"
curl "http://192.168.4.1/list/book?page=1&perPage=20"
```
```js
const res = await fetch('http://192.168.4.1/list/book');
const data = await res.json();
// data 可能是数组或分页对象，用 Array.isArray(data) 区分
```

---

## 2) 上传文件 — `/upload`

**方法**: GET（返回上传页 HTML）| POST（multipart/form-data 上传）

**POST 表单字段**:

| 字段 | 必需 | 说明 |
|------|------|------|
| `file` | 是 | 要上传的文件（multipart） |
| `tab` | 否 | 目标目录：`book` → `/book/`；`font` → `/font/`；`image` → `/image/`；`scback` → `/scback.png`（锁屏背景，强制文件名）；默认 → `/` 根目录 |

**实现细节**:
- 流式写入：先写临时路径（目标路径 + `.tmp`），完成后重命名覆盖；若目标已存在先备份为 `.upload.bak`。
- 内存检查：上传开始需 ≥32 KB 堆；写入阶段需 ≥24 KB；验证阶段 <16 KB 时跳过验证。
- 存储检查：SD 剩余空间不足（预留约 10 MB）时返回 507。
- 超时：300 秒，超时返回 408。
- 大小限制：单文件最大 50 MB，超过返回 413。
- 上传后副作用：覆盖当前阅读书籍触发重建索引；上传字体刷新字体列表；上传图片使锁屏缓存失效。

**响应**:
```json
{"ok": true, "message": "File uploaded successfully"}
```

**示例**:
```bash
curl -F "tab=book" -F "file=@/path/to/book.txt" http://192.168.4.1/upload
curl -F "tab=scback" -F "file=@bg.png" http://192.168.4.1/upload
```
```js
const fd = new FormData();
fd.append('tab', 'book');
fd.append('file', file, file.name);
const res = await fetch('http://192.168.4.1/upload', { method: 'POST', body: fd });
const json = await res.json();
```

**上传进度（XHR）**:
```js
const xhr = new XMLHttpRequest();
xhr.open('POST', 'http://192.168.4.1/upload');
xhr.upload.onprogress = e => {
  if (e.lengthComputable) console.log('进度', (e.loaded / e.total * 100).toFixed(1) + '%');
};
xhr.onload = () => console.log('完成', xhr.responseText);
const fd = new FormData();
fd.append('tab', 'book');
fd.append('file', file);
xhr.send(fd);
```

> **注意**: 不要手动设置 `Content-Type`，浏览器会自动为 multipart 设置 boundary。

---

## 3) 下载文件 — `/download`

**方法**: GET

**Query 参数**:
- `path` (必需) — 文件路径，例如 `/book/example.txt`

**响应**: 文件流，带 `Content-Type` 与 `Content-Disposition: attachment; filename="..."`。

**示例**:
```bash
curl "http://192.168.4.1/download?path=/book/book.txt" -o book.txt
```
```js
const res = await fetch('http://192.168.4.1/download?path=/book/book.txt');
if (res.ok) {
  const blob = await res.blob();
  const url = URL.createObjectURL(blob);
  // 创建 <a download> 或直接打开
}
```

---

## 4) 删除文件 — `/delete`

**方法**: GET

**Query 参数**:
- `path` (必需) — 要删除的文件路径

**响应**:
```json
{"ok": true, "message": "File deleted successfully"}
```

**保护机制与副作用**:
- 不允许删除当前正在阅读的书籍（返回 400）。
- 删除书籍时同步清理伴随文件：`.idx`、`/bookmarks/` 下的 `.bm`、`.page`、`.progress`、`.complete`、`.rec`、`.tags`；并刷新书籍缓存。

**示例**:
```bash
curl "http://192.168.4.1/delete?path=/book/book.txt"
```

---

## 5) 重命名书籍 — `/rename`

**方法**: GET

**Query 参数**:
- `old_path` (必需) — 原始文件路径，必须以 `/book/` 开头（示例：`/book/old_name.txt`）
- `new_name` (必需) — 新文件名（仅文件名，不含路径分隔符），必须以 `.txt` 结尾，最长 64 字符

**响应**:
```json
{"ok": true, "message": "File renamed successfully"}
```

服务端在主文件重命名成功后**立即返回 200**，随后异步完成以下副作用操作：

**副作用（异步）**:
1. 重命名同目录 `.idx` 索引文件
2. 重命名 `/bookmarks/` 下所有伴随文件（`.bm`、`.page`、`.progress`、`.complete`、`.rec`、`.tags`）及其 `.tmp` 副本（copy+delete 兜底，确保可靠性）
3. 更新 `.bm` 和 `.progress` 文件内的 `file_path=` 字段
4. 更新 `/history.list` 中的书籍路径记录
5. 若正在阅读该书籍：同步更新 `g_config.currentReadFile`、保存配置文件（`readpaper.cfg.A/B`）

**错误码**:
| 状态码 | 场景 |
|--------|------|
| 400 | 缺少参数 / 非 `/book/` 目录 / 文件名含路径分隔符 / 名称过长或非 `.txt` / 目标文件名已存在 |
| 404 | 源文件不存在 |
| 500 | 底层重命名失败 |

**示例**:
```bash
curl "http://192.168.4.1/rename?old_path=/book/old.txt&new_name=new.txt"
```
```js
const res = await fetch(
  `http://192.168.4.1/rename?old_path=${encodeURIComponent('/book/old.txt')}&new_name=${encodeURIComponent('new.txt')}`
);
const json = await res.json();
```

> **注意**: webapp 中 IndexedDB 阅读时长记录以书籍文件名为 key，重命名后旧记录**不会**自动迁移，新书名在 webapp 统计页面将从零开始计数。

---

## 6) 时间同步 — `/sync_time`

**方法**: POST

**请求体**: 文本或 JSON，服务端使用字符串检索（非严格 JSON 解析）提取 `timestamp` 和可选的 `tzOffsetMinutes` 字段。

**功能**: 调用 `settimeofday` 同步设备时间，返回本地时间可读字符串。

**示例**:
```bash
curl -X POST -d '{"timestamp": 1700000000, "tzOffsetMinutes": 480}' http://192.168.4.1/sync_time
```

---

## 7) 健康检查 / 版本信息 — `/heartbeat`

**方法**: GET

**响应**:
```json
{"status": "ok", "hw": "PaperS3", "firmware": "1.0", "version": "1.0.0"}
```

服务端从 SPIFFS `/version` 文件读取前三行（hw、firmware、version），文件不存在时使用内置默认值。

**使用场景**: 页面加载时 ping 此接口，判断设备是否就绪并展示版本号。

**示例**:
```bash
curl "http://192.168.4.1/heartbeat"
```

---

## 8) 设备管理 Guide — `/api/device_guide` / `/api/guide`

**方法**: GET

**用途**:
- 提供设备管理页面的能力声明。
- 前端据此决定三大模块（文件管理/时间管理/设置管理）是否显示。
- 文件管理页签、提示文案、是否支持目录层级、是否支持新建目录/重命名/阅读记录等均由该接口返回。
- 推荐优先使用 `/api/guide`，`/api/device_guide` 保留兼容。

**响应示例**（字段可扩展）：
```json
{
  "ok": true,
  "schema_version": 1,
  "device": {
    "hw": "M5Stack PaperS3",
    "firmware": "ReadPaper",
    "version": "V1.3",
    "ip": "192.168.4.1",
    "wifi_sta_connected": false,
    "wifi_ap_clients": 1,
    "upload_in_progress": false,
    "current_book": "/sd/book/demo.txt"
  },
  "sections": {
    "file_management": true,
    "time_management": true,
    "settings_management": true
  },
  "modules": {
    "file": true,
    "time": true,
    "settings": true
  },
  "fileManagement": {
    "required": true,
    "tabs": [
      {
        "id": "book",
        "apiTab": "book",
        "title": "书籍",
        "hint": "支持 unicode/GBK 编码的 txt 文件。",
        "supportsHierarchy": true,
        "allowUpload": true,
        "allowDelete": true,
        "allowRename": true,
        "allowMkdir": true,
        "allowReadingRecords": true,
        "showIdxBadge": true
      }
    ]
  },
  "timeManagement": {
    "enabled": true,
    "allowSyncTime": true,
    "allowReadingRecordsExport": true
  },
  "settingsManagement": {
    "enabled": true,
    "allowWifiConfig": true,
    "allowWebdavConfig": true,
    "hasWifiConfig": false,
    "hasWebdavConfig": false
  },
  "endpoints": {
    "heartbeat": "/heartbeat",
    "list": "/list",
    "upload": "/upload",
    "download": "/download",
    "delete": "/delete",
    "rename": "/rename",
    "mkdir": "/mkdir",
    "sync_time": "/sync_time",
    "reading_records": "/api/reading_records",
    "wifi_config": "/api/wifi_config",
      "webdav_config": "/api/webdav_config",
      "advconfig": "/api/advconfig",
      "device_guide": "/api/device_guide",
      "guide": "/api/guide"
    }
}
```

    兼容字段说明：
    - 现有前端继续使用 camelCase：`fileManagement`、`timeManagement`、`settingsManagement`。
    - 归一化客户端可使用 snake_case：`file_management`、`time_management`、`settings_management`。

    `timeManagement` 字段说明：
    - `enabled`: 是否启用时间管理模块页签。
    - `allowSyncTime`: 控制“同步设备时间”功能显示与可用。
    - `allowReadingRecordsExport`: 控制“下载阅读记录”入口显示与可用。
    - `allowSyncTime` 与 `allowReadingRecordsExport` 必须独立生效，禁止相互依赖。
    - 兼容默认值：当字段缺失时，前端按 `true` 处理（仅显式 `false` 才关闭）。

**前端建议**:
- `heartbeat` 用于在线探测；`/api/device_guide` 用于页面能力驱动。
- 若 guide 获取失败，可回退到本地默认配置（例如 ReadPaper 既有四个文件分类）。

---

## 9) 阅读记录 — `/api/reading_records`

**方法**: GET

**Query 参数**:
- `book` — 单本书路径（示例：`/book/example.txt`）
- `books` — 逗号分隔的多本书路径
- 无参数 — 扫描 `/bookmarks/` 目录，返回全部 `.rec` 记录

**响应**:
```json
{
  "total": 5,
  "records": [
    {
      "book_path": "/sd/book/example.txt",
      "book_name": "example.txt",
      "total_hours": 2,
      "total_minutes": 135,
      "hourly_records": { ... },
      "daily_summary": { ... },
      "monthly_summary": { ... }
    }
  ],
  "processed": 5
}
```

若某本书的 `.rec` 文件缺失或解析失败，对应条目中会包含 `error` 字段。

**示例**:
```bash
curl "http://192.168.4.1/api/reading_records"
curl "http://192.168.4.1/api/reading_records?book=/book/example.txt"
curl "http://192.168.4.1/api/reading_records?books=/book/a.txt,/book/b.txt"
```

---

## 10) WebDAV 配置 — `/api/webdav_config`

**GET** — 读取当前 WebDAV 配置

**响应**:
```json
{
  "ok": true,
  "config": {
    "url": "https://example.com/dav/",
    "username": "user",
    "password": "pass"
  }
}
```

**POST** — 更新 WebDAV 配置

**请求体（JSON）**:
```json
{
  "config": {
    "url": "https://example.com/dav/",
    "username": "user",
    "password": "pass"
  }
}
```
也支持根级字段：`{"url": "...", "username": "...", "password": "..."}`（与 `config` 对象等效，根级优先）。

**响应**: 同 GET，包含保存后的配置。失败时返回 500 并带 `"message": "save failed"`。

**示例**:
```bash
curl "http://192.168.4.1/api/webdav_config"
curl -X POST -H "Content-Type: application/json" \
  -d '{"config":{"url":"https://dav.example.com/","username":"u","password":"p"}}' \
  http://192.168.4.1/api/webdav_config
```

---

## 11) WiFi 连接配置 — `/api/wifi_config`

**GET** — 读取当前 WiFi 配置（支持最多 3 组 SSID/密码）

**响应**:
```json
{
  "ok": true,
  "configs": [
    {"ssid": "MyWifi", "password": "pass1"},
    {"ssid": "", "password": ""},
    {"ssid": "", "password": ""}
  ],
  "last_success_idx": 0
}
```

**POST** — 更新 WiFi 配置

**请求体（新格式，推荐）**:
```json
{
  "configs": [
    {"ssid": "Wifi1", "password": "pass1"},
    {"ssid": "Wifi2", "password": "pass2"},
    {"ssid": "", "password": ""}
  ]
}
```

**请求体（旧格式，兼容）**:
```json
{"ssid": "MyWifi", "password": "mypass"}
```
或 `{"config": {"ssid": "...", "password": "..."}}`（写入第 0 组）。

**响应**: 同 GET，包含保存后的全部配置。失败时返回 500。

---

## 16) 高级可定制配置 — `/api/advconfig`

**方法**: GET / POST

**用途**: 读写设备上可用的高级配置项（任意嵌套键值字典）。前端通过该接口实现"高级设置"面板——仅当接口返回非空配置字典时才显示入口，接口不存在或返回空则静默隐藏。

### GET — 读取当前高级配置

**响应**（字段内容因固件版本而异）：
```json
{
  "ok": true,
  "config": {
    "reading": {
      "font_size": { "title": "字体大小", "value": 2, "options": ["16", "20", "24", "28"], "hint": "选择字体像素大小" },
      "invert": { "title": "反转显示", "value": false, "hint": "颜色反转" }
    },
    "system": {
      "contrast": { "title": "对比度", "value": 3, "options": ["低", "中低", "中", "中高", "高"], "hint": "调整屏幕对比度" },
      "auto_sleep_min": { "title": "自动睡眠时间", "value": 15, "hint": "单位:分钟,0表示禁用" }
    },
    "text": {
      "encoding": { "title": "文本编码", "value": 0, "options": ["UTF-8", "GBK", "GB2312"], "hint": "选择默认文本编码" },
      "experimental_layout": { "title": "实验性布局", "value": true, "hint": "启用实验性界面布局" }
    }
  }
}
```

- 若设备无高级配置（或功能不可用），可返回 `{"ok": true, "config": {}}` 或直接 404。
- 前端行为：`config` 为空对象 / null / 空数组 / 404 均视为"无高级设置"，隐藏入口。
- **分组标题本地化**：前端会将 `reading` / `system` / `text` 自动显示为「阅读设置」/「系统设置」/「文本设置」；其他 key 原样显示。

### POST — 保存高级配置

**请求体（JSON）**：
```json
{
  "config": {
    "reading": {
      "font_size": { "title": "字体大小", "value": 2, "options": ["16", "20", "24", "28"], "hint": "选择字体像素大小" },
      "invert": { "title": "反转显示", "value": false, "hint": "颜色反转" }
    },
    "system": {
      "auto_sleep_min": { "title": "自动睡眠时间", "value": 30, "hint": "单位:分钟,0表示禁用" }
    }
  }
}
```

**响应**：保存成功后返回写入的配置（与 GET 格式相同）：
```json
{
  "ok": true,
  "config": { ... }
}
```

失败时返回 500 并带 `"message"` 字段。

**配置字段结构**（每个配置项都必须是字典形式）:

```js
"field_name": {
  "title": "字段显示名称",
  "value": 实际值,
  "options": ["选项A", "选项B"],  // 可选，枚举类型时提供
  "hint": "鼠标悬停提示"
}
```

| 属性 | 类型 | 说明 |
|------|------|------|
| `title` | string | 前端显示的中文名称 |
| `value` | boolean / number / string | 实际值；枚举类型时为选项索引（整数） |
| `options` | string[] | **可选**。存在时 `value` 为该数组的索引，前端渲染为下拉框 |
| `hint` | string | 可选，鼠标悬停时的提示信息 |

**值类型与前端控件对应**:

| `value` 类型 | `options` 存在 | 前端控件 |
|-------------|----------------|----------|
| `boolean` | — | 复选框 |
| `number` | 否 | 数字输入框 |
| `number` | 是 | 下拉框（`options[value]` 为当前选项文本）|
| `string` | — | 文本输入框 |

**示例**:
```bash
curl "http://192.168.4.1/api/advconfig"
curl -X POST -H "Content-Type: application/json" \
  -d '{"config":{"system":{"auto_sleep_min":{"title":"自动睡眠时间","value":30,"hint":"单位:分钟,0表示禁用"}}}}' \
  http://192.168.4.1/api/advconfig
```
```js
// 读取
const r = await fetch('http://192.168.4.1/api/advconfig');
const { config } = await r.json();
// 保存（部分或全量均可）
await fetch('http://192.168.4.1/api/advconfig', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ config }),
});
```

> **注意**: 设备端当前暂未实现此接口（返回 404 是合法行为）。webapp 在收到任何非 2xx 状态码或空配置时静默忽略，不显示高级设置入口。

---

## 12) 单次推送显示内容 — `/api/update_display`

**方法**: POST

**说明**: 将 PNG 图像和 RDT 布局数据一次性推送到设备，适合小图（<= ~1 MB 解码后二进制）。对于大图请使用分块接口（12–14）。

**请求体（JSON）**:
```json
{
  "png_base64": "<Base64 编码的 PNG 数据>",
  "rdt": "<RDT 布局文本>"
}
```

- `png_base64`：PNG 文件的 Base64 字符串（限制：Base64 长度 ≤ 1,500,000 字符，约对应 ~1.1 MB 二进制）
- `rdt`：RDT 布局文本

**响应**:
```json
{"ok": true, "message": "Display updated"}
```

**文件落盘**:
- PNG 解码后写入 `/rdt/readpaper.png`（原子写，经 `.tmp` 临时文件）
- RDT 写入 `/rdt/readpaper.rdt`

**错误码**: 400（缺字段）、413（png_base64 过大）、500（内存不足或写入失败）

---

## 13) 分块推送：初始化 — `/api/update_display_start`

**方法**: POST

**说明**: 开始一次分块上传，初始化（清空）目标临时文件。

**请求体（JSON）**:
```json
{"type": "rdt"}
```
或
```json
{"type": "png"}
```

- `type`：`"rdt"` 或 `"png"`

**响应**:
```json
{"ok": true}
```

**对应临时路径**:
- `rdt` → `/rdt/readpaper.rdt.upload`
- `png` → `/rdt/readpaper.png.upload`

---

## 14) 分块推送：追加数据块 — `/api/update_display_chunk`

**方法**: POST

**说明**: 向临时文件追加一个数据块；可调用多次，直到所有数据发送完毕。

**请求体（JSON）**:
```json
{"type": "rdt", "data": "<文本内容>"}
```
```json
{"type": "png", "data": "<Base64 编码的二进制块>"}
```

- `type`：`"rdt"` 或 `"png"`
- `data`：
  - `rdt` 类型：原始文本，直接追加
  - `png` 类型：Base64 字符串，服务端实时解码后追加二进制到临时文件
- 单块 `data` 长度限制：≤ 16,384 字符（`"Chunk too large"` → 413）

**响应**:
```json
{"ok": true}
```

---

## 15) 分块推送：提交 — `/api/update_display_commit`

**方法**: POST

**说明**: 将临时文件原子重命名为最终路径，完成本次推送。若 `type` 为 `rdt`，额外调用 `cache_clear_history()` 清空历史布局缓存。

**请求体（JSON）**:
```json
{"type": "rdt"}
```
或
```json
{"type": "png"}
```

**响应**:
```json
{"ok": true, "message": "Saved"}
```

**错误码**: 400（临时文件不存在 / 参数无效）、500（重命名失败）

**分块上传完整流程示例**（以 PNG 为例）:
```js
const API = 'http://192.168.4.1';
const CHUNK_SIZE = 12000; // 约 12 KB per chunk（base64 字符数）

async function uploadPngChunked(base64String) {
  // 1. 初始化
  await fetch(`${API}/api/update_display_start`, {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({ type: 'png' })
  });

  // 2. 分块发送
  for (let i = 0; i < base64String.length; i += CHUNK_SIZE) {
    const chunk = base64String.slice(i, i + CHUNK_SIZE);
    await fetch(`${API}/api/update_display_chunk`, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ type: 'png', data: chunk })
    });
  }

  // 3. 提交
  const res = await fetch(`${API}/api/update_display_commit`, {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({ type: 'png' })
  });
  const json = await res.json();
  console.log('提交结果', json);
}
```

---

## 前端实现最佳实践

- **文件列表**：`/list` 返回数组或分页对象，使用 `Array.isArray(data)` 区分；分页时取 `data.files`。
- **上传**：不要手动设置 `Content-Type`（浏览器自动设置 multipart boundary）；上传前客户端判断文件大小（≤ 50 MB）；使用 `XMLHttpRequest` 追踪进度。
- **删除/重命名**：操作后刷新文件列表；重命名书籍后 webapp 侧 IndexedDB 阅读时长记录不会自动迁移（以文件名为 key）。
- **低内存/存储错误**：设备返回 5xx/507 时提示用户释放空间或稍后重试。
- **分块大小**：`update_display_chunk` 每块不超过 16,384 字符；建议实际使用 ≤ 12,000 留有余量。
- **RDT + PNG 同步**：先上传 PNG，再上传 RDT（或反之），最后 commit 两者；设备读显示数据时同时需要两个文件。
