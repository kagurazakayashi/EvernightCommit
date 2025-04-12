// 提交信息合成与表单校验的纯逻辑测试：不起 Git、不开窗口，只断言「表單資料 → 最終訊息」。
// 覆盖：标题必填且不能全为空白、多行描述与空行排版、中文与特殊字符不被改写、
// 合作者 trailer 的位置与顺序、描述末尾已有相同 trailer 时的去重规则、
// 正文里同名文字的保留、表格内自重复的合作者收攏，以及提交者身份缺失/未知時的結論。
//
// 這裡刻意不檢查任何標題前綴規則：Conventional Commits 不是本程序的約束，
// 測試同時斷言「fix: 之类不被拒」與「中文敘述式標題不被拒」，防止以後誤加規則。
#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "git/commit_message.h"
#include "support/tiny_test.h"

namespace {

using gc::git::CommitterIdentityState;
using gc::git::ComposedCommitMessage;
using gc::git::CommitFormData;
using gc::git::CommitFormValidity;
using gc::git::CommitIssueField;
using gc::git::ComposeCommitMessage;
using gc::git::ValidateCommitForm;

constexpr std::wstring_view kGoodAuthor = L"张三 <zhangsan@example.com>";

CommitFormData Form(std::wstring_view subject, std::wstring_view description = {},
                    std::wstring_view author = kGoodAuthor,
                    const std::vector<std::wstring>& coauthors = {}) {
  CommitFormData data;
  data.subject = std::wstring(subject);
  data.description = std::wstring(description);
  data.author = std::wstring(author);
  data.coauthors = coauthors;
  return data;
}

bool HasField(const CommitFormValidity& validity, CommitIssueField field) {
  for (const gc::git::CommitIssue& issue : validity.issues) {
    if (issue.field == field) {
      return true;
    }
  }
  return false;
}

size_t CountOccurrences(std::wstring_view haystack, std::wstring_view needle) {
  size_t count = 0;
  size_t cursor = 0;
  for (;;) {
    const size_t found = haystack.find(needle, cursor);
    if (found == std::wstring_view::npos) {
      return count;
    }
    ++count;
    cursor = found + needle.size();
  }
}

std::string Utf8(const std::wstring& text) {
  std::string out;
  for (const wchar_t value : text) {
    // 只用於診斷輸出：把非 ASCII 轉成可讀的 <U+xxxx>，避免控制台代碼頁影響判讀。
    if (value < 0x80) {
      out.push_back(static_cast<char>(value));
    } else {
      char buffer[16]{};
      std::snprintf(buffer, sizeof(buffer), "<U+%04X>", static_cast<unsigned>(value));
      out += buffer;
    }
  }
  return out;
}

std::string Describe(const ComposedCommitMessage& composed) {
  return "message=[" + Utf8(composed.message) + "]；正文 " + std::to_string(composed.bodyLines) +
         " 行，trailer 共 " + std::to_string(composed.coauthorTrailers) + " 条（沿用 " +
         std::to_string(composed.reusedTrailers) + "，追加 " + std::to_string(composed.appendedTrailers) +
         "），拒绝 " + std::to_string(composed.rejectedCoauthors.size()) + " 条";
}

}  // namespace

GC_TEST(commit_message_requires_non_blank_subject) {
  const CommitFormValidity emptySubject = ValidateCommitForm(Form(L""), CommitterIdentityState::available);
  GC_CHECK_MESSAGE(HasField(emptySubject, CommitIssueField::subject), "空标题必须报问题");

  const CommitFormValidity blankSubject =
      ValidateCommitForm(Form(L"   \t  "), CommitterIdentityState::available);
  GC_CHECK_MESSAGE(HasField(blankSubject, CommitIssueField::subject), "全为空白的标题同样必须报问题");
  GC_CHECK_MESSAGE(!blankSubject.Ok(), "空白标题不能算校验通过");

  const CommitFormValidity multiline =
      ValidateCommitForm(Form(L"第一行\n第二行"), CommitterIdentityState::available);
  GC_CHECK_MESSAGE(HasField(multiline, CommitIssueField::subject), "标题里出现换行要报问题");

  // 不强制任何标题风格：前缀式与叙述式、中文、带特殊字符的都该通过（只要非空白）。
  for (std::wstring_view subject : {L"fix: 修正登录超时", L"随手记一笔", L"WIP",
                                     L"fix(log): a&b %c \"d\" $e `f` |g"}) {
    const CommitFormValidity validity = ValidateCommitForm(Form(subject), CommitterIdentityState::available);
    GC_CHECK_MESSAGE(validity.issues.empty(), "标题「" + Utf8(std::wstring(subject)) + "」不该报任何问题");
  }
}

