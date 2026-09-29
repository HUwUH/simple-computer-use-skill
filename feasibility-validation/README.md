# simple-computer-use —— 可行性验证

一个**不依赖任何框架**的 Windows computer-use 工具集的起点。
所有程序都是**单个静态 exe、零运行时依赖、直接调 Win32**。

当前阶段只做**两个可行性验证程序** —— 先把"坐标系"这件事钉死，再谈鼠标键盘。

代码在 [`feasibility-validation/`](feasibility-validation) 里。

---

## ✅ 本机验证结果（已通过）

```
dpi awareness  : PerMonitorV2
virtual screen : origin(0,0)  size 1920x1080  (physical)
monitor count  : 1
  [0] \\.\DISPLAY1  rect=(0,0)-(1920,1080)  1920x1080  work=(0,0)-(1920,1020)  primary=1  dpi=120 (125%)
```

- 光标推到屏幕最右下角，`mousepos.exe` 读到 **`1919, 1079`** —— 正是物理右下角像素
- 截出来的 `shot.png` 实际尺寸 **1920 x 1080** —— 正是该显示器的物理分辨率

**结论：坐标系干净。** 物理像素、1:1、**不需要任何换算**。
（如果 DPI 感知没生效，光标最多读到 `1535, 863`、截图也会缺一块 —— 那是被 125% 除过的假值。）

---

## 编译

两行 `g++` 命令，见 [`feasibility-validation/编译说明.md`](feasibility-validation/编译说明.md)。
**不需要 MSVC，不需要 Windows SDK。**

---

## 验证 1：`screenshot.exe`

```
screenshot.exe            # 截光标所在显示器 -> shot.png
screenshot.exe --list     # 只列显示器
screenshot.exe --monitor 1 --out d2.png
```

**看什么：**

| 检查项 | 期望 |
|---|---|
| `dpi awareness` | `PerMonitorV2` |
| `bitmap size` | **等于该显示器的真实物理分辨率** |
| 图片打开后的像素尺寸 | 同上（用看图工具确认一次） |
| `virtual screen` | 和"显示设置 → 显示器"里的一致 |

**如果 `bitmap size` 是 1536x864 这种数** → DPI 感知没生效，
Windows 给了你一套被 125% 除过的假值，截图也会缺一块。

**`mapping` 那一行是关键输出**：`image(x,y) == screen(左+x, 上+y)`。
后续所有点击坐标都要靠它换算。

**已知限制**：`BitBlt` 抓不到硬件加速内容（游戏、某些视频、部分 UWP）——
那些区域会是黑的。真需要时再上 DXGI Desktop Duplication。
加 `--layered` 可以额外抓分层窗口，但可能闪烁。

---

## 验证 2：`mousepos.exe`

```
mousepos.exe              # 每 500ms 打印一次，Ctrl+C 退出
mousepos.exe --count 10
mousepos.exe --interval 100
```

**看什么：**

1. **把鼠标推到屏幕最右下角** → 读数应当接近 `(1919, 1079)`
2. **推到左上角** → 应当接近 `(0, 0)`
3. 如果读到的是 `(1535, 863)` 这种"被除过"的数 → DPI 感知没生效
4. 插上/拔掉第二块屏，**再跑一次** → 布局应当立刻跟着变

程序还会显示光标在**哪个显示器**、以及**在该显示器内的局部坐标**，
`mon idx = -1` 表示光标处在虚拟桌面里没有显示器的"黑洞区"。

---

## 设计约定（从一开始就守住）

1. **每个 exe 的第一行都是 `WmEnablePerMonitorV2()`**，早于任何 GDI / 窗口调用
2. **只谈物理像素**，永远不做缩放换算
3. 每次运行都重新查询显示器布局，不缓存
4. 控制台输出一律 ASCII，只有路径走 UTF-8 输出助手
5. 不信返回值 —— 每个动作之后都要"读回状态"或"看截图"验证
   （`SendInput` 会返回成功却什么都没做）

---

## 文件

| 路径 | 作用 |
|---|---|
| `feasibility-validation/winmon.h` | 共用基础设施：DPI 感知、显示器枚举、UTF-8 输出 |
| `feasibility-validation/screenshot.cpp` | 截单个显示器，输出 PNG + 打印布局与坐标映射 |
| `feasibility-validation/mousepos.cpp` | 实时打印光标物理坐标 |
| `feasibility-validation/编译说明.md` | 两行 g++ 编译命令 |

---