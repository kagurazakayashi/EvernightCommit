#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/commit_plan.h"        // RepositoryWorkflowState（Git 目录里的流程痕迹）
#include "git/undo_commit_plan.h"   // 复用 UndoQueryRead / ReadUndoQuery 这套判读

namespace gc::git {

// 「按操作历史里的线索恢复一次本地引用移动」的可移植判读逻辑（本模块不碰任何 Win32 API、
// 不起子进程、不读文件系统；查询参数交给平台层在后台只读执行，判读与命令构造在这里）。
//
// 它服务于「操作历史 → 恢复入口」：用户从一条历史记录点「恢复」时，本模块把
//   1) 记录里存下的恢复线索（目标分支、操作后该分支应指的对象、要挪回去的对象、是否回到「无引用」）；
//   2) 平台层刚刚读回的仓库实况（那条分支现在到底指着什么、要挪去的那个对象还在不在、有没有流程停着）
// 判成一份「能不能安全恢复、恢复命令长什么样、要如实说明哪些后果」的结论。
//
// 恢复的语义就是「把某条分支引用挪回一次已完成操作之前的位置」，命令形态与「撤回最近提交」
// 完全同族——带预期旧值的原子 git update-ref（AGENTS：复用已修复的原子引用保护，绝不改用
// 更激烈的手段）：
//   * 普通回退：`git update-ref --create-reflog -m <reason> <完整分支引用> <要挪去的新ID> <预期旧ID>`
//   * 回到「该分支本不该存在」：`git update-ref -d -m <reason> <完整分支引用> <预期旧ID>`
// 两条都只移动那一个引用，索引与工作区一个字节都不动；Git 在引用现值不等于预期旧值时以非 0 拒绝、
// 引用原样不动。于是「确认之后分支又被别人推进」不会让恢复落错位置——这与撤回是同一条保证。
//
// 关键契约：这里绝不因为「记录当年写着可恢复」就直接执行旧命令。恢复前一律重新预检——
//   * 目标分支已被移动（现值 ≠ 预期的「操作后应指对象」）：如实说它现在在哪，判定为不可自动执行；
//     发出的命令也会被 Git 用预期旧值挡下，因此这里默认不发、只说明（复制命令时同样标注这一点）。
//   * 目标分支已被删除：按「回到无引用」与「重建引用」两种方向分别判断，绝不臆测它该在哪。
//   * 要挪去的那个对象不可达（被回收、或本就不在本地）：拒绝，不联网补、不自动 fetch。
//   * 有 Git 流程停着（merge/rebase/cherry-pick/…）：那种状态下引用被流程当进度记录用，
//     此时移动引用会搅乱现场，一律拒绝，让用户先把原流程走完。
//   * 实况读不回来（超时、启动失败、输出不完整）：判成「不知道」，绝不当成「干净」或「没有」。
//
// 本模块不产生、也绝不暗示任何 force push / hard reset / clean / 批量撤销：涉及远端历史的恢复
// 由上层按「只解释、只复制、不自动执行」处理（见 HistoryRestoreKind::manualRemote），不在这条链路上。

// ---- 只读预检的查询参数（与撤回同一族：显式 -C 绑定仓库根，全部 --no-optional-locks）----

// 问「这条完整分支引用现在指着哪个提交」：`rev-parse --no-replace-objects --verify --quiet <ref>^{commit}`。
// 这是撤回预检里 <ID>^{commit} 剥离问法用在「命名 ref」上的同族形态：
//   * 引用存在且指向一个提交：退出码 0，输出那个完整提交 ID；
//   * 引用不存在、或解不出一个提交（如指向一个被删的对象）：退出码 1 + 空输出（「--quiet 系」的明确「没有」）；
//   * 仓库坏到问不动：其余非 0，判成「问不出来」，绝不当「没有」。
// branchRef 不是合格的完整引用名时返回空数组：调用方据此跳过，绝不把形态不明的名字送进 Git。
[[nodiscard]] std::vector<std::wstring> BuildRestoreBranchValueArguments(std::wstring_view repositoryDirectory,
                                                                         std::wstring_view branchRef);

// 要挪去的那个对象可达吗：复用撤回里同族的 <ID>^{commit} 剥离问法（BuildUndoParentObjectArguments）。
// 这里只是给一个语义名字，实现就是那条已实测的查询，不另发明命令形态。
[[nodiscard]] std::vector<std::wstring> BuildRestoreTargetObjectArguments(std::wstring_view repositoryDirectory,
                                                                         std::wstring_view targetObjectId);

// ---- 恢复预检的输入：记录里的线索 + 平台层读回的实况 ----

// 从一条历史记录取出的恢复线索（UI 层从 OperationRecord 的 restore* 字段填，纯逻辑不认得记录格式）。
struct RestoreClues {
  bool valid = false;
  std::wstring repositoryRoot;       // 这次操作所属的工作区根
  std::wstring branchRef;            // 被移动的分支引用（refs/heads/…）
  std::wstring expectedCurrentOid;   // 「操作的效果若还在」该分支现在应指的完整对象 ID（= 原子命令的预期旧值）
  std::wstring moveToOid;            // 恢复要把该分支挪去的那个对象（= 命令的新值）；isRootDeletion 时为空
  bool isRootDeletion = false;       // 恢复效果是删除该引用（回到该分支本不存在的状态）
  std::wstring originalNote;         // 记录里那句面向人的恢复线索（原样带进确认框，已在上游脱敏）
};

// 平台层读回的实况（哪条查询跑没跑用布尔如实带，判读函数自己不猜）。
struct RestorePreflightQueries {
  bool branchRan = false;
  GitQueryResult branchValue;   // <branchRef>^{commit}
  bool moveToRan = false;       // isRootDeletion 时根本不跑
  GitQueryResult moveToValue;   // <moveToOid>^{commit}
  RepositoryWorkflowState workflow;
  bool workflowProbed = false;
};

// 恢复可行性的分类。除 feasible 外一律 blocked：不自动发命令，只把话说清楚。
enum class RestoreFeasibility {
  undetermined = 0,   // 实况没读回来 / 线索本身不成立：不知道，绝不臆测
  inProgressFlow,     // 仓库里有 Git 流程停着：那种状态下不移动引用
  branchMissing,      // 目标分支已不存在（读回来是明确的「没有」）
  branchMoved,        // 目标分支还在，但已不指向「操作后应指的那个对象」：现场已变
  moveToMissing,      // 要挪回去的那个对象在本地读不到：历史不完整，拒绝且不联网补
  feasible,           // 分支仍停在预期位置、要挪去的对象可达（或按删除处理）：可以给一条带预期旧值的命令
};

[[nodiscard]] std::wstring_view RestoreFeasibilityLabel(RestoreFeasibility feasibility) noexcept;

struct RestorePlan {
  RestoreFeasibility feasibility = RestoreFeasibility::undetermined;
  std::wstring explanation;  // 面向界面的完整说明（任何分类都要把话说完）