GC_TEST(commit_message_subject_only_has_no_extra_blank_lines) {
  const ComposedCommitMessage composed = ComposeCommitMessage(Form(L"只有一句标题"));
  GC_CHECK_MESSAGE(composed.message == L"只有一句标题", Describe(composed));
  GC_CHECK_MESSAGE(composed.bodyLines == 0, "没有描述时正文行数应为 0");
  GC_CHECK_MESSAGE(composed.coauthorTrailers == 0, "没有合作者时不该出现 trailer");
  GC_CHECK_MESSAGE(composed.message.back() != L'\n', "末尾不带换行（写档时由后续步骤补）");
}

GC_TEST(commit_message_keeps_description_verbatim) {
  // 多行描述：段落之间的空行保留，行内文字一个不改（不翻译、不润色、不修剪行尾空格以外的内容）。
  const CommitFormData data = Form(L"标题",
                                   L"第一段：中文说明\r\n继续第一段。\r\n"
                                   L"\r\n"
                                   L"第二段：保留原样，不做任何改写。");
  const ComposedCommitMessage composed = ComposeCommitMessage(data);
  GC_CHECK_MESSAGE(composed.message ==
                       L"标题\n\n第一段：中文说明\n继续第一段。\n\n第二段：保留原样，不做任何改写。",
                   Describe(composed));
  GC_CHECK_MESSAGE(composed.message.find(L'\r') == std::wstring::npos, "CRLF 要统一成 LF");
  GC_CHECK_MESSAGE(composed.bodyLines == 4, Describe(composed));
}

GC_TEST(commit_message_trims_blank_edges_only) {
  const ComposedCommitMessage composed =
      ComposeCommitMessage(Form(L"  标题两端空白会被修剪  ", L"\r\n\r\n正文\r\n\r\n\r\n"));
  GC_CHECK_MESSAGE(composed.message == L"标题两端空白会被修剪\n\n正文", Describe(composed));
}

GC_TEST(commit_message_appends_coauthor_trailers) {
  const ComposedCommitMessage one =
      ComposeCommitMessage(Form(L"标题", L"", kGoodAuthor, {L"李四 <lisi@example.com>"}));
  GC_CHECK_MESSAGE(one.message == L"标题\n\nCo-authored-by: 李四 <lisi@example.com>", Describe(one));
  GC_CHECK_MESSAGE(one.appendedTrailers == 1 && one.reusedTrailers == 0, Describe(one));
  GC_CHECK_MESSAGE(one.coauthorTrailers == 1, Describe(one));

  const ComposedCommitMessage many = ComposeCommitMessage(
      Form(L"标题", L"一段描述", kGoodAuthor,
           {L"李四 <lisi@example.com>", L"王五 <wangwu@example.com>"}));
  GC_CHECK_MESSAGE(many.message ==
                       L"标题\n\n一段描述\n\nCo-authored-by: 李四 <lisi@example.com>\n"
                       L"Co-authored-by: 王五 <wangwu@example.com>",
                   Describe(many));

  // 合作者头尾空白与大小写差异不该影响 trailer 的写法：收进列表前已修剪，这里照修剪后的写。
  const ComposedCommitMessage trimmed =
      ComposeCommitMessage(Form(L"标题", L"", kGoodAuthor, {L"  李四  <lisi@example.com>  "}));
  GC_CHECK_MESSAGE(trimmed.message == L"标题\n\nCo-authored-by: 李四 <lisi@example.com>",
                   Describe(trimmed));
}

