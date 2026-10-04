// ===========================================================================
// wire.h —— client 与 server 之间的通信（命名管道）
//
// ---------------------------------------------------------------------------
// 协议（两行）
// ---------------------------------------------------------------------------
//   请求（client -> server）：NUL 分隔的 argv 数组，UTF-8，末尾跟一个 '\n'
//       click\0 100\0 200\0 --button\0 right\0 \n
//       argv 里不可能出现 NUL，所以零歧义、零转义。
//
//   响应（server -> client）：第一行是状态码，第二行是一行 JSON
//       0\n
//       {"ok":true,"op":"click","at":[100,200]}\n
//
//   这样 client 只需要 atoi 第一行来设置自己的退出码，JSON 原样转发 ——
//   【两边都不需要 JSON 解析器】。
//
// ---------------------------------------------------------------------------
// 管道的安全设置（两件事都必须做对，否则 client 根本连不上）
// ---------------------------------------------------------------------------
// 1) DACL 必须授给【登录会话 SID】（Logon SID, S-1-5-5-X-Y）
//
//    DSH 沙箱里的 client 不只是 Low 完整性，它还带一个 write-restricted 令牌。
//    写访问要连过两轮检查：普通 SID 一轮、限制 SID 一轮。
//
//    实测（client 是 Low + write-restricted，用自己建的管道逐个试）：
//        只授"当前用户"            -> 拒绝
//        授 Authenticated Users    -> 拒绝
//        授 INTERACTIVE            -> 拒绝
//        授 BUILTIN\Users          -> 拒绝
//        授 DSH capability SID     -> 拒绝（S-1-4-… 在限制检查里不被接受）
//        授登录会话 SID            -> 【成功】  ← 默认就用它
//        授 Everyone               -> 成功（范围最大，只在 --allow-all-users 时用）
//
//    登录会话 SID 可以从自己的令牌里读出来（TokenGroups 里带 SE_GROUP_LOGON_ID
//    的那一条），所以 server 不需要任何外部告知。它只覆盖"这个用户这一次登录
//    的会话" —— 别的用户、别的登录会话（尤其是 session 0 的服务）都会被排除。
//
//    为什么不用 Everyone：session 0 的服务令牌里也有 Everyone，而服务本来
//    无法跨会话注入输入 —— 授 Everyone 等于给它们开了一条进桌面的通道。
//
// 2) 强制标签必须降到 Low
//
//    server 是 Medium，它建的管道默认也是 Medium 标签 —— 而 Low 的 client 写
//    Medium 对象会被强制完整性检查挡住（NO_WRITE_UP）。
//
//    ⚠️ 注意设置标签的【正确途径】：把 SACL 放进 SECURITY_ATTRIBUTES 交给
//       CreateNamedPipe 是【无效的】—— 创建对象时提供 SACL 需要
//       SeSecurityPrivilege，普通权限进程没有，内核静默忽略。
//       必须建完之后用 SetSecurityInfo(..., LABEL_SECURITY_INFORMATION, ...)
//       单独设，它只要求 WRITE_OWNER（创建者天然拥有）。
//
// ---------------------------------------------------------------------------
// 管道名
// ---------------------------------------------------------------------------
// 固定名字。DACL 限定了只有本登录会话能用，但同一个会话里开两个 server 会
// 撞名 —— 第二个启动时报错退出。
// ===========================================================================

#ifndef WIRE_H
#define WIRE_H

#include <windows.h>

// ---------------------------------------------------------------------------
// 状态码：server 回传，client 直接拿它当自己的退出码
//
// 放在这里而不是 actions.h —— 它是【协议】的一部分，client 也要用，
// 而 client 不包含任何 server 专属的头。
// ---------------------------------------------------------------------------
#define ST_OK        0   // 成功
#define ST_USAGE     2   // 参数错误
#define ST_NO_SERVER 3   // 连不上 server（只有 client 会产生）
#define ST_SERVER    4   // server 侧执行失败
#define ST_REFUSED   5   // 安全拒绝（越界 / 位置未验证 / 被人手取消）

