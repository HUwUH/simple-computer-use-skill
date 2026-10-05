# simple-computer-use

无需安装的 Windows computer-use 工具。设计稿见 [设计计划.md](设计计划.md)。说明见 [SKILL.md 的“工具概况”](SKILL.md#工具概况) 和 [references/notes.md](references/notes.md)。

## 编译

```
mingw32-make
```

产物在 `bin/`。

## 用法

> 环境：windows，c++编译工具（推荐MinGW）

1. 用git clone本仓库
2. 将仓库改名为 `simple-computer-use` （或将 SKILL.md 的 name 改为 `simple-computer-use-skill`）
3. 打开一个cmd，运行 `mingw32-make`
4. 将本仓库直接放到 harness 对应的 skills 目录下即可

## 架构与说明

> 本节将会在之后完善

请参阅 [SKILL.md 的“工具概况”](SKILL.md#工具概况) 和 [references/notes.md](references/notes.md)。



## 目录

```
references/     给 agent 的参考文档
src/common/     client 与 server 共用（通信、Win32 封装）
src/actions/    server 的动作实现
bin/            编译产物
feasibility-validation/ 可行性验证（独立小程序，不属于构建，目前已弃用）
```