GC_TEST(commit_message_does_not_repeat_existing_trailer) {
  // 描述末尾已经是标准 trailer 段，且已有同一位合作者：不重复追加，位置也保留在原处。
  const ComposedCommitMessage reused = ComposeCommitMessage(
      Form(L"标题", L"正文一行\n\nCo-authored-by: 李四 <lisi@example.com>", kGoodAuthor,
           {L"李四 <lisi@example.com>"}));
  GC_CHECK_MESSAGE(reused.message == L"标题\n\n正文一行\n\nCo-authored-by: 李四 <lisi@example.com>",
                   Describe(reused));
  GC_CHECK_MESSAGE(reused.reusedTrailers == 1, "已有的同一条应被就地沿用");
  GC_CHECK_MESSAGE(reused.appendedTrailers == 0, "不应再追加一份") ;
  GC_CHECK_MESSAGE(CountOccurrences(reused.message, L"Co-authored-by:") == 1,
                   Describe(reused));

  // 邮箱大小写不同算同一位（ASCII 折叠），姓名内部空格不同的也算同一位。
  const ComposedCommitMessage folded = ComposeCommitMessage(
      Form(L"标题", L"正文\n\nCo-authored-by: Li Si <LI.SI@Example.COM>", kGoodAuthor,
           {L"Li  Si <li.si@example.com>"}));
  GC_CHECK_MESSAGE(CountOccurrences(folded.message, L"Co-authored-by:") == 1, Describe(folded));
  // 判定为同一位之後，位置保留、写法换成表单里的这一条（内部的两个空格也照表单原样，不修剪）。
  GC_CHECK_MESSAGE(folded.message.find(L"Li  Si <li.si@example.com>") != std::wstring::npos,
                   Describe(folded));
  GC_CHECK_MESSAGE(folded.message.find(L"LI.SI@Example.COM") == std::wstring::npos, Describe(folded));

  // 表單裡有兩位、描述裡已有其中一位：已有的位置不動，另一位追加在後面。
  const ComposedCommitMessage mixed = ComposeCommitMessage(
      Form(L"标题", L"正文\n\nCo-authored-by: 李四 <lisi@example.com>", kGoodAuthor,
           {L"王五 <wangwu@example.com>", L"李四 <lisi@example.com>"}));
  GC_CHECK_MESSAGE(mixed.message ==
                       L"标题\n\n正文\n\nCo-authored-by: 李四 <lisi@example.com>\n"
                       L"Co-authored-by: 王五 <wangwu@example.com>",
                   Describe(mixed));
}

GC_TEST(commit_message_keeps_other_trailers_in_order) {
  // trailer 段裡的其他鍵（Signed-off-by 等）與不在表單裡的 Co-authored-by 都要原樣留下、順序不變。
  const std::wstring description =
      L"正文\n\nSigned-off-by: Boss <boss@example.com>\nCo-authored-by: Ghost <ghost@example.com>";
  const ComposedCommitMessage composed =
      ComposeCommitMessage(Form(L"标题", description, kGoodAuthor, {L"李四 <lisi@example.com>"}));
  GC_CHECK_MESSAGE(composed.message ==
                       L"标题\n\n正文\n\nSigned-off-by: Boss <boss@example.com>\n"
                       L"Co-authored-by: Ghost <ghost@example.com>\n"
                       L"Co-authored-by: 李四 <lisi@example.com>",
                   Describe(composed));
  GC_CHECK_MESSAGE(composed.reusedTrailers == 0, "描述里没有表单中的任何一位，全部按追加处理") ;
  GC_CHECK_MESSAGE(composed.appendedTrailers == 1, Describe(composed));

  // 鍵的大小寫不同仍是同一個 trailer 鍵（Git 的 trailer 鍵比較不區分大小寫）。
  const ComposedCommitMessage lowered = ComposeCommitMessage(
      Form(L"标题", L"正文\n\nco-authored-by: 李四 <lisi@example.com>", kGoodAuthor,
           {L"李四 <lisi@example.com>"}));
  GC_CHECK_MESSAGE(CountOccurrences(lowered.message, L"uthored-by:") == 1, Describe(lowered));
  GC_CHECK_MESSAGE(lowered.message.find(L"co-authored-by:") == std::wstring::npos,
                   "沿用时统一写成标准拼写 Co-authored-by") ;
}

