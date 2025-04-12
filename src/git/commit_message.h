#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "git/author_config.h"
#include "git/commit_identity.h"

namespace gc::git {

// 提交表单的数据（界面文字，一律寬字符）。把正文轉成 UTF-8 是之後寫提交資訊檔案時的事，
// 本模組不碰編碼邊界，因此校驗與合成都可以用純字串測試。
struct CommitFormData {
  std::wstring subject;                  // 標題（單行、必填）
  std::wstring description;              // 描述（可多行、可空）
  std::wstring author;                   // 「姓名 <邮箱>」
  std::vector<std::wstring> coauthors;   // 每條一個「姓名 <邮箱>」，可以一條也沒有
};

// 校验不通過時問題落在哪個欄位，界面據決定把說明指給使用者看哪裡。
enum class CommitIssueField {
  subject,
  author,
  coauthor,
  committer,
};

struct CommitIssue {
  CommitIssueField field = CommitIssueField::subject;
  size_t coauthorIndex = 0;  // field == coauthor 時的序號（0 起）
  std::wstring message;      // 面向使用者的原因
};

struct CommitFormValidity {
  std::vector<CommitIssue> issues;
  CommitterIdentityState committer = CommitterIdentityState::unknown;
  // 供界面复述「校验看到了什么」：標題字數、描述行數（去掉首尾空行後）、合作者条数。
  size_t subjectCharacters = 0;
  size_t descriptionLines = 0;
  size_t coauthorCount = 0;

  [[nodiscard]] bool Ok() const noexcept { return issues.empty(); }
  // 寫進「任務狀態」那一行的一句话：通過時報告實際規模，不通過時逐條列出原因。
  [[nodiscard]] std::wstring StatusText() const;
};

// 校驗表單資料。只做「必填 / 單行 / 身分格式 / 提交者可得性」這几件事，
// 不要求任何標題格式（Conventional Commits 之類一概不檢查），也不改寫任何文字。
// committer 由呼叫方從有效 Git 配置的判讀結果帶入：本模組不發查詢。
[[nodiscard]] CommitFormValidity ValidateCommitForm(const CommitFormData& data,
                                                   CommitterIdentityState committer);

struct ComposedCommitMessage {
  // 合成結果：以 LF 分行，末尾不帶換行（寫檔時由後續步驟補一個換行即可）。
  std::wstring message;
  size_t bodyLines = 0;        // 描述正文的行數（不含 trailer 段）
  size_t coauthorTrailers = 0;  // 最終訊息裡 Co-authored-by 的總數
  size_t reusedTrailers = 0;    // 描述末尾原本已有、被就地沿用的相同條目
  size_t appendedTrailers = 0;  // 這次新追加到末尾的條目
  // 表單裡解析不了的合作者原文：正常流程校驗先行，這裡不該出現；留著讓呼叫方能如實報告。
  std::vector<std::wstring> rejectedCoauthors;
};

// 合成提交訊息：標題、空行、描述、空行、trailer 段。
//
// 行尾統一為 LF；描述的首尾空行去掉，正文中間的行（含空白行與行尾空格）原樣保留——
// 不翻譯、不潤色、不收拾使用者的文字。
//
// 合作者寫成標準 trailer「Co-authored-by: 姓名 <邮箱>」，放在訊息最末。
// 「描述末尾已有相同合作者」的判定規則（刻意只限於最後一段，不做全域字串替換）：
//   1) 描述按空行分段，只看最後一段；該段每一行都要是「鍵: 值」（鍵到第一個 : 為止、
//      鍵內不含空白），或以空白開頭的續行，且最後一行不是續行，才算 trailer 段；
//      任何一行不符合，整段視為普通正文，一字不動。
//   2) 段內條目按鍵與 "co-authored-by" 做 ASCII 大小寫不敏感比較；值與表單裡的合作者
//      用 CanonicalIdentityKey 比較（姓名與郵箱都摺疊空白，並只對 ASCII 字母做大小寫摺疊）。
//   3) 判定為重複的條目在原位置換成表單裡的寫法；不在表單裡的 Co-authored-by
//      與其它鍵（Signed-off-by 等）原樣保留、順序不變。
//   4) 表單裡沒有被沿用的合作者，依表單順序追加在 trailer 段末尾。
//   5) 表單自身重複的合作者先按同一鍵去重，只寫一條。
// 因此：正文中間出現的「Co-authored-by: ⋯」句子、程式碼片段、以「Note: ⋯」開頭的散文行
// 都不會被刪除或改寫（第 1 步就會把這種段落判成正文）。
//
// 呼叫方應先確認 ValidateCommitForm 通過；未通過時本函數仍會盡力合成，
// 但把解析不了的合作者留在 rejectedCoauthors 裡如實報告，不會憑空變出一條 trailer。
[[nodiscard]] ComposedCommitMessage ComposeCommitMessage(const CommitFormData& data);

}  // namespace gc::git
