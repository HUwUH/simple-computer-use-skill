# simple-computer-use · 可行性验证

不依赖任何框架的 Windows computer-use 工具集。每个工具都是
**单个静态 exe、零运行时依赖、直接调 Win32**。

> A from-scratch Windows computer-use toolkit — one static exe per tool,
> no frameworks, no runtime dependencies, plain Win32.

---


## ✅ 验证结果

| 工具 | 验证点 | 结果 |
|---|---|---|
| `screenshot.exe` | 截图尺寸 = 显示器物理分辨率 | ✅ `1920x1080` |
| `screenshot.exe` | 多屏幕下 | ✅ 正常运行。此外，默认命令，鼠标在哪个屏幕，截图截哪个屏幕 |
| `mousepos.exe` | `GetCursorPos` 返回物理像素 | ✅ 右下角读到 `1919, 1079` |
| `mousepos.exe` | 多屏幕下 | ✅ 第二块屏幕返回的坐标换算正确 |
| `move.exe`| 移动是否正常 | ✅ |
| `click.exe`| 点击是否正常| ✅ 单击打、双击、右键全都正常|
| `type.exe` | `SendInput` + `KEYEVENTF_UNICODE` 能输入任意 Unicode（中文 / emoji），不受键盘布局与输入法影响 | 成功 |
| `key.exe` | 虚拟键组合键生效（修饰键状态确实进了输入队列）；退出前一定抬起所有键 | 成功 |
| `clip.exe` | `OpenClipboard` 一族能读写 `CF_UNICODETEXT`、枚举当前所有格式 | 成功 |


```
dpi awareness  : PerMonitorV2
virtual screen : origin(0,0)  size 1920x1080  (physical)
monitor count  : 1
  [0] \\.\DISPLAY1  rect=(0,0)-(1920,1080)  1920x1080  work=(0,0)-(1920,1020)  primary=1  dpi=120 (125%)
```

**结论：物理像素、1:1、不需要任何换算。**

> 若 DPI 感知没生效，光标最多读到 `1535, 863`、截图也会缺一块 ——
> 那是被 125% 除过的"虚拟化"假坐标。


## 编译

```
g++ -std=c++17 -O2 -municode -static -o screenshot.exe screenshot.cpp -lgdiplus -lgdi32 -luser32
g++ -std=c++17 -O2 -municode -static -o mousepos.exe  mousepos.cpp  -lgdi32 -luser32
g++ -std=c++17 -O2 -municode -static -o move.exe      move.cpp      -lgdi32 -luser32
g++ -std=c++17 -O2 -municode -static -o click.exe     click.cpp     -lgdi32 -luser32
g++ -std=c++17 -O2 -municode -static -o type.exe      type.cpp      -luser32
g++ -std=c++17 -O2 -municode -static -o key.exe       key.cpp       -luser32
g++ -std=c++17 -O2 -municode -static -o clip.exe      clip.cpp      -luser32
```

MinGW `g++`，**不需要 MSVC、不需要 Windows SDK**。参数逐个解释见 [编译说明.md](编译说明.md)。

## 工具

| 工具 | 作用 |
|---|---|
| `screenshot.exe` | 截**单个**显示器 → PNG，并打印布局与坐标映射 |
| `mousepos.exe` | 实时打印光标物理坐标 |
| `move.exe` | 移动光标 + **读回验证**（防静默钳制） |
| `click.exe` | 点击；动作前报告光标下的窗口；位置没验证通过**拒绝点击** |
| `type.exe` | 按 `KEYEVENTF_UNICODE` 逐字符输入文本，间隔可调 |
| `key.exe` | 发单个键或组合键，可设按住时长；退出前一定补 `KEYUP` |
| `clip.exe` | 读写剪贴板文本，或列出当前所有格式 |

```
screenshot.exe                        # -> shot.png
screenshot.exe --list                 # 只看布局
move.exe 960 540                      # 移到物理坐标
move.exe --dx 40 --dy 0               # 相对移动
click.exe 960 540 --dry               # 只报告将要点击什么
click.exe --double                    # 在当前位置双击
mousepos.exe --count 10

type.exe "hello 你好"                  # 逐字符输入（先点一下目标窗口；cmd 里要写全 type.exe）
type.exe "abc" --interval 300         # 每字符间隔 300ms
key.exe ctrl+s                        # 组合键；--hold MS 设按住时长
clip.exe --set "文字"                  # 写剪贴板（配 key.exe ctrl+v 发长文本最快）
clip.exe --get / --list               # 读回来 / 看当前有哪些格式
```

## 设计约定

1. 每个需要鼠标或屏幕的 exe 的第一行都是 `WmEnablePerMonitorV2()`，早于任何 GDI / 窗口调用
   （键盘 / 剪贴板工具不碰坐标，是这条的例外）
2. 只谈物理像素，工具不做缩放换算
3. 原子操作，尽量不缓存，比如屏幕数量、大小等
4. 考虑到win的字符问题，控制台尽量 ASCII，只有路径窗口标题走 UTF-8 输出
5. 不信返回值 —— 动作之后通过"读回状态"或"看截图"验证
   （比如 `SendInput` 会返回成功却什么都没做）

---

## 文件

| 文件 | 作用 |
|---|---|
| `winmon.h` | 共用基础设施：DPI 感知、显示器枚举、UTF-8 输出 |
| `screenshot.cpp` | 截单个显示器，输出 PNG + 打印布局与坐标映射 |
| `mousepos.cpp` | 实时打印光标物理坐标 |
| `move.cpp` | 移动光标 + 读回验证 |
| `click.cpp` | 鼠标点击（含动作前目标报告） |
| `type.cpp` | `SendInput` + `KEYEVENTF_UNICODE` 逐字符输入文本 |
| `key.cpp` | `SendInput` + 虚拟键，组合键 / 按住时长 / 退出前补 `KEYUP` |
| `clip.cpp` | 剪贴板读写与格式枚举 |
| `编译说明.md` | 编译参数详解 |

## 其他

本文件仅作为可行性测试的说明文件和记录文件。实际功能与此无关。