GC_TEST(commit_message_leaves_prose_alone) {
  // 情形一：最後一段是散文（有一行既不是「鍵: 值」也不是續行）→ 整段視為正文，一個字都不動。
  const ComposedCommitMessage lastIsProse = ComposeCommitMessage(
      Form(L"标题", L"Note: 这行像 trailer\n这行没有冒號，所以整段不算 trailer 段", kGoodAuthor, {}));
  GC_CHECK_MESSAGE(lastIsProse.bodyLines == 2, Describe(lastIsProse));
  GC_CHECK_MESSAGE(lastIsProse.message ==
                       L"标题\n\nNote: 这行像 trailer\n这行没有冒號，所以整段不算 trailer 段",
                   Describe(lastIsProse));

  // 情形二：散文行裡引用了别人的 Co-authored-by 寫法。即使這一行被當成 trailer 條目来看，
  // 它與表單裡的身份不是同一位，就必須原樣留下——本程序只做「同一位不重複」，
  // 絕不按全域字串替換去刪正文文字。
  const std::wstring quoted =
      L"先看这段。\n\n这段最后的「Co-authored-by: Ghost <ghost@example.com>」是引用，不是真的 trailer。";
  const ComposedCommitMessage composed =
      ComposeCommitMessage(Form(L"标题", quoted, kGoodAuthor, {L"李四 <lisi@example.com>"}));
  GC_CHECK_MESSAGE(composed.message.find(
                       L"这段最后的「Co-authored-by: Ghost <ghost@example.com>」是引用，不是真的 trailer。") !=
                       std::wstring::npos,
                   "正文里的引用必须一字不改");
  GC_CHECK_MESSAGE(CountOccurrences(composed.message, L"Co-authored-by:") == 2, Describe(composed));
  GC_CHECK_MESSAGE(composed.reusedTrailers == 0, Describe(composed));

  // 情形三：全形冒號不構成 trailer 鍵，中文敘述行不會被误判成「鍵: 值」。
  const ComposedCommitMessage fullwidth =
      ComposeCommitMessage(Form(L"标题", L"说明：这里用了全形冒號\n下一行也没有半形冒號", kGoodAuthor, {}));
  GC_CHECK_MESSAGE(fullwidth.message == L"标题\n\n说明：这里用了全形冒號\n下一行也没有半形冒號",
                   Describe(fullwidth));
  GC_CHECK_MESSAGE(fullwidth.bodyLines == 2, Describe(fullwidth));
}

GC_TEST(commit_message_trailer_only_description) {
  // 描述本身就只有 trailer 段：没有正文，trailer 紧跟在标题之后空一行。
  const ComposedCommitMessage composed = ComposeCommitMessage(
      Form(L"标题", L"Co-authored-by: 李四 <lisi@example.com>", kGoodAuthor,
           {L"李四 <lisi@example.com>"}));
  GC_CHECK_MESSAGE(composed.message == L"标题\n\nCo-authored-by: 李四 <lisi@example.com>",
                   Describe(composed));
  GC_CHECK_MESSAGE(composed.bodyLines == 0, Describe(composed));
}

GC_TEST(commit_message_continuation_line_is_preserved) {
  // 一条 trailer 的续行（以空白开头）必须整条原样保留，不能被拆成两条或被丢掉。
  const std::wstring description =
      L"正文\n\nNote: 这条有两行\n    续行内容\nSigned-off-by: Boss <boss@example.com>";
  const ComposedCommitMessage composed =
      ComposeCommitMessage(Form(L"标题", description, kGoodAuthor, {L"李四 <lisi@example.com>"}));
  GC_CHECK_MESSAGE(composed.message ==
                       L"标题\n\n正文\n\nNote: 这条有两行\n    续行内容\n"
                       L"Signed-off-by: Boss <boss@example.com>\n"
                       L"Co-authored-by: 李四 <lisi@example.com>",
                   Describe(composed));
  GC_CHECK_MESSAGE(composed.bodyLines == 1, "只有「正文」一行是正文，其余都属 trailer 段：" +
                                                Describe(composed));
}

GC_TEST(commit_message_paragraph_ending_with_continuation_is_prose) {
  // 最後一行是續行時，這段不視為 trailer 段（與 Git 自己解析 trailer 塊的條件一致）：
  // 整段當正文原樣保留，合作者另起一段追加在後面。
  const std::wstring description = L"正文\n\nNote: 这条有两行\n    续行内容";
  const ComposedCommitMessage composed =
      ComposeCommitMessage(Form(L"标题", description, kGoodAuthor, {L"李四 <lisi@example.com>"}));
  GC_CHECK_MESSAGE(composed.bodyLines == 4, Describe(composed));
  GC_CHECK_MESSAGE(composed.message ==
                       L"标题\n\n正文\n\nNote: 这条有两行\n    续行内容\n\n"
                       L"Co-authored-by: 李四 <lisi@example.com>",
                   Describe(composed));
}

