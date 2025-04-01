#include "git/workspace_status.h"

#include <algorithm>

namespace gc::git {
namespace {

// 索引側（X）可能出现的狀態字元：無變化以 '.' 表示（不同於短格式的空格）。
constexpr std::wstring_view kIndexCodes = L".AMDRCT";
// 工作區側（Y）只可能是修改、刪除、類型變化；新增與重命名只發生在暫存之後。
constexpr std::wstring_view kWorktreeCodes = L".MDT";

// 把記錄拆成前 count 個空格分隔欄位，其餘部分原樣留給 rest。
// 路徑可以含空格，因此只能「取固定數量的欄位」，不能整條記錄按空格拆開。
bool TakeFields(std::wstring_view record, size_t count, std::vector<std::wstring_view>* fields,
                std::wstring_view* rest) {
  fields->clear();
  size_t cursor = 0;
  for (size_t index = 0; index < count; ++index) {
    const size_t space = record.find(L' ', cursor);
    if (space == std::wstring_view::npos) {
      return false;
    }
    fields->push_back(record.substr(cursor, space - cursor));
    cursor = space + 1;
  }
  *rest = record.substr(cursor);
  return true;
}

// -z 下每條記錄以 NUL 結尾；重命名/複製記錄的兩條路徑之間也是 NUL，
// 因此來源路徑是「下一個」片段，必須按記錄類型成對消費，絕不能按行或按空格拆。
std::vector<std::wstring_view> SplitNulFields(std::wstring_view text) {
  std::vector<std::wstring_view> fields;
  size_t cursor = 0;
  while (cursor <= text.size()) {
    const size_t nul = text.find(L'\0', cursor);
    const size_t end = (nul == std::wstring_view::npos) ? text.size() : nul;
    fields.push_back(text.substr(cursor, end - cursor));
    if (nul == std::wstring_view::npos) {
      break;
    }
    cursor = nul + 1;
  }
  // 結尾的 NUL 會多出一個空片段，它不是一條記錄。
  if (!fields.empty() && fields.back().empty()) {
    fields.pop_back();
  }
  return fields;
}

// <sub> 是固定 4 字元的子模組狀態欄：普通檔案為「N...」，子模組為「S<c><m><u>」，
// 三個位置分別表示「提交指標已移動」「內部的已追蹤檔案有改動」「內部有未追蹤檔案」。
bool ParseSubField(std::wstring_view field, SubmoduleState* state, std::wstring* error) {
  if (field.size() != 4) {
    *error = L"子模块状态字段长度不符合约定";
    return false;
  }
  if (field[0] == L'N') {
    *state = SubmoduleState{};
    return true;
  }
  if (field[0] != L'S') {
    *error = L"子模块状态字段缺少 N 或 S 前缀";
    return false;
  }
  if (field[1] != L'.' && field[1] != L'C') {
    *error = L"子模块状态的第一位不是约定的 C 或 .";
    return false;
  }
  if (field[2] != L'.' && field[2] != L'M') {
    *error = L"子模块状态的第二位不是约定的 M 或 .";
    return false;
  }
  if (field[3] != L'.' && field[3] != L'U') {
    *error = L"子模块状态的第三位不是约定的 U 或 .";
    return false;
  }
  state->commitChanged = field[1] == L'C';
  state->trackedChanges = field[2] == L'M';
  state->untrackedChanges = field[3] == L'U';
  return true;
}

ChangeKind KindForCode(wchar_t code, bool isSubmodule) {
  if (isSubmodule) {
    // gitlink 的改動一律按子模組呈現：它的處理範圍與普通檔案不同（見 SubmoduleState 說明）。
    return ChangeKind::submodule;
  }
  switch (code) {
    case L'A':
      return ChangeKind::added;
    case L'M':
      return ChangeKind::modified;
    case L'D':
      return ChangeKind::deleted;
    case L'R':
      return ChangeKind::renamed;
    case L'C':
      return ChangeKind::copied;
    case L'T':
      return ChangeKind::typeChange;
    default:
      return ChangeKind::unknown;
  }
}

[[nodiscard]] ChangeItem MakeItem(ChangeKind kind, std::wstring_view statusCode, std::wstring_view path,
                                  std::wstring_view oldPath, std::wstring_view similarity,
                                  const SubmoduleState& submodule) {
  ChangeItem item;
  item.kind = kind;
  item.statusCode = std::wstring(statusCode);
  item.path = std::wstring(path);
  item.oldPath = std::wstring(oldPath);
  item.similarity = std::wstring(similarity);
  item.submodule = submodule;
  return item;
}

// 記錄片段本身的可讀摘要，只用於出錯時說明「哪一條讀不懂」，不帶完整路徑。
[[nodiscard]] std::wstring RecordSample(std::wstring_view record) {
  const size_t limit = std::min<size_t>(record.size(), 40);
  return std::wstring(record.substr(0, limit));
}

size_t CountKind(const std::vector<ChangeItem>& items, ChangeKind kind) {
  return static_cast<size_t>(
      std::count_if(items.begin(), items.end(), [kind](const ChangeItem& item) { return item.kind == kind; }));
}

}  // namespace

std::vector<std::wstring> BuildWorkspaceStatusArguments(std::wstring_view repositoryDirectory) {
  return std::vector<std::wstring>{
      L"-C",
      std::wstring(repositoryDirectory),
      L"--no-optional-locks",
      L"--no-replace-objects",
      L"status",
      L"--porcelain=v2",
      L"-z",
      L"--untracked-files=all",
      L"--ignore-submodules=none"};
}

WorkspaceStatusParseResult ParseWorkspacePorcelainV2(std::wstring_view nulSeparatedOutput) {
  WorkspaceStatusParseResult result;
  const std::vector<std::wstring_view> tokens = SplitNulFields(nulSeparatedOutput);
  std::wstring error;

  for (size_t index = 0; index < tokens.size(); ++index) {
    const std::wstring_view token = tokens[index];
    if (token.empty()) {
      error = L"输出中出现空的记录片段";
      break;
    }
    const wchar_t type = token[0];
    if (type == L'#' || type == L'!' || type == L'/') {
      // # 是未請求的頭部記錄；! 是被忽略的條目（默認不進列表）；
      // / 是被 --ignore-submodules 忽略的子模組變化（本查詢已要求 none，出現時只作計數）。
      ++result.skippedRecords;
      continue;
    }
    if (token.size() < 3 || token[1] != L' ') {
      error = L"记录缺少类型字母与空格：" + RecordSample(token);
      break;
    }

    std::vector<std::wstring_view> fields;
    std::wstring_view path;
    SubmoduleState submodule;
    switch (type) {
      case L'1': {
        if (!TakeFields(token, 8, &fields, &path)) {
          error = L"普通变化记录的字段数不足：" + RecordSample(token);
          break;
        }
        if (!ParseSubField(fields[2], &submodule, &error) || path.empty()) {
          if (error.empty()) {
            error = L"普通变化记录缺少路径";
          }
          break;
        }
        if (fields[1].size() != 2 || kIndexCodes.find(fields[1][0]) == std::wstring_view::npos ||
            kWorktreeCodes.find(fields[1][1]) == std::wstring_view::npos) {
          error = L"普通变化记录的 XY 状态不符合约定：" + std::wstring(fields[1]);
          break;
        }
        // 兩側各自成一條：索引相對 HEAD 的變化進 staged，工作區相對索引的變化進 unstaged。
        const bool isSubmodule = fields[2][0] == L'S';
        if (fields[1][0] != L'.') {
          result.model.staged.push_back(
              MakeItem(KindForCode(fields[1][0], isSubmodule), fields[1], path, {}, {}, submodule));
        }
        if (fields[1][1] != L'.') {
          result.model.unstaged.push_back(
              MakeItem(KindForCode(fields[1][1], isSubmodule), fields[1], path, {}, {}, submodule));
        }
        break;
      }
      case L'2': {
        // 重命名/複製：本片段是目標路徑，來源路徑是下一個 NUL 分隔片段。
        if (!TakeFields(token, 9, &fields, &path)) {
          error = L"重命名记录的字段数不足：" + RecordSample(token);
          break;
        }
        if (index + 1 >= tokens.size()) {
          error = L"重命名记录缺少来源路径";
          break;
        }
        const std::wstring_view oldPath = tokens[++index];
        if (oldPath.empty()) {
          error = L"重命名记录的来源路径为空";
          break;
        }
        if (!ParseSubField(fields[2], &submodule, &error) || path.empty()) {
          if (error.empty()) {
            error = L"重命名记录缺少目标路径";
          }
          break;
        }
        if (fields[1].size() != 2 || (fields[1][0] != L'R' && fields[1][0] != L'C') ||
            kWorktreeCodes.find(fields[1][1]) == std::wstring_view::npos) {
          error = L"重命名记录的 XY 状态不符合约定：" + std::wstring(fields[1]);
          break;
        }
        if (fields[8].empty() || (fields[8][0] != L'R' && fields[8][0] != L'C')) {
          error = L"重命名记录缺少相似度字段：" + std::wstring(fields[8]);
          break;
        }
        result.model.staged.push_back(
            MakeItem(KindForCode(fields[1][0], false), fields[1], path, oldPath, fields[8], submodule));
        if (fields[1][1] != L'.') {
          // 暫存了重命名之後又繼續編輯目標檔案：同一檔案同時出現在兩側，各帶自己的狀態。
          result.model.unstaged.push_back(
              MakeItem(KindForCode(fields[1][1], false), fields[1], path, {}, fields[8], submodule));
        }
        break;
      }
      case L'u': {
        if (!TakeFields(token, 10, &fields, &path)) {
          error = L"未合并记录的字段数不足：" + RecordSample(token);
          break;
        }
        if (!ParseSubField(fields[2], &submodule, &error) || path.empty() || fields[1].size() != 2) {
          if (error.empty()) {
            error = L"未合并记录缺少路径或状态";
          }
          break;
        }
        // 衝突條目只產生一條未暫存條目：它要在工作區裡被解決，解決手段就是暫存，
        // 不能拆成「一次刪除 + 一次新增」冒充兩側都有的普通變化。
        result.model.unstaged.push_back(MakeItem(ChangeKind::conflicted, fields[1], path, {}, {}, submodule));
        break;
      }
      case L'?': {
        path = token.substr(2);
        if (path.empty()) {
          error = L"未跟踪记录缺少路径";
          break;
        }
        // 未追蹤條目在 v2 裡只有類型字母「?」，沒有 XY 兩欄；狀態欄原樣保留這一個字母。
        result.model.unstaged.push_back(
            MakeItem(ChangeKind::untracked, token.substr(0, 1), path, {}, {}, submodule));
        break;
      }
      default:
        error = L"出现无法理解的记录类型：" + RecordSample(token);
        break;
    }
  }

  if (!error.empty()) {
    // 解析出問題時不交出半套模型：界面寧可顯示「讀取失敗」，也不能顯示一份缺檔案的列表。
    result.model = WorkspaceModel{};
    result.error = std::move(error);
  }
  return result;
}

std::wstring BuildWorkspaceSummary(const WorkspaceModel& model) {
  const size_t conflicts = CountKind(model.unstaged, ChangeKind::conflicted);
  std::wstring text = L"工作区已读取：未暂存 " + std::to_wstring(model.unstaged.size()) +
                      L" 项、已暂存 " + std::to_wstring(model.staged.size()) + L" 项。";
  if (conflicts > 0) {
    text += L"其中 " + std::to_wstring(conflicts) +
            L" 项存在冲突，需要先在工作区解决再暂存，不能当成普通删改。";
  }
  return text;
}

std::wstring BuildWorkspaceFailureMessage(RepoError error, std::wstring_view detail) {
  std::wstring text = std::wstring(RepoErrorLabel(error)) + L"（读取工作区状态）";
  if (!detail.empty()) {
    text += L"：" + std::wstring(detail);
  }
  // 讀取是純只讀的，務必把這點講清楚，免得使用者以為程式動過倉庫。
  text += L"。本次只读取仓库状态，没有对仓库做任何改动。";
  return text;
}

}  // namespace gc::git
