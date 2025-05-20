#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "git/first_push_plan.h"
#include "git/push_plan.h"
#include "platform/windows/push_probe.h"
#include "ui/operation_host.h"
#include "ui/upstream_write_flow.h"

namespace gc::ui {

// 「推送」操作控制器：点击 → 后台只读预检 → 判读成方案（有风险走强制确认）→ 点头后
// 执行前复核（同一组查询原样重发）→ 命令窗口里的 push → 终态 → 向发布目标逐条只读核实。
// 「确认了哪一份提交、实际去了哪、事后在哪里核实过」必须是同一件事：方案与预检事实都
// 由本类保管，核实请求绑定预检时那个仓库的根，而不是界面此刻显示的根。
//
// 这条链上还挂着「首次推送」分支：预检发现这条分支还没有上游、而仓库里有配好地址的远端时，
// 不再直接拒绝，改由本类多走几步——选目标远端、输入目标分支名、决定要不要设上游、
// 对选定目标做一次只读预检（其中 ls-remote 会访问远端）、确认后复核、推送、核实，
// 最后把「设置本地上游」作为各自有退出码的两条 git config 分别在命令窗口里跑。
// 推送与写上游是两件分开报告的事：任何一步取消或失败都只停下那一步，不自动重发。
class PushFlow {
public:
  // 一次推送走到哪一步。预检与执行前复核共用 pushWorker_，靠这个标记分辨
  // 「回来的那份事实给谁用」；核实走 verifyWorker_，与它们互不干扰。
  // 首次推送那几步各用自己的 worker：候选清单（纯本地）与目标预检（含只读 ls-remote），
  // 两者的超时与作废判定互不混用。
  enum class Stage {
    none,
    probe,               // 点击之后的只读预检在跑
    recheckProbe,        // 点头之后的执行前复核在跑
    pushing,             // 命令窗口里的那条 push 在跑
    verifying,           // 向发布目标核对在跑（这一步不影响仓库，只影响结论措辞）
    firstPushTargets,    // 首次推送：正在逐个远端问实际发布地址（本地只读）
    firstPushProbe,      // 首次推送：选定目标之后的只读预检在跑（含只读 ls-remote）
    firstPushRecheck,    // 首次推送：点头之后的执行前复核在跑（同一组查询原样重发）
    upstreamRecheck,     // 首次推送：推送与核实都结束后，写配置之前再问一次现场（前提是否还在）
    writingUpstream,     // 首次推送：正在命令窗口里逐条写上游配置
  };

  // 入口。调用前主窗口已通过准入判定。
  void Start(OperationHost& host, const OperationContext& ctx);

  // 预检/复核通知（按阶段分派；迟到或换仓库的结果一律作废）。
  void OnProbeCompleted(OperationHost& host, const OperationContext& ctx, uint64_t completionSerial);

  // 首次推送向导的两条通知。
  void OnFirstPushTargetsCompleted(OperationHost& host, const OperationContext& ctx,
                                   uint64_t completionSerial);
  void OnFirstPushProbeCompleted(OperationHost& host, const OperationContext& ctx,
                                 uint64_t completionSerial);

  // 命令窗口里那条 push 的终态：无论成败都发起对发布目标的核实——
  // 命令报成功时要拿它当证据，命令报失败时它正是「远端到底动没动」的唯一凭据。
  // conclusion 是界面已经拼到「命令窗口那头说了什么」的那半句结论。
  void BeginVerification(OperationHost& host, const OperationContext& ctx, bool commandSucceeded,
                         std::wstring_view conclusion);

  // 核实结果回来：结论写进状态栏；与命令窗口的结论不符时另开一个说明框，把逐目标证据摆出来。
  // 首次推送且用户选了「设置上游」而推送确实成功时，这一步结束之后才轮到最后那两条配置命令。
  void OnVerifyCompleted(OperationHost& host, const OperationContext& ctx, uint64_t completionSerial);

  // 上游写入那两步（各自一条 git config、各自一个命令窗口操作）的终态。
  // step 从 1 开始；succeeded 就是那条命令的退出码判定；conclusion 是主窗口已经拼好的那半句。
  void OnUpstreamStepSettled(OperationHost& host, const OperationContext& ctx, int step,
                             bool succeeded, std::wstring_view conclusion);

  // 有没有一次推送在走流程（含命令窗口步、核实步与上游写入步）。
  [[nodiscard]] bool Active() const noexcept { return stage_ != Stage::none; }

  // 命令完成通知丢失时的结案：否则界面永远停在「等那一次推送」上，把下一次点击锁死。
  // 核实本来就只在终态之后才发起，走到这里说明它没机会跑——结论只能以命令窗口那头的输出为准；
  // 上游写入同理：没机会跑就是没跑，一个配置都没写，这句要如实留在状态栏里。
  void AbandonFlow();

