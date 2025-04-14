#include "git/workspace_model.h"

#include <algorithm>

namespace gc::git {
namespace {

// 狀態列寬度有限，文案取 Git 官方中文界面常用的說法，不帶標點。
std::wstring_view KindLabel(ChangeKind kind) noexcept {
  switch (kind) {
    case ChangeKind::modified:
      return L"修改";
    case ChangeKind::added:
      return L"新增";
    case ChangeKind::deleted:
      return L"删除";
    case ChangeKind::renamed:
      return L"重命名";
    case ChangeKind::copied:
      return L"复制";
    case ChangeKind::untracked:
      return L"未跟踪";
    case ChangeKind::conflicted:
      return L"冲突";
    case ChangeKind::typeChange:
      return L"类型变化";
    case ChangeKind::submodule:
      return L"子模块";
    case ChangeKind::unknown:
      return L"变化";
  }
  return L"变化";
}

constexpr std::wstring_view kExplicitBreak = L"\r\n";

// 兩行說明：純中文文案沒有可斷行的空格，窄欄內單行繪製會被裁剪。
[[nodiscard]] std::wstring TwoLine(std::wstring_view first, std::wstring_view second) {
  return std::wstring(first) + std::wstring(kExplicitBreak) + std::wstring(second);
}

}  // namespace

std::wstring ChangeItem::StatusLabel() const { return std::wstring(KindLabel(kind)); }

std::wstring ChangeItem::PathLabel() const {
  if (oldPath.empty()) {
    return path;
  }
  return oldPath + L" → " + path;
}

bool WorkspaceModel::HasConflicts() const noexcept {
  const auto conflicted = [](const ChangeItem& item) { return item.kind == ChangeKind::conflicted; };
  return std::any_of(unstaged.begin(), unstaged.end(), conflicted) ||
         std::any_of(staged.begin(), staged.end(), conflicted);
}

std::vector<ChangeItem> WorkspaceModel::SubmoduleChanges() const {
  std::vector<ChangeItem> found;
  const auto collect = [&found](const std::vector<ChangeItem>& items) {
    for (const ChangeItem& item : items) {
      if (item.kind != ChangeKind::submodule) {
        continue;
      }
      // 同一子模組可能同時出現在兩側（指標已暫存、內部又繼續改動），說明只列一次。
      const bool duplicate = std::any_of(found.begin(), found.end(),
                                         [&item](const ChangeItem& existing) {
                                           return existing.path == item.path;
                                         });
      if (!duplicate) {
        found.push_back(item);
      }
    }
  };
  collect(staged);
  collect(unstaged);
  return found;
}

EmptyStateTexts WorkspaceEmptyTexts(WorkspaceLoadStatus status, std::wstring_view failureLabel,
                                   bool repositoryHasCommits, std::wstring_view historyFailureLabel) {
  // 歷史列的說明按「這一段讀取怎麼樣」給：工作區讀取與 git log 掛在同一次刷新上，
  // 但兩者的成敗互相獨立，任何一種空都要講清楚原因。
  const std::wstring historyFailure = TwoLine(L"提交历史读取失败", historyFailureLabel);
  const std::wstring historyEmpty = TwoLine(L"仓库还没有任何提交", L"提交后这里会显示历史记录。");
  const std::wstring historyOddEmpty =
      TwoLine(L"提交历史为空", L"HEAD 可解析却没有读到提交，请点“刷新”重新读取。");
  const auto historyForLoaded = [&]() {
    if (!historyFailureLabel.empty()) {
      return historyFailure;
    }
    return repositoryHasCommits ? historyOddEmpty : historyEmpty;
  };

  switch (status) {
    case WorkspaceLoadStatus::unloaded:
      return EmptyStateTexts{TwoLine(L"暂无数据", L"尚未选择可用仓库。"),
                             TwoLine(L"暂无数据", L"尚未选择可用仓库。"),
                             TwoLine(L"暂无数据", L"尚未选择可用仓库。")};
    case WorkspaceLoadStatus::loading:
      return EmptyStateTexts{TwoLine(L"正在读取", L"后台执行 git status…"),
                             TwoLine(L"正在读取", L"后台执行 git status…"),
                             TwoLine(L"正在读取", L"后台执行 git log…")};
    case WorkspaceLoadStatus::failed:
      return EmptyStateTexts{TwoLine(L"工作区读取失败", failureLabel),
                             TwoLine(L"工作区读取失败", failureLabel),
                             // 工作区这一轮整体失败时 git log 根本沒有跑：說「没有读到」而不是給個空列表。
                             historyFailureLabel.empty() ? TwoLine(L"提交历史未读取", failureLabel)
                                                         : historyFailure};
    case WorkspaceLoadStatus::loaded:
      return EmptyStateTexts{TwoLine(L"没有未暂存的更改", L"工作区与索引一致。"),
                             TwoLine(L"没有已暂存的更改", L"选中文件后用“加入暂存区”暂存。"),
                             historyForLoaded()};
  }
  return EmptyStateTexts{TwoLine(L"暂无数据", L"尚未选择可用仓库。"),
                         TwoLine(L"暂无数据", L"尚未选择可用仓库。"),
                         TwoLine(L"暂无数据", L"尚未选择可用仓库。")};
}

std::wstring SubmoduleExplanationText(const WorkspaceModel& model) {
  const std::vector<ChangeItem> submodules = model.SubmoduleChanges();
  if (submodules.empty()) {
    return {};
  }
  // 這裡必須說明兩件事：父倉庫能暫存的只有提交指標，內部檔案不屬於父倉庫的暫存範圍。
  std::wstring text = L"子模块变化：";
  for (size_t index = 0; index < submodules.size(); ++index) {
    const ChangeItem& item = submodules[index];
    if (index != 0) {
      text += L"；";
    }
    text += item.path + L"（";
    std::wstring states;
    if (item.submodule.commitChanged) {
      states += L"提交指针已移动、";
    }
    if (item.submodule.trackedChanges) {
      states += L"内部有改动、";
    }
    if (item.submodule.untrackedChanges) {
      states += L"内部有未跟踪文件、";
    }
    if (states.empty()) {
      text += L"仅记录提交指针";
    } else {
      states.pop_back();  // 去掉最後一個頓號
      text += states;
    }
    text += L"）";
  }
  text += L"。在父仓库暂存子模块只会记录它的提交指针；子模块内部的改动与未跟踪文件属于那个仓库，"
          L"需要进入子模块工作区另行暂存并提交。";
  return text;
}

}  // namespace gc::git
