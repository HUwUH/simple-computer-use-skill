# simple-computer-use

无需安装的 Windows computer-use 工具。设计稿见上一层目录的 `设计计划.md`。

## 编译

```
mingw32-make
```

产物在 `bin/`。

## 用法

1. 在**普通权限**的终端里运行 `bin/cua_server.exe`（**不要**以管理员运行）
2. 在另一个终端里调用 `bin/simple_cua.exe <命令> [参数]`

## ⚠️ 重要

- **不要把本仓库放进 DSH 的 workspace-write 工作区。**
  DSH 会给工作区目录打 **Low 完整性标签**，标签是**可继承**的 ——
  在那里编译出来的 exe 运行时会变成 Low 完整性，**无法注入任何输入**。
  （标签还会跟着同盘移动走，只有跨盘复制才会摆脱。）

- server 和 client 都是普通用户权限的程序，**不需要管理员**。

## 目录

```
src/common/     client 与 server 共用（通信、Win32 封装）
src/actions/    server 的动作实现
bin/            编译产物
```
