// push 闸门的判据用例（对应 support/push_guard.h）。
//
// 这一组是纯逻辑：不碰 Git、不起子进程，所以两个平台都要编进来。
// 它守的是本轮授权的边界——允许在隔离临时仓库里测 commit，不允许任何 push；
// 分组表决定「选哪些用例」，这里的判据决定「真跑到夹具/命令窗口时会不会被拦下」。
#include "support/push_guard.h"
#include "support/tiny_test.h"

#include <string>
#include <vector>

namespace {

using gc::test::DescribeArguments;
using gc::test::GuardGitPushForTest;
using gc::test::LooksLikeGitPush;

std::vector<std::wstring> Args(const std::vector<std::wstring>& arguments) {
  return arguments;
}

}  // namespace

GC_TEST(push_guard_flags_the_push_subcommand) {
  GC_CHECK(LooksLikeGitPush(Args({L"push"})));
  GC_CHECK(LooksLikeGitPush(Args({L"push", L"origin", L"main"})));
  GC_CHECK(LooksLikeGitPush(Args({L"push", L"--force-with-lease", L"origin", L"main"})));
}

GC_TEST(push_guard_skips_git_global_options_before_the_subcommand) {
  // 子命令位前面可以排全局选项：判据要跳过它们，否则「-C 目录 push」这种形态会漏网。
  GC_CHECK(LooksLikeGitPush(Args({L"-C", L"/tmp/repo", L"push", L"origin"})));
  GC_CHECK(LooksLikeGitPush(Args({L"--git-dir", L"/tmp/repo/.git", L"push", L"origin", L"main"})));
  GC_CHECK(LooksLikeGitPush(Args({L"-c", L"push.default=current", L"push"})));
  // 带等号的全局选项自带值，只占一个参数位。
  GC_CHECK(LooksLikeGitPush(Args({L"--git-dir=/tmp/repo/.git", L"push"})));
  GC_CHECK(LooksLikeGitPush(Args({L"--no-pager", L"-C", L"repo", L"push", L"--all"})));
}

GC_TEST(push_guard_lets_local_stash_push_through) {
  // `git stash push` 只往本地 refs/stash 写，不碰任何远端：按子命令位判，它是 stash。
  GC_CHECK(!LooksLikeGitPush(Args({L"stash", L"push", L"--include-untracked", L"-m", L"外部工具"})));
  GC_CHECK(!LooksLikeGitPush(Args({L"stash", L"pop"})));
}

GC_TEST(push_guard_ignores_push_text_outside_the_subcommand_slot) {
  // 推送相关的字样大量出现在别的位置：配置键、远端名、待推送的引用说明。
  // 只认子命令位，才能既不误伤这些只读/本地命令，也不漏掉真推送。
  GC_CHECK(!LooksLikeGitPush(Args({L"config", L"remote.origin.pushurl", L"/tmp/bare.git"})));
  GC_CHECK(!LooksLikeGitPush(Args({L"config", L"push.default", L"current"})));
  GC_CHECK(!LooksLikeGitPush(Args({L"config", L"--get", L"branch.main.merge"})));
  GC_CHECK(!LooksLikeGitPush(Args({L"remote", L"add", L"origin", L"/tmp/bare.git"})));
  GC_CHECK(!LooksLikeGitPush(Args({L"ls-remote", L"/tmp/bare.git"})));
  GC_CHECK(!LooksLikeGitPush(Args({L"fetch", L"origin"})));
  GC_CHECK(!LooksLikeGitPush(Args({L"commit", L"-m", L"先把这条提交出去"})));
  GC_CHECK(!LooksLikeGitPush(Args({L"rev-parse", L"HEAD"})));
  GC_CHECK(!LooksLikeGitPush(Args({})));
}

GC_TEST(push_guard_refuses_until_the_run_is_granted) {
  const bool granted = gc::test::GitPushAllowed();
  struct Restore {
    bool previous;
    ~Restore() { gc::test::SetGitPushAllowed(previous); }
  } restore{granted};

  gc::test::SetGitPushAllowed(false);
  bool threw = false;
  try {
    GuardGitPushForTest({L"push", L"origin", L"main"}, "用例");
  } catch (const gc::test::GitPushForbidden& failure) {
    threw = true;
    // 拒绝信息必须把被拦下的命令带出来，才有「清晰原因」可查（非 ASCII 降级成 ? 供展示）。
    GC_CHECK(std::string(failure.what()).find("git push origin main") != std::string::npos);
  }
  GC_CHECK_MESSAGE(threw, "未放行时 push 必须被拦下");

  // 同一道闸门放行后不再拦（--group git-push 由使用者本人跑的那条路）。
  gc::test::SetGitPushAllowed(true);
  bool threwAfterGrant = false;
  try {
    GuardGitPushForTest({L"push", L"origin", L"main"}, "用例");
  } catch (const gc::test::GitPushForbidden&) {
    threwAfterGrant = true;
  }
  GC_CHECK_MESSAGE(!threwAfterGrant, "明确放行后不该再拦");

  // 无论放不放行，非推送命令都不归它管。
  gc::test::SetGitPushAllowed(false);
  bool blockedLocal = false;
  try {
    GuardGitPushForTest({L"commit", L"-m", L"x"}, "用例");
  } catch (const gc::test::GitPushForbidden&) {
    blockedLocal = true;
  }
  GC_CHECK_MESSAGE(!blockedLocal, "提交命令不该被 push 闸门拦下");
}

GC_TEST(push_guard_describes_refused_arguments_without_raw_bytes) {
  // 诊断文本只用于展示：非 ASCII 一律降级，避免把无法无损表示的东西当成原值引用。
  const std::string text = DescribeArguments({L"push", L"origin", L"中文分支"});
  GC_CHECK(text == "push origin ????");
}