namespace wire {

const wchar_t* PipeName();      // \\.\pipe\simple_cua

// ---------------------------------------------------------------------------
// server 侧
// ---------------------------------------------------------------------------

// 逃生开关：把管道 DACL 放宽到 Everyone。
//
// 默认 false —— 只授当前用户 + 登录会话 SID，范围最小。
// 只有在"server 与 client 不在同一个登录会话"这种异常情况下才需要打开
// （对应 server 的 --allow-all-users 参数）。打开后【任何本地账户】都能连上
// 并驱动这台机器的鼠标键盘，所以 server 启动时会大声警告。
void SetAllowAllUsers(bool on);

// 创建一个管道实例（DACL 授给登录会话 SID；强制标签 = Low）。失败返回 NULL。
HANDLE CreateServerPipe();

// 阻塞等待一个客户端连上。返回 false = 管道坏了（需要重建）。
//
// ⚠️ 成功时【不返回新句柄】—— 连接建立后用的还是同一个 serverPipe 句柄。
//    所以调用方绝不能 CloseHandle 它：那会把管道关掉，下一次 Accept 拿到
//    err=6 (ERROR_INVALID_HANDLE)。
bool AcceptClient(HANDLE serverPipe);

// 是否正在用 Everyone 兜底（读不到登录 SID，或用户显式开了 --allow-all-users）
bool PipeUsesEveryone();

// 诊断用：本次建管道实际授了谁、标签有没有设成功。server 启动时打印出来。
const char* PipeSecuritySummary();

// ---------------------------------------------------------------------------
// client 侧
// ---------------------------------------------------------------------------

// 连接 server。timeoutMs 内连不上返回 INVALID_HANDLE_VALUE。
HANDLE ConnectToServer(int timeoutMs);

// ---------------------------------------------------------------------------
// 收发
// ---------------------------------------------------------------------------

// 写全部字节。失败（对端关闭/出错）返回 false。
bool SendAll(HANDLE h, const void* data, int len);

// ---------------------------------------------------------------------------
// 【请求】的收发：4 字节小端长度 + 内容
//
// 为什么不能用"读到换行符为止"来给请求定界：
//   argv 里【可以含换行】—— type --text 的文本就可能带一个，而那正是
//   "文本换行"这个用法本身。拿换行当分隔符会把请求拦腰截断，而且现象很难猜
//   （会变成一个看起来毫无道理的语法错误）。
//   长度前缀没有这个问题：内容里是什么字节都无所谓。
// ---------------------------------------------------------------------------
bool SendFrame(HANDLE h, const void* data, int len);
int  RecvFrame(HANDLE h, char* out, int cap);   // 返回字节数；-1 = 失败

// ---------------------------------------------------------------------------
// 【响应】仍然按行读
//
// 响应是 server 生成的 JSON，里面的换行一定会被 JsonEscapeAppend 转义成 \n，
// 所以它保证是单行的 —— 按行读在这里是安全的。
// ---------------------------------------------------------------------------

// 收一行（以 \n 结尾）。写入 out 的字符串不含 \r\n。
// 返回【实际字节数】；-1 = 对端先关闭或出错。
int RecvLine(HANDLE h, char* out, int cap);

// 把 UTF-8 的参数列表编码成 NUL 分隔的字节流（每个 token 后跟一个 NUL）。
int EncodeArgv(int n, const char* const* args, char* out, int cap);

// 把 NUL 分隔的 UTF-8 字节流解码成 argv（就地修改 buffer）。
int DecodeArgv(char* buffer, int len, char** argv, int maxArgc);

// ---------------------------------------------------------------------------
// 极简 JSON 生成（只有生成，没有解析）
// ---------------------------------------------------------------------------

// 把字符串按 JSON 规则转义后追加到 out，包含两端的引号。
void JsonEscapeAppend(char* out, int cap, const char* s);

} // namespace wire

#endif // WIRE_H
