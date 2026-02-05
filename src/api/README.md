# ReaderPaper 设备 Web API 文档 (v1)

本接口在设备开启 Wi‑Fi 热点并启动内置 HTTP 服务时可用，旨在替代返回整页 HTML 的方式，提供面向客户端（浏览器、桌面工具、脚本）的 JSON/文件流 API。该文档覆盖目前前端 `data/template.html` 所有用到的能力。

- 设备热点默认信息：SSID `ReaderPaper-AP`，密码 `readpaper123`（可在固件中修改）
- 设备默认地址：`http://192.168.4.1`（AP 模式下）
- 编码约定：UTF-8
- CORS：大多数端点已设置 `Access-Control-Allow-Origin: *`；`/upload` 与 `/sync_time` 提供了预检（OPTIONS）支持，其它端点浏览器同源访问不受限。若需跨域 DELETE 等方法，可扩展 OPTIONS 处理。
- 文件分类固定四类目录：`/book`（书籍）、`/font`（字体）、`/image`（屏保）、`/screenshot`（屏幕截图）
- 重要限制：单文件最大 50MB（服务器强制）。磁盘空间需要预留约 10MB 富余。

---

## 统一返回与错误

- 成功：
  - JSON 场景返回 HTTP 200 + `{"ok":true,...}` 或 JSON 数组
  - 文件下载返回 200 + 正确的 `Content-Type` 和 `Content-Disposition`
- 常见错误码：
  - 400 Bad Request（缺少参数/参数非法）
  - 404 Not Found（文件不存在）
  - 408 Request Timeout（上传超时）
  - 413 Payload Too Large（文件超过 20MB）
  - 500 Internal Server Error（写入失败/未知错误）
  - 507 Insufficient Storage（空间不足）
- 标头：JSON 端点一般附带 `Access-Control-Allow-Origin: *`

---

## 目录与文件

### GET /list
返回根目录（通常不直接使用）。

- 响应：`application/json` 数组，元素结构同下。

### GET /list/book | /list/font | /list/image | /list/screenshot
返回对应目录下的条目列表。支持分页参数。

- Query 参数（可选）：
  - `page` （整数）：页码（从 1 开始）
  - `perPage` （整数）：每页条数
  - 当 `page > 0 && perPage > 0` 时启用分页，否则返回所有条目（受配置文件上限限制）
- 响应内容：
  - 无分页：数组，元素结构：
    ```json
    {
      "name": "example.txt",
      "type": "file",      // "file" | "dir"
      "size": 12345,        // 字节，仅文件有
      "isCurrent": 0,       // 书籍：当前正在阅读；字体：当前选中；其余：0 或 1
      "isIdxed": 0,         // 书籍专用：是否存在 .idx 索引文件
      "path": "/book/example.txt"  // 完整路径，用于下载/删除
    }
    ```
  - 分页模式：
    ```json
    {
      "total": 100,
      "page": 1,
      "perPage": 20,
      "files": [ /* 同上面的数组元素 */ ]
    }
    ```
- 说明：
  - 服务器可能对总返回数量有上限（构建时常量），前端如需更多可分页或后续扩展服务端分页参数（未实现）。

### GET /download?path=/book/xxx.txt
下载指定文件。

- 请求参数：
  - `path`：完整虚拟路径，形如 `/book/<name>`、`/font/<name>`、`/image/<name>`
- 响应：
  - 200 成功，携带 `Content-Type` 与 `Content-Disposition: attachment; filename="…"`
  - 404 文件不存在

### GET /delete?path=/book/xxx.txt
删除指定文件（注意：当前实现用 GET 触发删除）。

- 请求参数：
  - `path`：完整虚拟路径（同上）
- 约束与行为：
  - 正在阅读的书籍不允许删除（将返回 400）。
  - 删除字体会刷新字体缓存；删除书籍会刷新书籍缓存，并在删除当前书籍时尝试回退至默认书。
- 响应：
  - 成功：`{"ok":true, "message":"File deleted successfully"}`
  - 失败：`{"ok":false, "message":"..."}`（含原因）

---

## 上传

### GET /upload
返回一个简单上传页（主要用于本地调试）。多数客户端可直接调用 POST 接口。

### POST /upload?tab=book|font|image|scback
Multipart 文件上传。使用标准 multipart/form-data 格式。

- Query：
  - `tab`：上传的目标目录类别（必填）：
    - `book` → `/book/` 目录
    - `font` → `/font/` 目录
    - `image` → `/image/` 目录
    - `scback` → SD根目录的 `/scback.png`（强制文件名，用于锁屏背景）
- Body（multipart/form-data）：
  - 标准的 multipart 文件上传字段（服务器从请求中解析文件内容）
- 服务器约束：
  - 单个文件最大 50MB；超过返回 413。
  - 需要足够存储空间（预留约 10MB）；不足返回 507。
  - 上传采用临时文件写入，结束后重命名覆盖同名文件；若覆盖了当前阅读书籍会触发重建索引。
  - 上传超时 300 秒（5分钟），超时返回 408。
