#pragma once

#include <cstdint>
#include <string>

#include "app/submodule_journey.h"
#include "git/submodule_navigation.h"
#include "git/workspace_model.h"
#include "platform/windows/submodule_probe.h"
#include "ui/operation_host.h"

namespace gc::ui {

// 「父仓库 ⇄ 子模块」的导航控制器。
//
// 进入之前先做一次只读探测（那个目录在不在、Git 认不认它是**当前这个**父仓库登记的子模块、
// 父索引里那条记录到底是什么），全部核对通过才把界面绑定的仓库换过去；换过去之前把父仓库
// 界面上那份未提交的表单草稿连同来路一起收进代管，回到那个仓库时原样交还。
// 返回时逐层退（嵌套子模块就是多层的栈），回到父仓库后再问三份位置——父索引记的、父提交记的、
// 子模块自己 HEAD 的——并提示下一步该由谁做。
//
// 这条链路上做的事只有「问」与「换绑定的目录」：不改父索引、不产生提交、不访问远端，
// 也不递归处理任何子模块。初始化/更新子模块会联网并改动工作区，绝不在点导航时隐式触发。
class SubmoduleFlow {
public:
  // 点击「进入子模块」。item 必须已由主窗口与刚读回来的模型逐行核对过（行号与条目的对应
  // 关系只在最后一次显示落地时成立）。
  void Enter(CommitOperationHost& host, const OperationContext& ctx, const git::ChangeItem& item);

  // 点击「返回父仓库」：一层一层退，退到哪个父仓库由栈顶记下的那一条说了算。
  void ReturnToParent(CommitOperationHost& host, const OperationContext& ctx);

  // 两条只读探测的完成通知（迟到、或仓库已经换掉的結果一律作废）。
  void OnEntryProbeCompleted(CommitOperationHost& host, const OperationContext& ctx,
                             uint64_t completionSerial);
  void OnPointerProbeCompleted(CommitOperationHost& host, const OperationContext& ctx,
                               uint64_t completionSerial);

  // 仓库切换落地（识别成功且绑定的正是这次导航要去的高目录）后由主窗口调用：
  // 交还代管的草稿；如果这次落地是「返回父仓库」，接着核那三份位置。
  void OnRepositoryArrived(CommitOperationHost& host, const OperationContext& ctx);
  // 切换没有落地（目标目录识别失败、或根本没有工作区）：代管里的内容一律原样留着，并说清。
  void OnRepositoryArrivalFailed(CommitOperationHost& host, const OperationContext& ctx);

  // 探测在途，或切换已发起、还在等识别落地。
  [[nodiscard]] bool Active() const noexcept { return stage_ != Stage::none; }
  // 有没有可以退回去的一层（按钮可用性用；真正的准入判定在 ReturnToParent 里再做一遍）。
  [[nodiscard]] bool CanReturn() const noexcept { return !journey_.empty(); }
  [[nodiscard]] size_t Depth() const noexcept { return journey_.depth(); }

  // 窗口关闭：两阶段收尾（喊停后剩余的只读查询不再发起，识别也一样在下一条前核对停止信号）。
  void BeginStop();
  void JoinWorkers();

private:
  enum class Stage {
    none,
    entering,        // 进入之前的只读探测在跑
    awaitingEntry,   // 探测已过，切换已发起，等识别落地
    awaitingReturn,  // 返回的切换已发起，等识别落地
    pointing,        // 回到父仓库之后，那三份位置的只读核对在跑
  };

  // 把界面上那份表单收成代管记录（键与根取当前绑定的仓库，不取输入框原文）。
  [[nodiscard]] app::HeldForm CaptureHeld(CommitOperationHost& host,
                                          const OperationContext& ctx) const;
  // 落地之后交还代管草稿：直接交 / 先问一句 / 没有可交的。
  void HandBackForm(CommitOperationHost& host, const OperationContext& ctx,
                    const std::wstring& repositoryKey);
  // 返回父仓库落地后的那三份位置怎么问、怎么说。
  void StartPointerProbe(CommitOperationHost& host, const OperationContext& ctx,
                         const app::JourneyHop& hop);

  platform::SubmoduleEntryWorker entryWorker_;
  platform::SubmodulePointerWorker pointerWorker_;
  app::SubmoduleJourney journey_;
  Stage stage_ = Stage::none;
  // 一次导航在途时：等待落地的那一站是哪个仓库（识别回来的根必须与它一致才算这一站的落地，
  // 否则就是用户自己另选的目录，不该拿这份代管草稿往上面交）。
  std::wstring expectedArrivalRoot_;
  // 点中的那条子模块记录（探测回来时原样带着，避免「探测期间列表又被刷新换了条目」）。
  git::ChangeItem pendingItem_;
  // 返回时弹出来的那一级来路（落地后按它核指针，也按它核对交还是不是该交的那个父仓库）。
  app::JourneyHop pendingHop_;
  bool pendingHopValid_ = false;
};

}  // namespace gc::ui
