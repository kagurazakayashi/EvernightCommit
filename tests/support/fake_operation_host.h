#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ui/operation_host.h"

namespace gc::test {

// 操作控制器的假宿主：把控制器「真的发给界面的东西」原样收下来，供接线层断言。
// 存在的理由是这类缺陷光靠纯计划测试测不出来——参数已经生成、界面文案也写着要跑那条命令，
// 但装配到 CommandWindowOperation 时漏了一个字段，纯逻辑的夹具用例照样全绿。
// 这里只记录，不做任何真事：不起子进程、不弹窗口、不碰文件系统。
class FakeOperationHost final : public gc::ui::OperationHost {
public:
  struct Launch {
    gc::git::CommandWindowOperation operation;
    gc::ui::CommandLaunchOptions options;
  };

  void SetStatus(std::wstring note) override { statuses.push_back(std::move(note)); }
  void SetFormNote(std::wstring note) override { formNotes.push_back(std::move(note)); }
  void RefreshUi() override { ++refreshCount; }

  bool Confirm(const std::wstring& title, const std::wstring& body, bool warningIcon) override {
    confirmations.push_back({title, body, warningIcon});
    return nextConfirmResult.value_or(true);
  }
  void ShowInfo(const std::wstring& title, const std::wstring& body) override {
    infos.push_back({title, body});
  }
  void ShowWarning(const std::wstring& title, const std::wstring& body) override {
    warnings.push_back({title, body});
  }
  bool RiskConfirm(const std::wstring& title, const std::wstring& mainInstruction,
                   const std::wstring& yesButton, const std::wstring& body) override {
    riskConfirms.push_back(
        ConfirmRecord{title, mainInstruction + L" ｜按钮：" + yesButton + L" ｜" + body, false});
    return nextRiskConfirmResult.value_or(true);
  }
  std::optional<size_t> PromptRemoteChoice(gc::platform::RemoteChoiceSpec spec,
                                           const gc::ui::RemoteChoiceLayoutHints&) override {
    remoteChoices.push_back(spec.title);
    return nextRemoteChoice;
  }
  std::optional<std::wstring> PromptForText(gc::platform::IdentityPromptSpec spec,
                                            const gc::ui::TextInputLayoutHints&) override {
    textPrompts.push_back(spec.title);
    return nextText;
  }

  bool LaunchCommandWindow(const gc::git::CommandWindowOperation& operation,
                           const gc::ui::CommandLaunchOptions& options) override {
    launches.push_back({operation, options});
    return launchResults.size() > launches.size() - 1 ? launchResults[launches.size() - 1] : true;
  }
  void ScheduleRefresh() override { ++scheduleRefreshCount; }
  void RememberOperationConclusion(std::wstring_view conclusion) override {
    conclusions.push_back(std::wstring(conclusion));
  }
  void RecordPushVerification(std::wstring_view verificationSummary) override {
    verifications.push_back(std::wstring(verificationSummary));
  }
  bool NavigateRepository(std::wstring_view directory, std::wstring_view statusNote) override {
    navigations.push_back({std::wstring(directory), std::wstring(statusNote)});
    return navigateResult;
  }

  // —— 可编程的答复 ——
  std::optional<bool> nextConfirmResult;
  std::optional<bool> nextRiskConfirmResult;
  std::optional<size_t> nextRemoteChoice;
  std::optional<std::wstring> nextText;
  bool navigateResult = true;
  // 第 N 次启动（下标 0 起）返回什么；没填的按「启动成功」处理。
  std::vector<bool> launchResults;

  // —— 收到的东西 ——
  std::vector<Launch> launches;
  std::vector<std::wstring> statuses;
  std::vector<std::wstring> formNotes;
  struct ConfirmRecord {
    std::wstring title;
    std::wstring body;
    bool warningIcon = false;
  };
  std::vector<ConfirmRecord> confirmations;
  std::vector<ConfirmRecord> riskConfirms;  // title / main+按钮 / body
  std::vector<std::pair<std::wstring, std::wstring>> infos;
  std::vector<std::pair<std::wstring, std::wstring>> warnings;
  std::vector<std::wstring> remoteChoices;
  std::vector<std::wstring> textPrompts;
  std::vector<std::wstring> conclusions;
  std::vector<std::wstring> verifications;
  std::vector<std::pair<std::wstring, std::wstring>> navigations;
  int refreshCount = 0;
  int scheduleRefreshCount = 0;
};

// 最近一条状态栏文字（没有就返回空串，让 GC_CHECK 去显示「什么都没有」这个事实）。
inline std::wstring LastStatus(const FakeOperationHost& host) {
  return host.statuses.empty() ? std::wstring() : host.statuses.back();
}

}  // namespace gc::test
