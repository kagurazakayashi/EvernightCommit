#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace gc::git {

// 一筆提交身份：「姓名 <郵箱>」拆出來的兩個成分。
//
// 本模組只做文字層面的拆解與裁決，不碰 Win32、不起子進程，因此全部行為都能用純字串測試。
// 後續步驟把身份交給 Git 時应当分別使用 name 與 email（環境變數或 `-c`），
// 不要把整串文字塞進命令列：`git commit --author=<整串>` 的拆解方式對壞形態是「靜默容忍」的。
//
// 為什麼本模組的規則比 Git 更嚴（本機 Git 2.56.0.windows.1 以 `git commit --author=⋯` 實測）：
//   * "Name <mail@example.invalid> extra"  → 姓名 Name、郵箱 mail@example.invalid（尾端文字被丟掉）
//   * "Na<me <mail@example.invalid>"       → 姓名 Na、郵箱 "me mail@example.invalid"（拆錯）
//   * "A <a@b> <c@d>"                      → 姓名 A、郵箱 a@b（第二組被丟掉）
//   * "Name <mail@example.invalid"         → 照樣接受（缺右括號）
//   * "Name <mail>"、"Name <ma il@x>"      → 照樣接受（缺 @ 之外的檢查、郵箱內含空格）
//   * "  <mail@example.invalid>"           → 拒絕：姓名為空（退出碼 128）
// 也就是說 Git 對這些壞形態幾乎不檢查，只會把身分拆成另一個樣子寫進提交，
// 使用者要到 `git log` 裡才發現姓名或郵箱不對。這裡在表單階段就當場拒絕並說明原因。
// 與此同時，檢查清單刻意保持最小：只要求「姓名 <郵箱> 這個形狀」與「郵箱有 @、不殘缺」，
// 不使用正則去挑剔郵箱的域名格式，因此 a@b、user+tag@host、非 ASCII 信箱都照常接受。
struct GitIdentity {
  std::wstring name;
  std::wstring email;

  // 回到界面與文字用的寫法：「姓名 <郵箱>」。
  [[nodiscard]] std::wstring Format() const;
};

// 解析並校驗一條身份。成功時填 *out 並返回 true；失敗時 *out 不被改動，
// *error 寫入面向使用者的原因（句首帶 roleLabel，例如「合作者⋯」），error 可為空指針。
//
// 拒絕清單（任一命中即失敗）：
//   1) 含控制字元或行分隔字元（CR、LF、Tab、NUL、C0/C1、U+2028/U+2029）——換行注入；
//   2) 整串為空白；
//   3) 沒有 `<`、`<` 之後沒有結尾的 `>`、或 `>` 之後還有別的文字；
//   4) 姓名為空，或姓名裡還有 `<`、`>`；
//   5) 郵箱為空、含空白字元，或缺少 `@`、`@` 出現在首尾。
[[nodiscard]] bool ParseGitIdentity(std::wstring_view text, GitIdentity* out, std::wstring* error,
                                    std::wstring_view roleLabel = L"身份");

// 只問「行不行」，返回空字串表示通過，否則返回要顯示的原因。
[[nodiscard]] std::wstring ValidateGitIdentity(std::wstring_view text, std::wstring_view roleLabel);

// 判斷兩條身份是不是「同一位」：姓名與郵箱都先摺疊空白再只做 ASCII 大小寫摺疊
// （「Anne Smith <a@b>」與「anne  SMITH <A@b>」是同一位）。非 ASCII 字母不做大小寫摺疊，
// 因此中文不受影響；給描述末尾的 Co-authored-by 去重與列表去重共同使用。
// 解析失敗時退回「整串摺疊後小寫」，因此壞形態彼此仍能比對，不會憑空被當成兩條。
[[nodiscard]] std::wstring CanonicalIdentityKey(std::wstring_view text);

// 同一份表單裡的合作者清單去重：保留首次出現的那一条文字（原樣，不改寫大小寫），
// 後續與前面某条同鍵的條目被丟掉。*removed 非空時寫入被丟掉的條目原文，
// 界面據此告訴使用者「第 N 条與第 M 条是同一个人」而不是靜默少寫一條。
[[nodiscard]] std::vector<std::wstring> DeduplicateIdentities(const std::vector<std::wstring>& entries,
                                                              std::vector<std::wstring>* removed);

}  // namespace gc::git
