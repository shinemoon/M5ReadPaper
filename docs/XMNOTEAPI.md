## 🔌 API 文档

M5Paper 设备在主页状态下会运行一个 HTTP 服务（端口 80），纸间书摘 App 就是通过这些接口来推送数据的。如果你想自己写脚本或做二次开发，可以直接调用。

> 所有接口的 Content-Type 均为 `application/json`，仅限局域网访问。
>
> 下文中 `{IP}` 代表设备 IP 地址，比如 `192.168.1.100`。

---

### GET /api/status

查询设备当前状态。

**请求示例**

```bash
curl http://{IP}/api/status
```

**响应**

```json
{
  "ip": "192.168.1.100",
  "excerptCount": 42,
  "bookCount": 8,
  "sdFreeKB": 15200,
  "version": "1.0.0"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `ip` | string | 设备 IP 地址 |
| `excerptCount` | int | 当前已同步的书摘条数 |
| `bookCount` | int | 当前已同步的书籍数 |
| `sdFreeKB` | int | SD 卡剩余空间（KB） |
| `version` | string | 固件版本号 |

---

### POST /api/sync

一次性同步全部数据（适合书摘数量不多的情况）。调用后设备上的旧数据会被完全替换。

**请求示例**

```bash
curl -X POST http://{IP}/api/sync \
  -H "Content-Type: application/json" \
  -d '{
    "books": [
      { "id": 1, "name": "小王子", "author": "圣埃克苏佩里" }
    ],
    "excerpts": [
      {
        "id": 101,
        "bookId": 1,
        "content": "所有的大人都曾经是小孩，虽然，只有少数的人记得。",
        "idea": "写在扉页上的话",
        "chapter": "作者献辞",
        "createdTime": 1713950000000
      }
    ],
    "reviewSettings": {
      "sortRule": 1,
      "sortOrder": 0,
      "autoSwitchMinutes": 10
    }
  }'
```

**请求体字段**

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `books` | array | 是 | 书籍列表 |
| `books[].id` | int | 是 | 书籍 ID |
| `books[].name` | string | 是 | 书名（最长 127 字符） |
| `books[].author` | string | 是 | 作者（最长 63 字符） |
| `excerpts` | array | 是 | 书摘列表 |
| `excerpts[].id` | int | 是 | 书摘 ID |
| `excerpts[].bookId` | int | 是 | 所属书籍 ID |
| `excerpts[].content` | string | 是 | 书摘正文（纯文本，不含 HTML） |
| `excerpts[].idea` | string | 否 | 个人想法/批注，默认空字符串 |
| `excerpts[].chapter` | string | 否 | 章节名（最长 63 字符） |
| `excerpts[].createdTime` | long | 否 | 书摘创建时间，Unix epoch milliseconds；无有效时间时可为 `0` |
| `reviewSettings` | object | 否 | 回顾设置 |
| `reviewSettings.sortRule` | int | 否 | 排序方式：`0` 顺序，`1` 随机（默认 `1`） |
| `reviewSettings.sortOrder` | int | 否 | 排序方向：`0` 从旧到新，`1` 从新到旧（默认 `0`） |
| `reviewSettings.autoSwitchMinutes` | int | 否 | 自动切换间隔，单位分钟，范围 1-1440（默认 `10`） |

`createdTime` 是为需要读取原始书摘创建时间的接收端预留的兼容字段。当前固件会将该字段原样保存到 SD 卡数据文件中，但不会在界面展示，也不会参与排序或筛选。

**成功响应** — `200 OK`

```json
{ "status": "ok" }
```

**错误响应**

| 状态码 | 响应 | 原因 |
|--------|------|------|
| 400 | `{"error": "empty body"}` | 请求体为空 |
| 400 | `{"error": "invalid JSON: ..."}` | JSON 格式错误 |
| 500 | `{"error": "SD write failed"}` | SD 卡写入失败 |

---

### 批量同步（适合大量书摘）

当书摘数量较多时，建议用批量同步的方式，分三步完成。每批最多发送 200 条书摘。

#### 第一步：POST /api/sync/begin

发送书籍信息和回顾设置，开始一次同步会话。

```bash
curl -X POST http://{IP}/api/sync/begin \
  -H "Content-Type: application/json" \
  -d '{
    "books": [
      { "id": 1, "name": "小王子", "author": "圣埃克苏佩里" },
      { "id": 2, "name": "月亮与六便士", "author": "毛姆" }
    ],
    "reviewSettings": {
      "sortRule": 1,
      "sortOrder": 0,
      "autoSwitchMinutes": 10
    }
  }'
```

**成功响应** — `200 OK`

```json
{ "status": "ok" }
```

**错误响应**

| 状态码 | 响应 | 原因 |
|--------|------|------|
| 400 | `{"error": "empty body"}` | 请求体为空 |
| 400 | `{"error": "invalid JSON: ..."}` | JSON 格式错误 |
| 500 | `{"error": "SD write meta failed"}` | 元数据写入 SD 卡失败 |
| 500 | `{"error": "SD write failed"}` | 无法创建书摘数据文件 |

#### 第二步：POST /api/sync/batch

分批发送书摘数据，可以调用多次。

```bash
curl -X POST http://{IP}/api/sync/batch \
  -H "Content-Type: application/json" \
  -d '{
    "excerpts": [
      {
        "id": 101,
        "bookId": 1,
        "content": "所有的大人都曾经是小孩，虽然，只有少数的人记得。",
        "idea": "",
        "chapter": "作者献辞",
        "createdTime": 1713950000000
      },
      {
        "id": 201,
        "bookId": 2,
        "content": "追逐梦想就是追逐自己的厄运，在满地都是六便士的街上，他抬起头看到了月光。",
        "idea": "全书的主题",
        "chapter": "",
        "createdTime": 1713960000000
      }
    ]
  }'
```

**成功响应** — `200 OK`

```json
{
  "status": "ok",
  "totalExcerpts": 200
}
```

`totalExcerpts` 是到目前为止累计接收的书摘总数。

**错误响应**

| 状态码 | 响应 | 原因 |
|--------|------|------|
| 400 | `{"error": "no sync in progress"}` | 没有先调用 `/api/sync/begin` |
| 400 | `{"error": "empty body"}` | 请求体为空 |
| 400 | `{"error": "invalid JSON: ..."}` | JSON 格式错误 |

#### 第三步：POST /api/sync/end

告诉设备所有数据已发完，完成同步。

```bash
curl -X POST http://{IP}/api/sync/end
```

**成功响应** — `200 OK`

```json
{
  "status": "ok",
  "totalExcerpts": 500
}
```

**错误响应**

| 状态码 | 响应 | 原因 |
|--------|------|------|
| 400 | `{"error": "no sync in progress"}` | 没有活跃的同步会话 |

---

### DELETE /api/data

清除设备上的所有书摘数据。

```bash
curl -X DELETE http://{IP}/api/data
```

**成功响应** — `200 OK`

```json
{ "status": "ok" }
```

---

### 404 兜底

访问不存在的路径会返回：

```json
{ "error": "not found" }
```
