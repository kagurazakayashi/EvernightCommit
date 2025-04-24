#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/git_environment.h"
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

// ——— 集中环境策略下的统一执行入口：生产代码启动 Git 子进程一律走这两个函数 ———
// 环境块由 platform::BuildGitChildEnvironment 装配：继承项按 git/git_environment.h 的
// 类别保留、删除或受控覆盖（仓库定位、索引/对象库、配置注入、身份/时间一律剔除，
// PATH/HOME/USERPROFILE/SSH 与凭据助手等照常保留）。环境装配失败时根本不启动进程：
// started=false，launchErrorText 给出原因（只含变量名，绝不含任何值）。
// environmentNotice（可空）收到“从继承环境移除的重定向变量”告知文本（只含名字）。
// 命令窗口那条链路不经此处：执行器在启动辅助进程时用同一个装配函数（purpose=commandWindow），
// 辅助进程再把自己的环境原样传给 Git —— 两处共用同一份策略，预检与执行语义因此一致。
[[nodiscard]] SubprocessRunResult RunGitCaptured(std::wstring_view program,
                                                 const std::vector<std::wstring>& arguments,
                                                 std::wstring_view workingDirectory,
                                                 unsigned long timeoutMilliseconds,
                                                 std::wstring* environmentNotice);

// 后台只读查询：purpose=backgroundProbe，并一步完成 MakeGitQueryResult 的完整度判定与解码；
// 环境告知写入 result.environmentNotice。
[[nodiscard]] git::GitQueryResult RunGitBackgroundQuery(const std::wstring& exePath,
                                                        const std::vector<std::wstring>& arguments,
                                                        const std::wstring& workingDirectory,
                                                        unsigned long timeoutMilliseconds);

}  // namespace gc::platform
