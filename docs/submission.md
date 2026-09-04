# 提交说明

## 项目概述

本项目使用 C、POSIX Socket、pthread 和 cJSON 实现网络图书管理系统。服务器负责认证、授权、借阅规则、并发控制和 JSON 持久化；客户端提供按角色变化的命令行菜单。

## 快速验收

```bash
sudo apt install build-essential make libcjson-dev python3
make test
make run-server
```

另开终端：

```bash
make run-client
```

## 演示账号

- 管理员：`admin` / `admin123`
- 普通读者：`reader` / `123456`

以上仅为测试账号。

## 评审入口

1. `README.md`：构建与使用方式。
2. `docs/design.md`：架构、数据和并发设计。
3. `docs/protocol.md`：网络 JSON 协议。
4. `docs/test-plan.md`：测试方法。
5. `docs/test-report.md`：已执行测试结果。
6. `tests/integration_test.py`：可复现集成测试。

## 设计取舍

题目要求采用 JSON/XML 管理数据，因此本实现选择 JSON 文件，而非数据库。写入采用临时文件替换，并通过进程内互斥锁保证多客户端操作一致性。该方案适合课程和笔试演示，不等同于生产数据库事务。
