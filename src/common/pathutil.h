// ===========================================================================
// pathutil.h —— 输出路径的解析与可写性探测
//
// 这两件事【必须由 client 做】，不能让 server 做：
//   * 相对路径是相对于【client 被调用时的 cwd】，而 server 是常驻进程，
//     它根本不知道 client 的 cwd 是什么。
//   * 可写性探测必须用【client 自己的令牌】—— 那正是我们要的保守闸门。
// ===========================================================================

#ifndef PATHUTIL_H
#define PATHUTIL_H

// 把路径解析成绝对路径（相对路径以【本进程的 cwd】为基础）。
// 返回 false = 失败（路径过长等）。
bool PathResolveAbsolute(const char* path, char* out, int cap);

// 以【本进程的权限】探测能否写到这个路径。
//
// 为什么这个探测是【保守而安全】的：
//   client 跑在 Low 完整性，而且带 write-restricted 令牌（要连过"普通 SID"和
//   "限制 SID"两轮检查）；server 只有普通令牌，只需过一轮。
//   所以 "client 能写 ⇒ server 一定能写" —— 它只会【误拒】，不会【漏报】。
//   误拒的方向是安全的方向。
//
// ⚠️ 实现必须【无副作用】：
//   - 目标已存在 -> 以 GENERIC_WRITE / OPEN_EXISTING 试开它（不给 TRUNCATE，
//     所以不会动内容）
//   - 目标不存在 -> 以 FILE_ADD_FILE 试开它所在【目录】
//   【不要】用"建个临时文件再删掉"那种探测 —— 那是真副作用，失败时留垃圾，
//   在只读目录上还会误判。
//
// 返回 false 时，whyOut 里写入给 agent 看的原因（英文，一行）。
bool PathProbeWritable(const char* path, char* whyOut, int whyCap);

// 路径是否以指定扩展名结尾（不区分大小写）。ext 要带点，例如 ".png"。
//
// 这是【安全白名单】，不是格式偏好：
// 可写性探测只回答"能不能写"，不回答"该不该写"。没有这条，
//     simple_cua.exe screenshot --out 设计计划.md
// 会把 PNG 写进一个文档里，把文档毁掉。加上之后最坏情况只是覆盖同类文件。
bool PathHasExtension(const char* path, const char* ext);

#endif // PATHUTIL_H
