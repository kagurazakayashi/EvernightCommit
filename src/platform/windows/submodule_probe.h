#pragma once

#include <functional>
#include <string>

#include "git/repository.h"
#include "git/submodule_navigation.h"
#include "platform/windows/git_task_worker.h"
#include "platform/windows/repo_detect.h"

namespace gc::platform {

// 子模块导航的两组只读探测。全部走后台隐藏子进程：不弹命令窗口、不改动任何仓库、不访问远端。
//
//   进入之前 —— 问三件事：那个目录在不在（只读的属性探测）、它是不是「当前这个父仓库登记的
//   子模块工作区」（一次完整的仓库识别，含 --show-superproject-working-tree）、
//   父索引里那条记录到底是什么（`ls-files -s -z`）。
//   返回之后 —— 问三份位置：父索引记的、父提交记的、子模块自己 HEAD 的。
//
// 这里只「收集事实」，判断与文案都在 git/submodule_navigation（纯逻辑，可用桩输出完整测试）。
// 初始化/更新子模块会联网并改动工作区，因此绝不在这条链路上发起。

struct SubmoduleProbeDeps {
  // 父仓库那两条只读查询的执行器（界面用 RunGitBackgroundQuery，夹具用它的隔离执行器）。
  git::GitQueryRunner runner;
  // 对子模块目录跑一次仓库识别所需的依赖（同一套 runner 加上路径工具与存在性检查）。
  RepoDetectDeps detect;
  std::function<bool(const std::wstring&)> directoryExists;
  std::function<bool(const std::wstring&)> regularFileExists;
};

// ---- 进入之前 ----

struct SubmoduleEntryRequest {
  std::wstring exePath;
  std::wstring parentRoot;                // 界面当前绑定的工作区根
  std::wstring submoduleRelativePath;     // Git 给出的仓库相对路径（数据，不是通配表达式）
  git::SubmoduleState flags;              // 父仓库 status 里那条 S<c><m><u>（界面从模型里原样带来）
  bool itemIsSubmodule = false;           // 点中的那一条确实是子模块条目（界面已逐行核对）
  unsigned long timeoutMilliseconds = 0;
  StopFlag stopFlag;
};

struct SubmoduleEntryOutcome {
  git::SubmoduleEntryFacts facts;
  std::wstring repositoryDirectory;  // 原样回显：界面据此判别「这份探测还是刚才那个仓库的」。
};

[[nodiscard]] git::SubmoduleEntryFacts CollectSubmoduleEntryFacts(
    const SubmoduleEntryRequest& request, const SubmoduleProbeDeps& deps);

[[nodiscard]] SubmoduleEntryOutcome RunSubmoduleEntryLoad(const SubmoduleEntryRequest& request);

// ---- 返回之后 ----

struct SubmodulePointerRequest {
  std::wstring exePath;
  std::wstring parentRoot;
  std::wstring submoduleRelativePath;
  std::wstring submoduleDirectory;  // 父根 + 相对路径拼出的目录（由调用方按同一份纯函数算出）
  unsigned long timeoutMilliseconds = 0;
  StopFlag stopFlag;
};

struct SubmodulePointerOutcome {
  git::SubmodulePointerFacts facts;
  std::wstring repositoryDirectory;
};

[[nodiscard]] git::SubmodulePointerFacts CollectSubmodulePointerFacts(
    const SubmodulePointerRequest& request, const SubmoduleProbeDeps& deps);

[[nodiscard]] SubmodulePointerOutcome RunSubmodulePointerLoad(const SubmodulePointerRequest& request);

// 界面装配的依赖：runner 用集中环境策略下的后台只读查询，识别依赖沿用仓库识别那一套。
[[nodiscard]] SubmoduleProbeDeps MakeSubmoduleProbeDeps(unsigned long timeoutMilliseconds);

using SubmoduleEntryWorker = GitTaskWorker<SubmoduleEntryRequest, SubmoduleEntryOutcome>;
using SubmodulePointerWorker = GitTaskWorker<SubmodulePointerRequest, SubmodulePointerOutcome>;

}  // namespace gc::platform