GC_TEST(commit_message_collapses_duplicate_coauthors_in_form) {
  const ComposedCommitMessage composed = ComposeCommitMessage(
      Form(L"标题", L"", kGoodAuthor,
           {L"李四 <lisi@example.com>", L"  李四  <LISI@EXAMPLE.COM>", L"王五 <wangwu@example.com>"}));
  GC_CHECK_MESSAGE(composed.coauthorTrailers == 2, Describe(composed));
  GC_CHECK_MESSAGE(CountOccurrences(composed.message, L"Co-authored-by:") == 2, Describe(composed));
  GC_CHECK_MESSAGE(composed.message.find(L"李四 <lisi@example.com>") != std::wstring::npos,
                   "只保留第一次出现的写法");
}

GC_TEST(commit_message_reports_unparsable_coauthor_instead_of_inventing_one) {
  // 正常流程应先过校验；这里确认即使被直接呼叫，坏条目也只被如實報告，不憑空生成 trailer。
  const ComposedCommitMessage composed =
      ComposeCommitMessage(Form(L"标题", L"", kGoodAuthor, {L"没有尖括号 <>, 邮箱"}));
  GC_CHECK_MESSAGE(composed.rejectedCoauthors.size() == 1, Describe(composed));
  GC_CHECK_MESSAGE(composed.coauthorTrailers == 0, Describe(composed));
}

GC_TEST(commit_message_committer_identity_state_affects_validity) {
  // 提交者身份由有效 Git 配置决定：缺配置是拦路虎，还没读到只是「还不知道」，不算错误。
  const CommitFormValidity missing =
      ValidateCommitForm(Form(L"标题"), CommitterIdentityState::missing);
  GC_CHECK_MESSAGE(HasField(missing, CommitIssueField::committer), "提交者身份缺失时必须报问题");
  GC_CHECK_MESSAGE(!missing.Ok(), "提交者身份缺失不算校验通过");

  const CommitFormValidity unknown =
      ValidateCommitForm(Form(L"标题"), CommitterIdentityState::unknown);
  GC_CHECK_MESSAGE(unknown.Ok(), "还没读到提交者身份不该被当成错误");
  GC_CHECK_MESSAGE(unknown.StatusText().find(L"尚未读到") != std::wstring::npos,
                   Utf8(unknown.StatusText()));

  const CommitFormValidity badAuthor =
      ValidateCommitForm(Form(L"标题", L"", L"只有姓名没有邮箱"), CommitterIdentityState::available);
  GC_CHECK_MESSAGE(HasField(badAuthor, CommitIssueField::author), "作者格式不对要报问题");

  const CommitFormValidity badCoauthor = ValidateCommitForm(
      Form(L"标题", L"", kGoodAuthor, {L"张三 <z@e.com>", L"坏条目"}), CommitterIdentityState::available);
  GC_CHECK_MESSAGE(HasField(badCoauthor, CommitIssueField::coauthor), "合作者格式不对要报问题");
  GC_CHECK_MESSAGE(badCoauthor.StatusText().find(L"第 2 条合作者") != std::wstring::npos,
                   "原因里要指出是哪一条：" + Utf8(badCoauthor.StatusText()));
}

GC_TEST(commit_message_validity_status_text_counts) {
  const CommitFormValidity validity = ValidateCommitForm(
      Form(L"十二个字的中文标题", L"一行\n两行", kGoodAuthor, {L"李四 <lisi@example.com>"}),
      CommitterIdentityState::available);
  GC_CHECK_MESSAGE(validity.Ok(), Utf8(validity.StatusText()));
  GC_CHECK_MESSAGE(validity.subjectCharacters == 9, "标题字数按修剪后的字符计") ;
  GC_CHECK_MESSAGE(validity.descriptionLines == 2, "描述行数按去掉首尾空行后的行计") ;
  GC_CHECK_MESSAGE(validity.coauthorCount == 1, "合作者条数就是列表长度");
  GC_CHECK_MESSAGE(validity.StatusText().find(L"校验通过") != std::wstring::npos,
                   Utf8(validity.StatusText()));
}
