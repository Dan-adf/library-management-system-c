# Linux C 图书管理系统

这是一个使用 TCP、pthread 和 cJSON 实现的客户端/服务器图书管理系统。

## 依赖

```bash
sudo apt update
sudo apt install build-essential make libcjson-dev netcat-openbsd
```

## 编译和运行

```bash
cd ~/library-system
make clean
make
./build/server
```

在另一个终端运行：

```bash
cd ~/library-system
./build/client
```

管理员：admin / admin123
普通用户：reader / 123456
禁用用户：disabled / 123456

常用工程命令：

```bash
make                # 编译服务器和客户端
make debug          # 生成带调试信息的版本
make check-json     # 校验运行数据 JSON
make test           # 运行隔离的自动化集成测试
make run-server     # 启动服务器
make run-client     # 启动客户端
make clean          # 删除构建产物
```

服务器和客户端使用 TCP 端口 `18080`。协议规定每行是一条完整 JSON 消息。
可通过 `LIBRARY_PORT` 修改端口，通过客户端的 `LIBRARY_HOST` 修改服务器地址。

设计、协议和测试说明分别位于：

- `docs/design.md`
- `docs/protocol.md`
- `docs/test-plan.md`
- `docs/test-report.md`
- `docs/submission.md`

自动化测试使用 `tests/fixtures/` 中的独立测试数据，不会污染 `data/`。

## 功能

- 多客户端并发连接
- 在线登录、退出和连接级会话
- `ADMIN` 与 `READER` 角色权限控制
- 图书列表与多字段搜索
- 借书、还书、查看本人记录和续借
- 逾期状态判定和逾期借阅限制
- 管理员新增、修改、上架和下架图书
- 管理员新增、查看、启用和禁用读者
- 管理员查看全部借阅记录和操作日志
- 读者验证原密码后修改自己的用户名和密码
- JSON 临时文件替换写入及启动时库存一致性修复

## 数据文件

- `data/users.json`：用户与角色
- `data/books.json`：图书和库存
- `data/borrow_records.json`：借阅历史
- `data/operation_logs.json`：关键操作审计日志

请始终从项目根目录启动服务器，否则相对路径下的数据文件无法读取。

## 主要业务规则

- 每名用户最多同时借 5 本书。
- 同一用户不能重复借阅同一本尚未归还的书。
- 每条借阅记录最多续借 1 次，每次延长 30 天。
- 已逾期或已归还的记录不能续借。
- 有逾期图书时必须先归还，才能继续借书。
- 下架图书不可新借，但原借阅记录仍可正常归还。
- 图书总册数不能小于当前借出册数。
- 只有管理员可以调用管理接口；客户端隐藏菜单不是权限边界，服务器会再次验证角色。
- 读者只能修改自己的登录凭据，不能修改其他用户或把自己提升为管理员。

## 测试建议

1. 同时打开两个客户端，验证并发登录。
2. 将一本书设置为只剩 1 册，用两个账号同时借阅，确认只有一个成功。
3. 验证普通读者无法调用管理员接口。
4. 验证重复借阅、重复归还、重复续借均被拒绝。
5. 验证下架图书不能借阅，重新上架后可借。
6. 重启服务器并确认 JSON 数据仍然存在且库存一致。

## 当前安全说明

本项目用于课程演示，当前密码字段仍以明文保存在本地 JSON 中，且 TCP 通信未加密。生产系统应使用 Argon2/bcrypt 等密码哈希、TLS 和数据库事务。
