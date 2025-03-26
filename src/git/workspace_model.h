#pragma once

#include <string>
#include <vector>

namespace gc::git {

// Git 工作区变化的类别。步骤 6 会按 porcelain v2 字段填充，本步骤只提供界面渲染需要的数据形状。
enum class ChangeKind {
  unknown,
  modified,
  added,
  deleted,
  renamed,
  copied,
  untracked,
  conflicted,
  typeChange,
  submodule,
};

struct ChangeItem {
  ChangeKind kind = ChangeKind::unknown;
  std::wstring statusCode;  // Git 原始两字符状态码（如 "M "、"??"），不从显示文字反解
  std::wstring path;        // 仓库相对路径
  std::wstring oldPath;     // 重命名/复制时的原路径，普通条目为空
};

struct CommitItem {
  std::wstring objectId;  // 完整对象 ID
  std::wstring summary;
  std::wstring author;
  std::wstring authoredAt;
};

struct WorkspaceModel {
  std::vector<ChangeItem> unstaged;
  std::vector<ChangeItem> staged;
  std::vector<CommitItem> recentCommits;

  [[nodiscard]] bool HasAnyItems() const noexcept {
    return !unstaged.empty() || !staged.empty() || !recentCommits.empty();
  }
};

struct EmptyStateTexts {
  const wchar_t* unstaged;
  const wchar_t* staged;
  const wchar_t* history;
};

// 尚未接入 Git 读取时三块列表各自显示的空状态说明。
EmptyStateTexts NotLoadedTexts() noexcept;

// 仓库已识别、但工作区读取（git status / git log）尚未实现时的说明。
EmptyStateTexts LoadedButNotImplementedTexts() noexcept;

}  // namespace gc::git
