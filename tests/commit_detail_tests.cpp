// 「提交详情」的纯逻辑测试：字段切分约定、答非所问时的作废、描述过长时的截断，
// 以及面板与剪贴板共用那份文本的排版。断言的是解析出来的字段与最终文本，不是命令文本
// （参数形态由 commit_detail_fixture_tests.cpp 交给真实 Git 核验）。
#include "support/tiny_test.h"

#include <algorithm>  // std::find：此前只靠标准库的间接包含侥幸编过，GCC 下直接报找不到
#include <string>
#include <vector>

#include "git/commit_detail.h"

namespace {

using gc::git::CommitDetail;
using gc::git::CommitDetailParseResult;

// 桩时间换算：把秒数原样写成可断言的文本，纯逻辑用例不依赖任何时区规则。
const gc::git::CommitTimeFormatter kStubTime = [](long long epochSeconds) {
  return L"T" + std::to_wstring(epochSeconds);
};

constexpr wchar_t kSep = L'\0';

std::wstring Record(std::wstring_view objectId, std::wstring_view author, std::wstring_view email,
                    std::wstring_view authorEpoch, std::wstring_view committerEpoch,
                    std::wstring_view summary, std::wstring_view description) {
  std::wstring out;
  out.append(objectId);
  out.push_back(kSep);
  out.append(author);
  out.push_back(kSep);
  out.append(email);
  out.push_back(kSep);
  out.append(authorEpoch);
  out.push_back(kSep);
  out.append(committerEpoch);
  out.push_back(kSep);
  out.append(summary);
  out.push_back(kSep);
  out.append(description);
  out.push_back(kSep);  // `-z` 让这条记录以 NUL 结束
  return out;
}

const wchar_t* kOid = L"9f2c1a7b4e5d6c8b9a0f1e2d3c4b5a6978899a0b";
const wchar_t* kOtherOid = L"1111111111111111111111111111111111111111";

bool Has(std::wstring_view haystack, std::wstring_view needle) {
  return haystack.find(needle) != std::wstring_view::npos;
}

std::string Narrow(std::wstring_view text) {
  std::string out;
  out.reserve(text.size());
  for (const wchar_t c : text) {
    out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  return out;
}

std::wstring DescribeResult(const CommitDetailParseResult& result) {
  return L"ok=" + std::to_wstring(result.ok ? 1 : 0) + L" error=[" + result.error + L"]";
}

}  // namespace

GC_TEST(commit_detail_parses_the_seven_field_record) {
  // 实测形态：Git 给 %b 末尾自带一个 LF，展示前要去掉，内部换行原样保留。
  const std::wstring output =
      Record(kOid, L"神楽坂雅詩", L"kagurazaka@example.com", L"1748349225", L"1748349900",
             L"列表改成两页", L"第一行\n第二行\n\n末段\n");
  const CommitDetailParseResult parsed =
      gc::git::ParseCommitDetail(output, kOid, kStubTime);
  GC_REQUIRE_MESSAGE(parsed.ok, Narrow(DescribeResult(parsed)));
  GC_CHECK(parsed.detail.objectId == kOid);
  GC_CHECK(parsed.detail.authorName == L"神楽坂雅詩");
  GC_CHECK(parsed.detail.authorEmail == L"kagurazaka@example.com");
  GC_CHECK(parsed.detail.summary == L"列表改成两页");
  GC_CHECK(parsed.detail.authoredAt == L"T1748349225");
  GC_CHECK(parsed.detail.committedAt == L"T1748349900");
  GC_CHECK(parsed.detail.description == L"第一行\n第二行\n\n末段");
  GC_CHECK(!parsed.detail.descriptionTruncated);
}

GC_TEST(commit_detail_accepts_empty_author_and_summary_and_description) {
  // --allow-empty-message 与只有标题的提交都是 Git 允许的真实形态：空是事实，不是读坏了。
  const std::wstring output = Record(kOid, L"", L"", L"1748349225", L"1748349225", L"", L"");
  const CommitDetailParseResult parsed = gc::git::ParseCommitDetail(output, kOid, kStubTime);
  GC_REQUIRE_MESSAGE(parsed.ok, Narrow(DescribeResult(parsed)));
  GC_CHECK(parsed.detail.authorName.empty());
  GC_CHECK(parsed.detail.summary.empty());
  GC_CHECK(parsed.detail.description.empty());

  const std::wstring text = gc::git::FormatCommitDetailText(parsed.detail);
  GC_CHECK(Has(text, L"标题：（这条提交没有标题）"));
  GC_CHECK(Has(text, L"作者：（没有作者信息）"));
  GC_CHECK(Has(text, L"描述：（无描述）"));
}

GC_TEST(commit_detail_refuses_when_answered_object_is_not_the_requested_one) {
  // 行在读取期间被刷新换成别的提交：答的那枚与问的那枚不是同一枚，整份作废。
  const std::wstring output =
      Record(kOtherOid, L"A", L"a@example.com", L"1748349225", L"1748349225", L"标题", L"");
  const CommitDetailParseResult parsed = gc::git::ParseCommitDetail(output, kOid, kStubTime);
  GC_CHECK(!parsed.ok);
  GC_CHECK(parsed.detail.objectId.empty());
  GC_CHECK(Has(parsed.error, L"不是同一枚提交"));
}

GC_TEST(commit_detail_refuses_wrong_field_count_and_empty_output) {
  const std::wstring shortRecord =
      std::wstring(kOid) + kSep + L"A" + kSep + L"a@example.com" + kSep + L"1748349225" + kSep +
      L"1748349225" + kSep + L"标题" + kSep;  // 少一段描述
  const CommitDetailParseResult tooFew =
      gc::git::ParseCommitDetail(shortRecord, kOid, kStubTime);
  GC_CHECK(!tooFew.ok);
  GC_CHECK(Has(tooFew.error, L"字段数不是 7"));

  // 两段（读了两条记录）同样不合约定：这里只该有 `-n 1` 的那一条。
  const std::wstring twoRecords =
      Record(kOid, L"A", L"a@example.com", L"1", L"1", L"甲", L"") +
      Record(kOtherOid, L"B", L"b@example.com", L"2", L"2", L"乙", L"");
  const CommitDetailParseResult tooMany =
      gc::git::ParseCommitDetail(twoRecords, kOid, kStubTime);
  GC_CHECK(!tooMany.ok);
  GC_CHECK(Has(tooMany.error, L"字段数不是 7"));

  const CommitDetailParseResult nothing = gc::git::ParseCommitDetail(L"", kOid, kStubTime);
  GC_CHECK(!nothing.ok);
  GC_CHECK(Has(nothing.error, L"输出为空"));
}

GC_TEST(commit_detail_refuses_record_without_terminating_nul) {
  // 缺结尾 NUL 的半条记录（描述读到一半断掉）不能当成「这条提交就是这样」。
  const std::wstring truncated =
      std::wstring(kOid) + kSep + L"A" + kSep + L"a@example.com" + kSep + L"1748349225" + kSep +
      L"1748349225" + kSep + L"标题" + kSep + L"描述写到一半";
  const CommitDetailParseResult parsed = gc::git::ParseCommitDetail(truncated, kOid, kStubTime);
  GC_CHECK(!parsed.ok);
  GC_CHECK(Has(parsed.error, L"记录不完整"));
}

GC_TEST(commit_detail_refuses_unusable_time_and_identity_fields) {
  const std::wstring badTime =
      Record(kOid, L"A", L"a@example.com", L"不是秒数", L"1748349225", L"标题", L"");
  const CommitDetailParseResult timeRejected =
      gc::git::ParseCommitDetail(badTime, kOid, kStubTime);
  GC_CHECK(!timeRejected.ok);
  GC_CHECK(Has(timeRejected.error, L"作者时间字段"));

  const std::wstring badId =
      Record(L"\"; rm -rf /", L"A", L"a@example.com", L"1", L"1", L"标题", L"");
  const CommitDetailParseResult idRejected =
      gc::git::ParseCommitDetail(badId, kOid, kStubTime);
  GC_CHECK(!idRejected.ok);
  GC_CHECK(Has(idRejected.error, L"对象 ID 不合约定"));

  // 界面递来的那一条本身就不合约定：在解析之前先拒，绝不带着它去比对。
  const CommitDetailParseResult requestRejected =
      gc::git::ParseCommitDetail(badTime, L"", kStubTime);
  GC_CHECK(!requestRejected.ok);
  GC_CHECK(Has(requestRejected.error, L"发起读取时那条提交的对象 ID"));
}

GC_TEST(commit_detail_caps_oversized_description_without_splitting_a_surrogate_pair) {
  // 截点落在代理对中间会留下孤立的半字符：编辑框与剪贴板都会拿到非法的 UTF-16。
  std::wstring description;
  description.append(gc::git::kCommitDetailDescriptionLimit - 1, L'a');
  description.append(L"\U0001F600");  // 一枚需要两个 UTF-16 单元的字符
  description.push_back(L'\n');
  const std::wstring output =
      Record(kOid, L"A", L"a@example.com", L"1748349225", L"1748349225", L"标题", description);
  const CommitDetailParseResult parsed = gc::git::ParseCommitDetail(output, kOid, kStubTime);
  GC_REQUIRE_MESSAGE(parsed.ok, Narrow(DescribeResult(parsed)));
  GC_CHECK(parsed.detail.descriptionTruncated);
  GC_CHECK(parsed.detail.description.size() < gc::git::kCommitDetailDescriptionLimit);
  const wchar_t last = parsed.detail.description.back();
  GC_CHECK(!(last >= 0xD800 && last <= 0xDBFF));

  // 截断这件事必须写在文本里：面板与剪贴板都不能假装这就是完整描述。
  const std::wstring text = gc::git::FormatCommitDetailText(parsed.detail);
  GC_CHECK(Has(text, L"以上只是开头那一段"));
}

GC_TEST(commit_detail_text_has_one_line_per_field_and_crlf_only) {
  CommitDetail detail;
  detail.objectId = kOid;
  detail.authorName = L"神楽坂雅詩";
  detail.authorEmail = L"kagurazaka@example.com";
  detail.authoredAt = L"2026-10-12 18:03:22";
  detail.committedAt = L"2026-10-13 09:10:01";
  detail.summary = L"列表改成两页";
  detail.description = L"第一行\r\n第二行";  // 已经是 CRLF 的那一份不能被写成两个行结束

  const std::wstring text = gc::git::FormatCommitDetailText(detail);
  GC_CHECK(Has(text, L"提交哈希：" + std::wstring(kOid)));
  GC_CHECK(Has(text, L"标题：列表改成两页"));
  GC_CHECK(Has(text, L"作者：神楽坂雅詩 <kagurazaka@example.com>"));
  GC_CHECK(Has(text, L"作者时间：2026-10-12 18:03:22"));
  GC_CHECK(Has(text, L"提交者时间：2026-10-13 09:10:01"));
  GC_CHECK(text.size() >= 2 && text.compare(text.size() - 2, 2, L"\r\n") != 0);
  // 整段里不许出现任何「单个 LF」：编辑框与剪贴板都要 CRLF。
  for (size_t index = 0; index < text.size(); ++index) {
    if (text[index] == L'\n') {
      GC_CHECK_MESSAGE(index > 0 && text[index - 1] == L'\r',
                        "第 " + std::to_string(index) + " 个字符是孤立的 LF");
    }
    GC_CHECK_MESSAGE(text[index] != kSep, "详情文本里不许出现 NUL");
  }
  GC_CHECK(Has(text, L"描述：\r\n第一行\r\n第二行"));
}

GC_TEST(commit_detail_normalizes_every_line_ending_shape) {
  // LF / CRLF / CR 三种形态都换行，且不产生重复的行结束；NUL 之外的字符原样保留。
  GC_CHECK(gc::git::NormalizeTextForEdit(L"甲\n乙\r\n丙\r丁") == L"甲\r\n乙\r\n丙\r\n丁");
  GC_CHECK(gc::git::NormalizeTextForEdit(L"") == L"");
  GC_CHECK(gc::git::NormalizeTextForEdit(L"只有一行") == L"只有一行");
}

GC_TEST(commit_detail_arguments_bind_the_verified_object_id_last) {
  const std::vector<std::wstring> arguments =
      gc::git::BuildCommitDetailArguments(L"P:\\repo 目录", kOid);
  GC_REQUIRE(!arguments.empty(), "参数数组为空");
  GC_CHECK(arguments.front() == L"-C");
  GC_CHECK(arguments[1] == L"P:\\repo 目录");
  GC_CHECK(arguments.back() == kOid);  // 对象 ID 在最后、不套 --（套了会被当成路径限定）
  GC_CHECK(std::find(arguments.begin(), arguments.end(), L"--") == arguments.end());

  const auto contains = [&arguments](std::wstring_view needle) {
    for (const std::wstring& argument : arguments) {
      if (argument.find(needle) != std::wstring::npos) {
        return true;
      }
    }
    return false;
  };
  GC_CHECK(contains(L"log"));
  GC_CHECK(contains(L"-z"));
  GC_CHECK(contains(L"-n"));
  GC_CHECK(contains(L"1"));
  GC_CHECK(contains(L"--format=%H%x00%an%x00%ae%x00%at%x00%ct%x00%s%x00%b"));
  GC_CHECK(contains(L"log.showSignature=false"));
  GC_CHECK(contains(L"--no-optional-locks"));
}
