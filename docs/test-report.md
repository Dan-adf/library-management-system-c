# 测试报告

- 测试日期：2026-09-04
- 测试环境：Ubuntu / GCC / pthread / cJSON
- 执行命令：`make test`
- 结论：通过

## 自动化测试结果

```text
JSON validation passed.
PASS: protocol, permissions, borrowing, renewal, return
PASS: concurrent last-copy protection
PASS: administrator book/user/log endpoints
All integration tests passed.
```

测试运行于临时目录和独立端口 `18081`，没有修改正式运行数据。

## 覆盖范围

- JSON 数据合法性
- 未登录访问控制
- 用户与管理员登录
- 图书查询
- 借阅、重复借阅、归还、重复归还
- 续借及续借次数限制
- 普通用户越权拦截
- 最后一册图书的并发竞争
- 管理员新增、修改、下架图书
- 用户、全部借阅记录和日志查询

## 未自动化项目

- 人工修改到期时间后的逾期界面显示
- 长时间压力测试
- 网络断开与重连体验
- TLS 与密码安全测试（课程版本未实现）