  bool blocked = true;                // feasible 之外都为 true：不产出可执行命令
  std::vector<std::wstring> arguments;  // feasible 时：update-ref 的参数数组（不含 -C，命令窗口自绑工作目录）
  std::wstring operationId;           // 执行器操作 ID（纯 ASCII：restore-ref）
  std::wstring displayName;           // L"按记录恢复引用"
  std::wstring commandLabel;          // 展示用命令开头，如 L"git update-ref"
  std::wstring previewText;           // 恢复前确认框正文（说明现状 / 原始完整 ID / 目标分支 / 索引工作区影响）
  std::wstring notice;                // 随操作显示的范围说明
  std::wstring copyableCommand;       // 供「复制恢复命令」的精确一行（含目标与预期旧值），复制不等于执行

  // 方案绑定的三件事，界面与测试直接读它们、不从 arguments 反推：
  std::wstring targetRef;             // 完整分支引用（refs/heads/…）
  std::wstring expectedOldObjectId;   // 命令里的预期旧值（= 操作后该分支应指的对象）
  std::wstring newObjectId;           // 命令要把分支挪去的新值；删除形态为空
  std::wstring observedCurrentOid;    // 预检读回的「这条分支现在真正指着的对象」（branchMissing / 未读回时为空）
};

// 主入口：把线索与实况判成恢复方案。feasible 之外的每一种都不产出可执行命令。
// 只有 feasible 会填 arguments/previewText/copyableCommand；branchMoved 额外填 copyableCommand，
// 但明确标注「照发也会被 Git 挡下」，交界面决定只复制还是放弃。
[[nodiscard]] RestorePlan BuildRestorePlan(const RestoreClues& clues,
                                           const RestorePreflightQueries& queries);

// 「确认后、执行前的复核」裁决：重新问一次分支现值，与方案绑定的那份预期旧值比对。
// 返回空串 = 仍一致，可以发那条 update-ref；否则返回要原样写进「任务状态」的完整说明
// （「复核没读回来」与「分支在这期间又被挪走/删掉」分别说清，绝不合并成一句“请重试”）。
// 调用方拿到非空答案就不得发命令：放弃的是「这一份现状」，不是用户的恢复意图。
[[nodiscard]] std::wstring DescribeRestoreRecheckMismatch(const GitQueryResult& recheckBranch,
                                                          std::wstring_view expectedOldObjectId,
                                                          std::wstring_view branchRef);

}  // namespace gc::git