- 响应示例：
  - 成功：`{"ok":true,"message":"File uploaded successfully"}`
  - 失败：`{"ok":false,"message":"Write failed"}` 等
- CORS：支持 `OPTIONS` 预检（返回 204），允许头：`Content-Type, X-Requested-With`

---

## 时间同步

### POST /sync_time
将客户端时间写入设备 RTC。

- 请求头：`Content-Type: application/json`
- 请求体：
  ```json
  {
    "timestamp": 1731043200,      // 必填，Unix 秒
    "iso": "2024-11-08T12:00:00.000Z",  // 可选，仅记录
    "tzOffsetMinutes": -480       // 可选，浏览器 getTimezoneOffset() 值（注意符号）
  }
  ```
- 行为：
  - 若提供 `tzOffsetMinutes`，将依 POSIX 规则设置环境变量 `TZ`（例如中国标准时间 -> `CST-8`）。
  - 返回纯文本，如：`Time synced: 1731043200 (2024-11-08 20:00:00 LOCAL)`。
- CORS：支持 `OPTIONS` 预检。

---

## 路径与命名规则

- 客户端总是使用虚拟前缀路径：`/book/...`、`/font/...`、`/image/...`。
- 服务端内部可能将这些映射到 SD 或 SPIFFS 实际路径，不影响客户端。
- 文件名大小写与特殊字符：
  - JSON 输出对 `"` 和 `\` 做了转义；客户端在拼接 URL 时应使用 `encodeURIComponent`。

---

## 示例

下方示例以默认设备地址 `192.168.4.1` 为例。

### curl（可在多数平台使用）

- 列出书籍
  ```bash
  curl http://192.168.4.1/list/book
  ```
- 下载文件
  ```bash
  curl -OJ "http://192.168.4.1/download?path=/book/example.txt"
  ```
- 删除文件
  ```bash
  curl "http://192.168.4.1/delete?path=/book/example.txt"
  ```
- 上传文件到书籍目录
  ```bash
  curl -F "file=@example.txt" "http://192.168.4.1/upload?tab=book"
  ```
- 上传锁屏背景
  ```bash
  curl -F "file=@background.png" "http://192.168.4.1/upload?tab=scback"
  ```
- 同步时间
  ```bash
  curl -H "Content-Type: application/json" -d '{"timestamp":'$(date +%s)'}' http://192.168.4.1/sync_time
  ```

### Windows PowerShell（v5.1）

- 列出书籍
  ```powershell
  Invoke-RestMethod -Uri "http://192.168.4.1/list/book" -Method Get
  ```
- 下载文件
  ```powershell
  Invoke-WebRequest -Uri "http://192.168.4.1/download?path=/book/example.txt" -OutFile "example.txt"
  ```
- 删除文件
  ```powershell
  Invoke-RestMethod -Uri "http://192.168.4.1/delete?path=/book/example.txt" -Method Get
  ```
- 上传文件到书籍目录
  ```powershell
  $file = Get-Item ".\example.txt"
  Invoke-WebRequest -Uri "http://192.168.4.1/upload?tab=book" -Method Post -Form @{ file = $file }
  ```
- 同步时间
  ```powershell
  $ts = [int][double]::Parse((Get-Date -AsUTC -UFormat %s))
  $body = @{ timestamp = $ts; tzOffsetMinutes = -[int]([timezone]::CurrentTimeZone.GetUtcOffset((Get-Date)).TotalMinutes) } | ConvertTo-Json
  Invoke-RestMethod -Uri "http://192.168.4.1/sync_time" -Method Post -ContentType "application/json" -Body $body
  ```

### 浏览器 JS fetch

```js
// 示例：拉取列表并删除一个文件
async function demo() {
  const files = await fetch('http://192.168.4.1/list/book').then(r=>r.json());
  console.log('book files', files);
  if (files.length) {
    const name = files[0].name;
    const path = `/book/${encodeURIComponent(name)}`; // 注意：此处仅演示，真实需全名编码
    const res = await fetch(`http://192.168.4.1/delete?path=${path}`);
    console.log(await res.json());
  }
}
```

---

## 兼容性与后续扩展

- 兼容性：端点路径与 `data/template.html` 中的调用保持一致，可直接替换为纯客户端调用。
- 可能的增强：
  - 为所有端点补齐 `OPTIONS` 预检（DELETE/下载跨域场景）。
  - 服务端分页与排序：`/list/book?page=1&pageSize=20&order=name`。
  - 配置/状态接口：`/config/get`、`/config/set`、`/status`、`/book/current` 等（当前未开放）。
  - 简易鉴权：启动时生成临时 token，通过 `X-Auth-Token` 校验。
  - 批量操作：批量删除、批量移动等。

---

## 变更记录

- v1（2025‑11‑08）：整理初版 API 文档，覆盖列表 / 上传 / 删除 / 下载 / 时间同步。
