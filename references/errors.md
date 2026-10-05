# 出错怎么办

（`<cua>` 代表 client 的完整路径，通常是 `<本 skill 目录>\bin\simple_cua.exe`。）

## 一、先看退出码

client 的**退出码**就是最可靠的判断依据：

| 码 | 含义 | 大类 |
|---|---|---|
| `0` | 成功 | |
| `2` | **参数写错了** | 你的问题，改参数重试 |
| `3` | **连不上 server** | 环境问题，要请用户介入 |
| `4` | **server 侧执行失败** | 环境问题，看 `code` 判断 |
| `5` | **安全拒绝** | ⚠️ **不要盲目重试** |

失败时 stdout 上是一行 JSON：

```json
{"ok":false,"op":"click","code":"not-on-monitor","msg":"(5000,5000) is outside monitor 0 (size 1920x1080); coordinates are monitor-local, see get_state"}
```

---

## 二、按退出码细看

### 退出码 2 —— 参数写错了

改参数就好，不需要用户介入。

| `code` | 意思 |
|---|---|
| `no-action` | 没给动作名 |
| `unknown-action` | 动作名不认识 |
| `unknown-option` / `bad-arg` | 有不认识的参数，或参数缺值 |
| `conflicting-args` | 两个互斥的参数都给了（如 `X Y` 和 `--dx/--dy`） |
| `need-target` | 坐标只给了一个 |
| `need-region` | `zoom` 的 `X Y W H` 没给全 |
| `need-points` | `drag` 的四个坐标没给全 |
| `need-text` | `type` / `clipboard set` 没给内容 |
| `need-chord` | `key` 没给键名 |
| `need-subcommand` / `bad-subcommand` | `clipboard` 的子命令不对 |
| `unknown-key` | 键名不认识 —— 用 `key --show` 查 |
| `bad-button` | `--button` 取值不对 |
| `bad-chord` | 组合键格式不对（空的段，或超过 4 个键） |
| `bad-monitor` | 显示器编号超出范围 |
| `bad-region` / `bad-scale` | `zoom` 的宽高或倍数不合法 |
| `bad-hold` / `bad-interval` | 时长参数超范围 |
| `too-long` | 文本超过上限（`type` 是 1024 字节；`clipboard set` 也有上限） |
| `too-much` | `scroll` 的格数超过 ±100 |
| `nothing-to-do` | `scroll` 的 `--dy` 和 `--dx` 都是 0 |
| `bad-b64` / `bad-utf8` | base64 或 UTF-8 解不开 |
| `no-out` / `bad-out` / `out-not-absolute` | `--out` 没给 / 解析不了 / 不是绝对路径 |
| `request-too-long` | 整条命令行太长了 |

### 退出码 3 —— 连不上 server

`code` 只有一种：`server-not-running`。

**处理**：不要自己想办法。**把 client 返回的那句话原样转告用户**：

> 请启动 `bin\cua_server.exe`（普通权限即可）。

然后让用户确认它起来了（可以用 `cua_server.exe --help` 看参数）。

另一种可能是 server 在跑、但连不上（管道权限问题）。这时 `cua_server.exe` 的启动日志里会有一行 `pipe-security:`，正常应该是：

```
dacl=current-user+logon-session  low-label=set(Low)
```

出现 `EVERYONE(...)` 或 `SET-FAILED` 就说明有问题。将这个检查的需求也转告用户。

### 退出码 4 —— server 侧执行失败

工具跑到了、但做事失败。多半是环境问题。

| `code` | 意思 | 怎么办 |
|---|---|---|
| `capture-failed` | 抓屏失败 | 会话可能被锁 / 无头环境 |
| `save-failed` | 图片编码或写文件失败 | 检查路径和磁盘 |
| `no-text` | 剪贴板里不是文本 | 换 `--text` 内容，或让用户先复制一段文字 |
| `set-failed` / `clear-failed` | 剪贴板被别的程序占着 | 稍后重试 |
| `write-failed` | 写剪贴板全文到文件失败 | 检查路径 |
| `oom` | 内存不够 | |
| `send-failed` / `no-response` / `no-body` | 管道通信断了 | server 可能已退出 → 按退出码 3 处理 |

### 退出码 5 —— 安全拒绝

这一类不要盲目重试。它们是"闸门拦下来了"，重试通常还是同样的结果。

| `code` | 意思 | 怎么办 |
|---|---|---|
| `not-on-monitor` / `start-not-on-monitor` / `end-not-on-monitor` | 坐标不在那台显示器上 | 用 `get_state` 看尺寸，把坐标改到范围内 |
| `region-not-on-monitor` | `zoom` 的区域跨出了显示器 | 缩小区域，或确认 `--monitor` |
| `landed-elsewhere` / `move-failed` | 光标没落到要求的位置（多半被屏幕边缘钳住） | 换个目标点 |
| `blocked-key` | 撞上 `key` 的黑名单 | **不要绕** —— 换个做法，或问用户 |
| `cancelled-by-user-motion` | **人在预备窗口里把鼠标移开了** | ✅ **这是正常结果**，说明用户否决了这次操作。**不要重试**，先弄清楚用户为什么否决，或者换一种方式征求同意 |
| `out-bad-extension` / `out-not-png` / `out-not-txt` | `--out` 的扩展名不对 | 按报错里的提示改（截图是 `.png`，剪贴板是 `.txt`） |
| `out-not-writable` | 这个 client **以自己的权限**写不了那个位置 | 换个位置（比如 client 的 cwd），或让用户来处理 |

---

## 三、三个最常见的场景

### 场景 1：`server-not-running`（退出码 3）

```
<cua> ping
RESULT {"ok":false,"code":"server-not-running","msg":"the cua server is not running. ..."}
```

**做**：告诉用户去启动 `cua_server.exe`，等他们确认之后再 `ping` 一次。
**不要**：自己去找 server、自己启动它、或者换别的路径试。

### 场景 2：`cancelled-by-user-motion`（退出码 5）

```
<cua> click 900 500
RESULT {"ok":false,"code":"cancelled-by-user-motion","msg":"the cursor moved more than 50 px during the 1 s arm window, ..."}
```

**含义**：那 1 秒里有人把鼠标移开了 —— **这就是设计要给人的否决权**。

**做**：把它当成"用户不同意这次点击"。
**不要**：重试、或者换个坐标继续点。

### 场景 3：动作"成功"了但界面没变化

`SendInput` 这类 API **失败时也可能返回成功**，所以"返回值说成功了"不等于"真的生效了"。

**做**：**再截一张图**，用眼睛确认。
**不要**：只看 `{"ok":true}` 就认为事情办成了。

---

## 四、实在不行

**多次尝试仍然做不成某个动作时，不要一直试** —— 把现象和已经试过的命令告诉用户，让用户决定。
