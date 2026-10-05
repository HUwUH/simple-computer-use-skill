# 其他注意事项

（本文件当前只有下面一节，是给**用户**看的技术说明。）

---

## 用户可能想知道的

### panic 和 server 的用法

```
cua_server.exe [--allow-all-users] [-v | --verbose]
```

- **用普通权限运行，不要用管理员。**
- 它一启动就常驻，之后所有动作都由它执行。
- `--help` 可以看参数。
- `-v` 让控制台也打印每次动作往返的内容（**截断到 384 字节**，人看方便）；**日志文件里始终是全文**。
- `--allow-all-users` 是逃生开关，详见下面「管道权限」一节。
- 日志文件：和 `cua_server.exe` **同目录**的 `cua_server.log`；超过 1MB 会轮转成 `cua_server.log.1`。

```
cua_panic.exe
```

**这是给人用的救火程序** —— server 卡死了、或者有键卡住了，跑一下它。它做三件事，顺序有讲究：

1. **先**松开当前按着的一切（不能委托给 server：server 可能就是卡住的那个）
2. **再**杀掉 `cua_server.exe`（杀完它就没法再注入了）
3. **再**松一次（万一它在被杀之前又按下了什么）

它也必须是**普通权限的人**来跑 —— 要杀一个 Medium 完整性的进程，动手的进程也得是 Medium；agent 在沙箱里是 Low，跑这个会失败。

### 图片和文件的默认位置

| 动作 | 默认落到哪 |
|---|---|
| `screenshot` | **client 被调用时的 cwd** 下的 `scua-screenshot.png` |
| `zoom` | **client 被调用时的 cwd** 下的 `scua-zoom.png` |
| `clipboard get --out X` | 你给的路径 |
| server 日志 | `cua_server.exe` 同目录的 `cua_server.log` |

**`--out` 的规则：**

- 必须**以指定扩展名结尾**：截图和 zoom 是 `.png`，剪贴板全文是 `.txt`
  （这是防呆：否则一个手滑就能把 PNG 写进 `设计计划.md` 里，把文档毁掉）
- 相对路径相对 **client 被调用时的 cwd**
- 必须是 **client 以自己的权限写得进去**的地方

### client 和 agent 的 pwsh 同权限，以及写文件的探测

**`simple_cua.exe` 是由 agent 的 shell 启动的，所以它和那个 shell 权限相同。**
（在 DSH 里，就是低完整性 + 受限令牌。）

这意味着：**agent 能写哪里，就等于 client 能写哪里，。**

所以在真正把请求发给 server 之前，client 会用**自己的权限**先探一次"这个位置能不能写"：

- 目标**已存在** → 试着以"写"的方式打开它（**不会改动内容**）
- 目标**不存在** → 试着以"能建文件"的方式打开它所在的目录
- **全程无副作用** —— 不创建、不删除任何东西

探不过就**直接拒绝**（退出码 5），请求根本不会发出去。

这个探测是**保守的**：client 的权限比 server **更严**（server 是 Medium，client 在 DSH 里是 Low），所以它**只会误拒，不会漏报** —— 而误拒的方向正是安全的方向。

### 这个 skill 给管道设置的权限

- **管道名**：`\\.\pipe\simple_cua`（固定名，同一时刻只允许一个 client 连接）
- **DACL 只授给**：当前用户 + **本登录会话的 SID**
  → 也就是**只有这个用户、这一次登录会话里的进程**才能连上
  → 别的账户、别的登录会话（尤其是 session 0 里的服务）都连不上
- **强制完整性标签降到 Low**
  → 否则低权限的 client 根本写不进去（这是"连不上"最常见的原因）
- `--allow-all-users` 会把第一条**放宽成 Everyone**
  → 那时**任何本地账户**都能连上来驱动这台机器的鼠标键盘
  → 所以打开它时，启动日志里会打一个醒目的警告框

启动日志里有一行可以直接核对当前状态：

```
pipe-security: dacl=current-user+logon-session  low-label=set(Low)
```

正常就该长这样。出现 `EVERYONE(...)` 或 `SET-FAILED` 就说明有问题。
