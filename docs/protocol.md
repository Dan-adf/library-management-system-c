# JSON 通信协议

## 1. 消息边界

传输层使用 TCP。每条请求和响应都是一行 UTF-8 JSON，以 `\n` 结束。不能假设一次 `send()` 对应一次 `recv()`。

## 2. 通用请求

```json
{
  "action": "LIST_BOOKS",
  "request_id": "REQ-1",
  "data": {}
}
```

## 3. 通用响应

```json
{
  "success": true,
  "request_id": "REQ-1",
  "message": "Book list retrieved",
  "data": {}
}
```

`success=false` 表示业务失败；客户端应显示 `message`，不能仅依靠 TCP 连接状态判断操作是否成功。

## 4. 操作清单

| action | 登录要求 | 角色 | data 主要字段 |
|---|---|---|---|
| `PING` | 否 | 任意 | `message` |
| `LOGIN` | 否 | 任意 | `username`, `password` |
| `LOGOUT` | 是 | 任意 | 无 |
| `WHO_AM_I` | 是 | 任意 | 无 |
| `LIST_BOOKS` | 是 | 任意 | 无 |
| `SEARCH_BOOKS` | 是 | 任意 | `keyword` |
| `BORROW_BOOK` | 是 | 任意 | `book_id` |
| `MY_BORROWS` | 是 | 任意 | 无 |
| `RETURN_BOOK` | 是 | 任意 | `record_id` |
| `RENEW_BOOK` | 是 | 任意 | `record_id` |
| `ADD_BOOK` | 是 | ADMIN | 图书字段 |
| `UPDATE_BOOK` | 是 | ADMIN | `book_id` 及待修改字段 |
| `SET_BOOK_STATUS` | 是 | ADMIN | `book_id`, `status` |
| `ADD_USER` | 是 | ADMIN | `username`, `password`, `name` |
| `LIST_USERS` | 是 | ADMIN | 无 |
| `SET_USER_STATUS` | 是 | ADMIN | `user_id`, `enabled` |
| `ALL_BORROWS` | 是 | ADMIN | 无 |
| `LIST_LOGS` | 是 | ADMIN | 无 |

## 5. 环境配置

- `LIBRARY_PORT`：服务器与客户端端口，默认 `18080`。
- `LIBRARY_HOST`：客户端连接地址，默认 `127.0.0.1`。

例如：

```bash
LIBRARY_PORT=19090 ./build/server
LIBRARY_HOST=192.168.1.20 LIBRARY_PORT=19090 ./build/client
```