  // 窗口关闭：两阶段收尾（喊停后多目标核实不再逐个发起 ls-remote，
  // 首次推送那两组查询也各自在下一条前核对停止信号）。
  void BeginStop();
  void JoinWorkers();

private:
  // 预检回来：判读成方案。blocked 只说明原因（前提不成立时也不硬推）；
  // ready 给确认框（有风险时要求明确点「仍要推送」）。
  void HandleProbe(OperationHost& host, const OperationContext& ctx,
                   const platform::PushProbeOutcome& outcome);
  // 确认框点头之后：把预检那套只读查询原样重发一遍，比对两回事实——一致才启动那条命令。
  void RequestExecutionRecheck(OperationHost& host, const OperationContext& ctx);
  // 复核回来：一致才发命令；不一致就作废并请求重读。
  void HandleRecheck(OperationHost& host, const OperationContext& ctx,
                     const platform::PushProbeOutcome& outcome);

  // ---- 首次推送那几步 ----
  // 预检被「没有上游」卡住、但仓库里确实有可用远端：先逐个远端问出实际发布地址。
  void StartFirstPushTargets(OperationHost& host, const OperationContext& ctx,
                             const git::PushPreflightFacts& baseFacts);
  // 候选回来：摆出远端 → 输入目标分支名 → 决定要不要设上游，然后做一次目标预检。
  void HandleFirstPushTargets(OperationHost& host, const OperationContext& ctx,
                              const platform::FirstPushTargetsOutcome& outcome);
  void StartFirstPushProbe(OperationHost& host, const OperationContext& ctx, bool forRecheck);
  void HandleFirstPushProbe(OperationHost& host, const OperationContext& ctx,
                            const platform::FirstPushProbeOutcome& outcome);
  void HandleFirstPushRecheck(OperationHost& host, const OperationContext& ctx,
                              const platform::FirstPushProbeOutcome& outcome);
  // 上游写入：推送与核实都结束之后，先用与预检同族的只读查询再问一次现场（这条分支还是不是
  // 当初那条、上游是不是已经被人设好），前提还在才逐条发 git config。参数一律取
  // firstPlan_.upstreamSteps 里那份已审查过的数组——发第几条、发给哪个仓库、带什么参数由
  // UpstreamWriteFlow 统一装配（那一段接线单独可测，见 ui/upstream_write_flow.h）。
  void StartUpstreamRecheck(OperationHost& host, const OperationContext& ctx);
  void HandleUpstreamRecheck(OperationHost& host, const OperationContext& ctx,
                             const platform::PushProbeOutcome& outcome);
  void StartUpstreamWrites(OperationHost& host, const OperationContext& ctx);
  // 这一段彻底结束（成功写完 / 某条失败 / 拒绝覆盖 / 一条都没发出）：清掉推送侧的现场状态。
  void SettlePushFlow() noexcept;

  // 在命令窗口里启动一条确定的推送方案（复核已通过）。绑定核实用的那份依据一并记下。
  struct LaunchRequest {
    std::wstring operationId;
    std::wstring displayName;
    std::wstring commandLabel;
    std::vector<std::wstring> argumentList;
    std::wstring scopeNotice;
    std::wstring pushedObjectId;
    std::wstring remoteName;
    std::wstring remoteBranchRef;
    std::vector<std::wstring> pushUrls;
  };
  void Launch(OperationHost& host, const OperationContext& ctx, const LaunchRequest& request);
  // 放弃这次推送（预检失败、用户取消、复核不过）：原因写进状态栏，不打开命令窗口。
  void Abandon(OperationHost& host, std::wstring_view reason);

  platform::PushProbeWorker worker_;
  platform::PushVerifyWorker verifyWorker_;
  platform::FirstPushTargetsWorker targetsWorker_;
  platform::FirstPushProbeWorker firstWorker_;
  Stage stage_ = Stage::none;
  platform::PushProbeOutcome preflight_;  // 预检那一份：复核要和它逐条比对。
  git::PushPlan plan_;

  // 首次推送向导的状态：基准事实（分支与那份提交，用来把候选摆得有意义）、
  // 用户当场选定的目标、预检事实与方案、以及待执行的上游写入步序。
  git::PushPreflightFacts firstPushBase_;
  bool firstPushActive_ = false;
  std::wstring firstPushRemoteName_;
  std::wstring firstPushBranchName_;    // 用户输入的目标分支名（不含 refs/heads/）
  bool firstPushSetUpstream_ = false;
  platform::FirstPushProbeOutcome firstPreflight_;
  git::FirstPushPlan firstPlan_;
  // 上游写入那几条命令的接线（第几条在等终态、下一条发不发）由它自己管，
  // 推送控制器只负责「什么时候轮到它」和「这一段结束后清理现场」。
  UpstreamWriteFlow upstreamWrites_;
  bool upstreamPending_ = false;        // 推送成功后要不要写上游（核实结束之后才执行）
  std::wstring upstreamPushConclusion_;  // 推送那一步的结论文字，供最后一段总结引用

  // 这次推送的核实依据：由 Launch 记下，绑定「确认过的那一份」与「确认框列出的那些去处」。
  LaunchRequest binding_;
  // 命令窗口里那条 push 所绑定的仓库根（启动当场记下）。核实与之后的上游写入都问/写这一个仓库，
  // 界面中途换了仓库也不会把这两步发给另一个仓库——那种场合直接作废剩下的步骤。
  std::wstring boundRepositoryDirectory_;
};

}  // namespace gc::ui
