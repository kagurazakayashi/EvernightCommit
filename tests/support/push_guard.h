#pragma once

// 本轮授权的可执行边界：AI 会话可以在「用例自有的隔离临时仓库」里跑真实 git commit，
// 但任何情况下都不许跑 git push。分组表只是「选哪些用例」的账，真正的硬闸门在这里——
// 只要本次运行没被明确放行 push，夹具与命令窗口两条执行路径都会把 push 参数数组拦下来，
// 抛 GitPushForbidden 让运行器按「拒绝」单独计数，而不是让它悄悄跑过去。
//
// 这个头文件不含任何平台类型，Windows 与 Linux 都能编，判据本身由
// tests/push_guard_tests.cpp 的用例钉住。

#include <string>
#include <vector>

#include "support/tiny_test.h"

namespace gc::test {

// 本次运行是否放行 git push。默认 false：只有运行器在「用户明确要求跑 git-push 组，
// 并且环境里带了 GC_ALLOW_PUSH=1」时才置真，单看环境变量不算数（避免误设环境就放行）。
inline bool& GitPushAllowedFlag() {
  static bool allowed = false;
  return allowed;
}

inline bool GitPushAllowed() { return GitPushAllowedFlag(); }
inline void SetGitPushAllowed(bool allowed) { GitPushAllowedFlag() = allowed; }

// 参数数组是不是「把提交写到远端」的那条命令。
//
// 判据落在子命令位上，不靠「整条命令行里有没有 push 这几个字母」：
//   * git 的全局选项可以排在子命令之前（-C <目录>、--git-dir <目录>、-c key=value 等），
//     所以要先跳过它们才能找到真正的子命令；
//   * `git stash push` 的 push 是本地引用存储，不写远端，不算；
//   * 配置项名里带 push（remote.origin.pushurl、push.default）出现在值的位置而不是
//     子命令位，按子命令位判定天然不会误伤。
inline bool LooksLikeGitPush(const std::vector<std::wstring>& arguments) {
  static const wchar_t* const kValueTakingGlobalOptions[] = {L"-C",     L"-c",       L"--git-dir",
                                                             L"--work-tree", L"--namespace",
                                                             L"--exec-path", L"--super-prefix"};

  size_t index = 0;
  while (index < arguments.size()) {
    const std::wstring& token = arguments[index];
    if (token.empty()) {
      ++index;
      continue;
    }
    if (token[0] == L'-') {
      // --opt=value 形态自带值，只占一个参数位。
      if (token.find(L'=') == std::wstring::npos) {
        for (const wchar_t* option : kValueTakingGlobalOptions) {
          if (token == option) {
            ++index;  // 连它吃掉的那个值一起跳过
            break;
          }
        }
      }
      ++index;
      continue;
    }
    if (token == L"push") {
      return true;
    }
    // `git stash push` 的 push 只往本地 refs/stash 写，不碰任何远端：子命令位是 stash 即放行。
    return false;  // 第一个非选项参数就是子命令：不是 push 就不算
  }
  return false;
}

// 把被拦下的命令渲染成诊断文本。这里只做展示，不参与任何文件路径或 Git 参数构造，
// 所以非 ASCII 可以降级成 '?'；真正的原始参数仍留在用例源码与断言输出里。
inline std::string DescribeArguments(const std::vector<std::wstring>& arguments) {
  std::string text;
  for (const std::wstring& token : arguments) {
    if (!text.empty()) {
      text += ' ';
    }
    for (const wchar_t c : token) {
      text += (c > 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
  }
  return text;
}

// 拦截点。channel 说明是哪条执行路径（"夹具" / "命令窗口"），便于拒绝信息指到地方。
inline void GuardGitPushForTest(const std::vector<std::wstring>& arguments, const char* channel) {
  if (GitPushAllowed() || !LooksLikeGitPush(arguments)) {
    return;
  }
  throw GitPushForbidden(std::string(channel) + "拦下了会写远端的命令：git " +
                         DescribeArguments(arguments));
}

}  // namespace gc::test
