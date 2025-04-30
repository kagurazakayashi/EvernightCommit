// app/operation_conclusions 的纯逻辑测试：命令窗口操作终态之后追加的范围承诺。
// 这里钉的不是修辞，而是每句承诺必须包含的关键点（fetch 只动远端跟踪引用、
// 不自动重试、不改配置；pull 两步各自负责什么；推送在核实回来之前不下「送到」的断言）。
// 文案改动会同步改变这些关键点时必须先在这里改预期，并确认长期承诺本身有没有变化。
#include <string>
#include <string_view>

#include "app/operation_conclusions.h"
#include "support/tiny_test.h"

namespace {

using gc::app::DescribeFetchConclusion;
using gc::app::DescribePullFetchConclusion;
using gc::app::DescribePullIntegrateSuccessConclusion;
using gc::app::DescribePushCommandConclusion;

bool Contains(std::wstring_view text, std::wstring_view needle) {
  return text.find(needle) != std::wstring_view::npos;
}

GC_TEST(operation_conclusions_fetch_promises_scope_both_ways) {
  const std::wstring ok = DescribeFetchConclusion(true);
  GC_CHECK(Contains(ok, L"｜"));
  GC_CHECK(Contains(ok, L"远端跟踪引用"));
  GC_CHECK(Contains(ok, L"HEAD、本地分支、索引与工作区不归它动"));

  const std::wstring failed = DescribeFetchConclusion(false);
  GC_CHECK(Contains(failed, L"不自动重试"));
  GC_CHECK(Contains(failed, L"不会因此 prune、换远端或改写任何远端配置"));
}

GC_TEST(operation_conclusions_pull_stages_keep_separate_accounts) {
  const std::wstring fetchOk = DescribePullFetchConclusion(true);
  GC_CHECK(Contains(fetchOk, L"pull 第一步（获取）完成"));
  GC_CHECK(Contains(fetchOk, L"正在重读现状并核对本地与远端的关系"));

  const std::wstring fetchFailed = DescribePullFetchConclusion(false);
  GC_CHECK(Contains(fetchFailed, L"pull 停在第一步"));
  GC_CHECK(Contains(fetchFailed, L"没有做任何整合"));

  const std::wstring integrated = DescribePullIntegrateSuccessConclusion();
  GC_CHECK(Contains(integrated, L"没有 push、没有 reset、没有 stash"));
}

GC_TEST(operation_conclusions_push_defers_delivery_to_verification) {
  const std::wstring ok = DescribePushCommandConclusion(true);
  GC_CHECK(Contains(ok, L"Git 报告推送成功（退出码 0）"));
  GC_CHECK(Contains(ok, L"正在向确认框上列出的发布目标核实"));

  const std::wstring failed = DescribePushCommandConclusion(false);
  GC_CHECK(Contains(failed, L"不自动重试"));
  GC_CHECK(Contains(failed, L"不会改用 --force"));
  // 无论成败，核实都要照常发起：那句预告不能只在成功侧存在。
  GC_CHECK(Contains(failed, L"正在向确认框上列出的发布目标核实"));
}

}  // namespace
