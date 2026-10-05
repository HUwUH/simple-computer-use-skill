# 完整使用序列

下面都是**跑通过的真实序列**。约定：

- `<cua>` = client 的路径
- 每条命令都是一次独立的进程调用；**一条 PowerShell 命令里可以串多步**
- 输出里的路径都是示例值

一个方便的流程：看 → 动 → 再看。每次动完手，可以截一张图确认。

---

## 例 1 · 最小闭环：确认环境 + 看一眼屏幕

```powershell
<cua> ping
<cua> get_state
<cua> screenshot --monitor 0
```

```json
{"ok":true,"op":"ping","version":"0.1.0","pid":12345,"integrity":"S-1-16-8192 (Medium)","elevated":false,"uptime_s":42}
{"ok":true,"op":"get_state","cursor":{"local":[900,500],"monitor":0},"cursor_flags":"SHOWING","monitor_count":1,...}
{"ok":true,"op":"screenshot","monitor":0,"size":[1920,1080],"bytes":220460,"path":"D:\\work\\scua-screenshot.png"}
```

其中：
- `ping` 里的 `integrity` 必须是 `Medium`（详见 `actions.md`）
- `get_state` 告诉你显示器尺寸和光标在哪 —— 后面的坐标都要落在 `size` 范围内
- 去读 `path` 指向的那个文件

---

## 例 2 · 点一个按钮（含验证）

假设截图上看到一个"确定"按钮，大约在 (960, 620)。

```powershell
<cua> click --monitor 0 960 620
<cua> screenshot --monitor 0
```

```json
{"ok":true,"op":"click","monitor":0,"at":[960,620],"button":"left","moved_px":0,
 "foreground":{"class":"#32770","title":"确认"}}
```

其中：
- 结束，最好要再截一张图，确认按钮真的按下去了 —— `{"ok":true}` 只说明事件发出去了
- 返回里的 `foreground` 是点击**之后**的前台窗口，可以用它判断有没有弹出新窗口
- 如果返回 `cancelled-by-user-motion`（退出码 5），说明用户否决了 —— 不用重试

---

## 例 3 · 往输入框里填字并提交

假设光标已经停在输入框上（或者先用 `click` 点进去）。

```powershell
<cua> click --monitor 0 700 400
<cua> type --text "hello@example.com"
<cua> key tab
<cua> type --text "hunter2"
<cua> key enter
<cua> screenshot --monitor 0
```

其中：
- **`type` 发给焦点窗口** —— 所以必须先 `click` 到目标
- `key enter` 才会提交；`type` 里的换行也是回车，但显式写 `key enter` 更清楚
- 含引号、换行、中文的文本，改用 `--b64` 更稳（见例 5）

---

## 例 4 · 看不清就先放大

```powershell
<cua> screenshot --monitor 0
<cua> zoom 800 400 320 240 --scale 4
```

```json
{"ok":true,"op":"zoom","monitor":0,"region":[800,400,320,240],"scale":4,"size":[1280,960],"bytes":96011,"path":"D:\\work\\scua-zoom.png"}
```

其中：
- 放大是**最近邻**，像素是硬的，适合看清小字或小图标
- 区域必须完整落在显示器内

---

## 例 5 · 把一段多行文本贴进文本框（避开回车）

目标是往一个多行文本框里放两行字，而且**不要触发任何"提交"**。

```powershell
# "line1<换行>line2" 的 base64
<cua> clipboard set --b64 bGluZTEKbGluZTI=
<cua> click --monitor 0 700 400
<cua> key ctrl+a
<cua> key ctrl+v
<cua> screenshot --monitor 0
```

其中：
- **粘贴是"插入文本"，不是按键** —— 所以换行不会被当成提交 ✓
- 这正是"要纯文本换行"时该走的路（`type` 里的换行等于回车）
- `ctrl+a` 是先全选，让粘贴**替换**原有内容；不需要替换就跳过它
- base64 可以用 PowerShell 自己算，避免手出错：

  ```powershell
  [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("line1`nline2"))
  ```

---

## 例 6 · 拖拽

把某个东西从 (400, 300) 拖到 (900, 600)。

```powershell
<cua> drag --monitor 0 400 300 900 600
<cua> screenshot --monitor 0
```

```json
{"ok":true,"op":"drag","monitor":0,"from":[400,300],"to":[900,600],
 "steps":25,"button":"left","moved_px":0,"ms":430}
```

其中：
- 起点终点要在**同一台显示器**内
- 同样有 1 秒预备圈；用户挪动鼠标就是否决
- 如果程序不认这次拖拽（当成了点击），可以加大步数：`--steps 60`

---

## 例 7 · 滚一段长内容

```powershell
<cua> move --monitor 0 900 500
<cua> scroll --dy 5
<cua> screenshot --monitor 0
```

其中：
- 滚轮发给**光标底下**的窗口，所以先 `move` 到位
- `--dy` 正数 = 向下滚动

---

## 例 8 · 从屏幕读一段文字

屏幕上有你需要的文字时，可以用"选中 → 复制 → 读剪贴板"。

```powershell
<cua> drag --monitor 0 300 200 900 260
<cua> key ctrl+c
<cua> clipboard get
```

```json
{"ok":true,"op":"clipboard","action":"get","chars":42,"bytes":42,"truncated":false,"text":"这里是复制到的内容"}
```

其中：
- 内容超过 1KB 时会被截断（`truncated:true`）—— 加 `--out D:\work\clip.txt` 拿全文
- 剪贴板里不是文本时（图片、文件列表）会报 `no-text`
- 用 drag 拖动选中文字，对 agent 的定位能力要求较高。做不到不必勉强。

---

## 出问题时

看 **client 的退出码**，然后查 `errors.md`：

| 退出码 | 先想到 |
|---|---|
| 2 | 参数写错了，看 `msg` 改 |
| 3 | 请用户启动 server |
| 4 | 环境问题，看 `code` |
| 5 | **被拦了 —— 别盲目重试** |
