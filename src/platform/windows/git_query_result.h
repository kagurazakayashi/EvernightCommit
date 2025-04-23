#pragma once

#include "git/repository.h"
#include "platform/windows/subprocess.h"

namespace gc::platform {

// 哪一路输出没读全、为什么：形如「标准输出超过上限被截断：…；标准错误未能读到结尾：…」。
// 两段说明都由子进程层限长，不含输出原文，可以安全地进界面文案与日志。全部完整时返回空串。
[[nodiscard]] std::wstring DescribeIncompleteStreams(const SubprocessRunResult& run);

// 把一次子进程捕获换成 git 层的查询结果。所有 Windows 侧的 Git 只读查询都经过这里，
// 「这份输出能不能当成 Git 的完整回答」因此只有这一个判定点：
//   * 标准输出那一路必须完整收尾（读到 EOF、没有读失败、没超过字节上限），
//     且整段字节是有效的 UTF-8 —— 半途断掉的多字节字符解码后会变成另一个路径或另一个
//     配置值，拿它去拼命令就是拿残缺事实做决定，因此一律判为不完整；
//   * 标准错误只用于归类与诊断，按宽松规则解码，但它的收尾形态照样记录在案。
// 启动失败与「没读全」的原因文字都已限长，可以安全地进界面与日志。
[[nodiscard]] git::GitQueryResult MakeGitQueryResult(const SubprocessRunResult& run);

}  // namespace gc::platform
