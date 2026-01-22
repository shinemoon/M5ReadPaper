**ReaderPaper — HTTP API（Web 前端 开发者参考）**

- 说明: 本文档面向前端工程师，汇总设备中用于 Web 前端交互的 HTTP 接口（由 `WiFiHotspotManager` 和 `ApiRouter` 提供）。包括请求方法、必需/可选参数、响应格式、示例（curl / fetch / XHR）、以及前端实现注意事项（CORS、预检、上传进度、分页）。
- 设备默认热点地址: `http://192.168.4.1`（在 AP 模式下，`WiFiHotspotManager::getIPAddress()` 返回热点地址）

**通用注意**
- CORS: 服务端会在 JSON API 上添加以下响应头：
  - `Access-Control-Allow-Origin: *`
  ```markdown
  **ReaderPaper — HTTP API（Web 前端 开发者参考）**

  - 说明: 本文档面向前端工程师，汇总设备中用于 Web 前端交互的 HTTP 接口（由 `WiFiHotspotManager` 和 `ApiRouter` 提供）。包含请求方法、必需/可选参数、响应格式、示例（curl / fetch / XHR）以及前端实现注意事项（CORS、预检、上传进度、分页）。
  - 设备默认热点地址: `http://192.168.4.1`（在 AP 模式下，`WiFiHotspotManager::getIPAddress()` 返回热点地址）

  **通用注意**
  - CORS: 服务端会在 JSON API 上添加以下响应头：
    - `Access-Control-Allow-Origin: *`
    - `Access-Control-Allow-Methods: GET, POST, OPTIONS, DELETE`
    - `Access-Control-Allow-Headers: Content-Type, X-Requested-With`
  - OPTIONS 预检: 当浏览器发起跨域复杂请求（例如带自定义头或 Content-Type 为 JSON）时，浏览器会先发送 `OPTIONS`，服务器已实现对应的 `HTTP_OPTIONS` 返回（通常返回 204）。前端无需特殊处理，浏览器自动发起预检。
  - 常见辅助路由: `/favicon.ico` 返回 204（避免 404）
  - 大文件上传: 服务器以流式方式写入到 SD 卡（使用临时 `.tmp` 文件），完成后会尝试重命名覆盖目标文件。上传期间与写入相关的内存与存储检查可能导致上传被拒绝并返回 4xx/5xx 错误（详见上传章节）。

  ---

  **1) 获取文件列表 — `/list`**
  - 方法: GET
  - 路径示例:
    - 列出所有: `GET /list`
    - 列出书籍: `GET /list/book`
    - 列出字体: `GET /list/font`
    - 列出图片: `GET /list/image`
  - Query 参数 (可选):
    - `page` (整数) — 页码（从1开始），与 `perPage` 一起作用；代码以 `page>0 && perPage>0` 判断是否启用分页
    - `perPage` (整数) — 每页条数
  - 返回:
    - 无分页: 返回 JSON 数组，元素形如:
      {
        "name":"显示名称（最长约60字符，过长会被截断并添加 ...）",
        "type":"file|dir",
        "size":12345,
        "isCurrent":0|1,       // 是否为当前打开的书/正在使用的字体
        "isIdxed":0|1,         // 针对书籍：是否存在对应的 .idx 索引
        "path":"/sd/book/xxx.txt" // 可用于下载/删除的规范路径（注意需要以此 path 参数调用 /download 或 /delete）
      }
    - 分页: 返回对象 {"total":N,"page":P,"perPage":M,"files":[...]}（`total` 为总条目数）
  - HTTP 示例 (curl):
    ```bash
    curl "http://192.168.4.1/list/book"
    curl "http://192.168.4.1/list/book?page=1&perPage=20"
    ```
  - 前端 Fetch 示例:
    ```js
    const res = await fetch('http://192.168.4.1/list/book');
    const data = await res.json();
    // data 可能是数组或分页对象，检查是否有 data.files
    ```
  - 注意:
    - 对于 `/list/book`，服务端会过滤掉非 `.txt` 的文件并只返回书籍条目（兼容性策略）。
    - 名称会被 JSON 转义并做长度限制以减少前端内存峰值。

  ---

  **2) 上传文件 — `/upload`**
  - 方法: GET (返回上传页面 HTML), POST (multipart/form-data 上传)
  - 说明: 上传使用 multipart/form-data 表单提交。前端可使用 `FormData` 或 `XMLHttpRequest` 发送文件。
  - 表单字段:
    - `tab` (可选)：目标目录，支持值 `book`, `font`, `image`。服务端将把文件保存到 `/book/`, `/font/` 或 `/image/` 下；默认为 `/`。
    - 文件字段：通常使用 `file`（后端从 multipart 中读取文件并写入临时文件）。
  - 行为与实现细节（关键点）:
    - 流式写入：服务端在上传时以流式写入到 SD 卡的临时路径（目标路径 + `.tmp`），上传完成并验证后再重命名为目标文件。若目标文件已存在，会尝试删除或先重命名为备份（`.upload.bak`）再覆盖。
    - 内存检查：上传开始时会检查可用堆内存（代码示例中对启动阶段检查 >= ~32KB），写入阶段会再次检查（例如 >= ~24KB），验证阶段若内存很低会跳过验证并返回成功提示。不同阶段可能返回 507/500 等错误。
    - 存储检查：会检查 SD 剩余空间并预留一定空间（代码内检查以 MB 为单位），如空间不足会返回 507（Insufficient storage space）。
    - 超时：上传有超时时间（例如 300 秒），超时会返回 408。
    - 大文件限制：代码中存在一个上限变量（当前实现中为 50MB），如果上传大于该限制，服务器会返回 413。但注意实现中返回的错误文本可能仍写为“20MB supported”（实现有不一致），前端应以状态码为准并对错误信息做容错处理。
    - 完整性验证：上传结束后会打开临时文件验证大小（允许小幅差异，容忍度为 1% 或 1KB 二者较小者），若差异过大会删除临时文件并返回 500。
    - 覆盖/索引触发：若上传到 `/book/` 并覆盖了当前正在阅读的文件，设备会触发重建索引请求；若上传到 `/font/`，会刷新字体列表；上传到 `/image/` 会使锁屏图片缓存失效。

  - 返回值:
    - 成功: HTTP 200 + JSON {"ok":true,"message":"File uploaded successfully"}
    - 常见错误: HTTP 413 (文件过大), 507 (资源不足), 500 (写入/验证失败), 408 (超时) 等，响应体为 JSON 错误描述。

  - curl 示例:
    ```bash
    curl -F "tab=book" -F "file=@/path/to/book.txt" http://192.168.4.1/upload
    ```

  - 前端 Fetch 示例 (FormData):
    ```js
    const file = fileInput.files[0];
    const fd = new FormData();
    fd.append('tab', 'book');
    fd.append('file', file, file.name);

    const res = await fetch('http://192.168.4.1/upload', {
      method: 'POST',
      body: fd,
      mode: 'cors'
    });
    const json = await res.json();
    console.log(json);
    ```

  - Upload 进度（XHR）示例:
    ```js
    const xhr = new XMLHttpRequest();
    xhr.open('POST', 'http://192.168.4.1/upload');
    xhr.upload.onprogress = function(e) {
      if (e.lengthComputable) {
        const pct = (e.loaded / e.total) * 100;
        console.log('上传进度', pct);
      }
    };
    xhr.onload = function() { console.log('完成', xhr.responseText); };
    const fd = new FormData(); fd.append('tab','book'); fd.append('file', file);
    xhr.send(fd);
    ```

  - 实用提示:
    - 不要手动设置 `Content-Type`（浏览器会为 multipart 设置边界）；若手动设置，会导致上传失败或预检出错。
    - 客户端在上传前应做文件大小检查并给出友好提示；若文件确实很大，建议在界面上告知用户预计耗时并在断线后提供重试。

  ---

  **3) 下载文件 — `/download`**
  - 方法: GET
  - 参数: `path` (必需) — 例如 `/book/somebook.txt` 或 `/font/xxx`
  - 返回: 文件流，带适当 `Content-Type` 与 `Content-Disposition: attachment; filename="..."`，浏览器会提示保存。
  - curl 示例:
    ```bash
    curl "http://192.168.4.1/download?path=/book/book.txt" -o book.txt
    ```
  - 前端 Fetch 示例:
    ```js
    const res = await fetch('http://192.168.4.1/download?path=/book/book.txt');
    if (res.ok) {
      const blob = await res.blob();
      const url = URL.createObjectURL(blob);
      // 创建 <a download> 链接或直接打开
    }
    ```

  ---

  **4) 删除文件 — `/delete`**
  - 方法: GET
  - 参数: `path` (必需)
  - 返回: JSON {"ok":true,"message":"File deleted successfully"} 或错误信息
  - 保护机制及副作用:
    - 若尝试删除当前正在阅读的书，服务器会返回 400 并拒绝删除（以避免运行时引用失效）。
    - 删除书籍时，设备会同时清理相关的辅助文件（如 `.idx`、书签 `.bm`、进度/索引文件、tags 等），并刷新书籍缓存；若删除的文件正被当前打开的书使用，设备会尝试回退到默认文件（例如 `/spiffs/ReadPaper.txt`）或清空当前引用。

  - curl 示例:
    ```bash
    curl "http://192.168.4.1/delete?path=/book/book.txt"
    ```

  ---

  **5) 时间同步 — `/sync_time`**
  - 方法: POST
  - Body: 服务器从请求 body 的文本中解析 `timestamp`（示例 JSON 字段名或简单文本形式皆可，解析基于字符串检索而非严格 JSON 解析），可选解析 `tzOffsetMinutes` 字段来设置时区偏移。
  - 功能: 调用 `settimeofday` 同步设备时间，并返回本地时间的可读字符串。
  - curl 示例:
    ```bash
    curl -X POST -d '{"timestamp": 1630000000}' http://192.168.4.1/sync_time
    ```

  ---

  **6) 健康检查 / 版本信息 — `/heartbeat`**
  - 方法: GET
  - 返回: JSON {"status":"ok","hw":"...","firmware":"...","version":"..."}
  - 实现细节: 服务端会尝试从 SPIFFS 的 `/version` 文件读取前三行（hw、firmware、version），若文件不存在则使用内置默认值。
  - 使用场景: Web 前端在页面加载时可 ping 此接口以判断设备是否已就绪并显示版本信息。
  - curl 示例:
    ```bash
    curl "http://192.168.4.1/heartbeat"
    ```

  ---

  **7) 阅读记录 — `/api/reading_records`**
  - 方法: GET
  - 用途: 导出/查询设备上的阅读时长记录（由设备在 `/bookmarks` 下生成的 `.rec` / `.bm` 等文件）。
  - 支持的查询参数:
    - `book`：单本书路径（示例 `/book/example.txt`）
    - `books`：逗号分隔的多本书路径（示例 `/book/a.txt,/book/b.txt`）
    - 若无参数，则返回设备上所有可发现的 `.rec` 记录（会扫描 `/bookmarks` 目录）
  - 返回: JSON 对象，示例结构：
    {
      "total": N,
      "records": [ { /* 每本书的统计 JSON，包含 book_path, book_name, total_hours, total_minutes, hourly_records, daily_summary, monthly_summary */ }, ... ],
      "processed": M
    }
  - 细节: 服务端会尝试匹配 `/sd` 和 `/spiffs` 前缀以找到对应的 `.rec` 文件；单本/多本查询会解析并返回每本书的小时/天/月聚合统计（若文件缺失会在对应条目中包含 error 字段）。

  ---

  **错误与状态码（常见）**
  - 200: 成功（对于部分操作返回 JSON 或文件流）
  - 204: 无内容（用于 OPTIONS 预检 或 favicon）
  - 400: 客户端错误（如缺失参数、非法 path、尝试删除当前阅读书等）
  - 404: 资源未找到（例如下载路径无效）
  - 408: 上传超时
  - 413: 上传文件过大（服务器可能返回此状态）
  - 500/507: 服务器错误或资源不足（内存/存储不足） — 前端应显示用户友好错误并允许重试或稍后重试

  **前端最佳实践**
  - 在发起上传前，检测文件大小并在客户端给出用户提示，避免尝试上传明显过大的文件。
  - 使用 `XMLHttpRequest` 追踪上传进度（`xhr.upload.onprogress`），为用户显示上传条。
  - 处理 CORS/OPTIONS：浏览器会自动在需要时发送预检，不要为 `FormData` 手动设置 `Content-Type`。
  - 对返回的 JSON 做容错处理：`/list` 可能返回数组或分页对象，检查 `Array.isArray()` 或 `data.files`。
  - 对低内存或 SD 不足错误做好友好提示（服务器会返回 5xx/507），并建议用户释放空间或重试。

  ```
