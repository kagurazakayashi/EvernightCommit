#include "app/operation_conclusions.h"

namespace gc::app {

std::wstring DescribeFetchConclusion(bool succeeded) {
  // fetch 的结论必须把范围说死：成功只是「远端跟踪引用按 Git 的回答更新了」，
  // HEAD/本地分支/索引/工作区本来就不归它动；失败则明确「不自动重试、不改配置」，
  // 具体原因看命令窗口里留下的真实输出。紧随其后的自动刷新会重读分支摘要。
  return succeeded
             ? std::wstring(L"｜fetch 只按确认框上那份范围更新了远端跟踪引用（.git/FETCH_HEAD 与对象库随抓取变化，"
                            L"这是 fetch 本身的行为）；HEAD、本地分支、索引与工作区不归它动。"
                            L"分支摘要正按新状态重读。")
             : std::wstring(L"｜fetch 未成功：远端跟踪引用是否变化以重读结果为准。本程序不自动重试，"
                            L"也不会因此 prune、换远端或改写任何远端配置；原因看命令窗口里 Git 的真实输出。");
}

std::wstring DescribePullFetchConclusion(bool succeeded) {
  // pull 的两个阶段各自有各自的结论：获取成功只是「远端跟踪引用按承诺更新了」，
  // 整合的结论才涉及分支/索引/工作区。两者都不把「窗口还开着」当成 Git 成功。
  return succeeded
             ? std::wstring(L"｜pull 第一步（获取）完成：只按确认框上那份范围更新了远端跟踪引用"
                            L"（FETCH_HEAD 与对象库随之变化）；正在重读现状并核对本地与远端的关系…")
             : std::wstring(L"｜pull 停在第一步：命令窗口里那次获取没有成功，因此没有做任何整合。"
                            L"本程序不自动重试，也不会因此 prune、换远端或改写任何远端配置。");
}

std::wstring DescribePullIntegrateSuccessConclusion() {
  return L"｜pull 整合完成：本地分支已按刚才预检的那一份提交整合过；"
         L"本程序没有 push、没有 reset、没有 stash，也没有改任何配置。"
         L"现在正在重读分支、列表与历史。";
}

std::wstring DescribePushCommandConclusion(bool succeeded) {
  std::wstring text = succeeded
                          ? std::wstring(L"｜命令窗口里 Git 报告推送成功（退出码 0）。")
                          : std::wstring(L"｜推送没有成功：本程序不自动重试，也不会改用 --force 之类"
                                         L"更激烈的参数。原因看命令窗口里 Git 的真实输出。");
  text += L"正在向确认框上列出的发布目标核实那条引用的实际位置…";
  return text;
}

std::wstring DescribeConflictContinueSuccessConclusion() {
  return L"｜继续这一步按 Git 的退出码（0）判为成功：那条命令是由 Git 建立提交的，"
         L"提交内容、钩子、签名与是否打开编辑器都按它自己的结果算。"
         L"这个仓库现在是否还停在流程里、还剩几个未合并文件，以下面这次重读的结果为准——"
         L"本程序没替你选任何一边的内容，也没替你暂存或退回任何文件。";
}

std::wstring DescribeConflictAbortSuccessConclusion() {
  return L"｜中止这一步按退出码（0）判为成功：Git 说它已经中止了这个流程。"
         L"工作区与索引被改成什么，以刚才命令窗口里的输出与下面的重读为准——"
         L"除那一条 --abort 命令外本程序什么都没做（没 stash、没 clean、没 reset --hard、没删锁）。";
}

}  // namespace gc::app
