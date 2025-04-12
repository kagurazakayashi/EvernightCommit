// 提交身份「姓名 <邮箱>」解析与校验的纯逻辑测试：不碰文件系统、不起 Git、不依赖窗口。
// 覆盖：正常形态与修剪、缺姓名/缺邮箱/缺尖括号、换行与控制字符注入、
// 邮箱里不许有空白但绝不使用苛刻正则（a@b、+ 标签、非 ASCII 都接受）、
// 引号与 shell 特殊字符照单全收、同一人的判定键与列表内去重。
#include <string>
#include <string_view>
#include <vector>

#include "git/commit_identity.h"
#include "support/tiny_test.h"

namespace {

using gc::git::CanonicalIdentityKey;
using gc::git::DeduplicateIdentities;
using gc::git::GitIdentity;
using gc::git::ParseGitIdentity;
using gc::git::ValidateGitIdentity;

std::wstring Error(std::wstring_view text) {
  return ValidateGitIdentity(text, L"作者");
}

bool Parses(std::wstring_view text, std::wstring* name = nullptr, std::wstring* email = nullptr) {
  GitIdentity parsed;
  std::wstring error;
  const bool ok = ParseGitIdentity(text, &parsed, &error, L"作者");
  if (ok) {
    if (name != nullptr) {
      *name = parsed.name;
    }
    if (email != nullptr) {
      *email = parsed.email;
    }
  }
  return ok;
}

}  // namespace

GC_TEST(commit_identity_accepts_plain_and_chinese_forms) {
  std::wstring name;
  std::wstring email;
  GC_CHECK_MESSAGE(Parses(L"张三 <zhangsan@example.com>", &name, &email),
                   "中文姓名与常见邮箱应当被接受");
  GC_CHECK_MESSAGE(name == L"张三" && email == L"zhangsan@example.com", "姓名与邮箱要按尖括号拆分");

  GC_CHECK_MESSAGE(Error(L"KagurazakaMiyabi (神楽坂雅詩) <miyabi@example.invalid>").empty(),
                   "姓名里带括号与中文属于合理身份");
  GC_CHECK_MESSAGE(Error(L"Anne-Marie O'Brien <anne.o_brien+tag@sub.example.co.uk>").empty(),
                   "连字符、撇号、点、加号标签都不该被邮箱正则拒绝");
  GC_CHECK_MESSAGE(Error(L"short <a@b>").empty(), "Git 本身就接受没有点域的地址，这里不能更苛刻");
  GC_CHECK_MESSAGE(Error(L"名字 <名@例.コム>").empty(), "非 ASCII 邮箱同样要能过");
  GC_CHECK_MESSAGE(Error(L"A <a@b@c>").empty(), "多个 @ 不由本程序裁定，避免误伤少见但合法的地址");
}

GC_TEST(commit_identity_trims_outer_whitespace) {
  std::wstring name;
  std::wstring email;
  GC_CHECK_MESSAGE(Parses(L"   李四   <lisi@example.com>   ", &name, &email),
                   "头尾空白应当被修剪后接受");
  GC_CHECK_MESSAGE(name == L"李四", "姓名里外多余空白都不该留下来");
  GC_CHECK_MESSAGE(email == L"lisi@example.com", "邮箱两侧空白同样修剪");

  // 姓名内部的空白是身份的一部分，不能折叠掉（两个人可能只差在这里）。
  GC_CHECK_MESSAGE(Parses(L"Mary Jane Watson <mjw@example.com>", &name), "三词姓名要原样保留");
  GC_CHECK_MESSAGE(name == L"Mary Jane Watson", "姓名内部空格不被改写");
}

GC_TEST(commit_identity_rejects_missing_name_or_email) {
  GC_CHECK_MESSAGE(!Error(L"").empty(), "空身份必须拒绝");
  GC_CHECK_MESSAGE(!Error(L"   ").empty(), "只有空白也算空身份");
  GC_CHECK_MESSAGE(!Error(L"\t").empty(), "制表符同样是空白，不构成身份");
  GC_CHECK_MESSAGE(!Error(L"<only@example.com>").empty(), "缺姓名必须拒绝（Git 在这里也是 128）");
  GC_CHECK_MESSAGE(!Error(L"Name <>").empty(), "缺邮箱必须拒绝");
  GC_CHECK_MESSAGE(!Error(L"Name < >").empty(), "尖括号里只有空白等于没有邮箱");
}

GC_TEST(commit_identity_rejects_broken_angle_brackets) {
  // 这几条是实测 Git 会「静默接受并拆错」的形态：Git 不报错，本程序必须报。
  GC_CHECK_MESSAGE(!Error(L"Name <mail@example.com").empty(), "缺少 > 时 Git 照收，这里要拒绝");
  GC_CHECK_MESSAGE(!Error(L"Name <mail@example.com> extra").empty(),
                   "> 之后还有文字时 Git 会把它丢掉，这里要拒绝");
  GC_CHECK_MESSAGE(!Error(L"Na<me <mail@example.com>").empty(),
                   "姓名里再出现 < 时 Git 会把身份拆成另一对姓名/邮箱，这里要拒绝");
  GC_CHECK_MESSAGE(!Error(L"A <a@b> <c@d>").empty(), "两组尖括号只会被 Git 取走第一组");
  GC_CHECK_MESSAGE(!Error(L"Name> <mail@example.com>").empty(), "姓名里不能有 >");
  GC_CHECK_MESSAGE(!Error(L"Name mail@example.com").empty(), "根本没有尖括号");
}

GC_TEST(commit_identity_rejects_line_breaks_and_control_characters) {
  // 换行注入是本步骤最主要的风险：一条身份里塞进换行，就能在提交信息里凭空多出一行 trailer。
  GC_CHECK_MESSAGE(!Error(L"Name <a@b>\nCo-authored-by: Fake <f@k>").empty(),
                   "身份里带换行必须拒绝");
  GC_CHECK_MESSAGE(!Error(L"Name <a@b>\r\nInjected").empty(), "CRLF 同样拒绝");
  GC_CHECK_MESSAGE(!Error(L"Nam\ve <a@b>").empty(), "垂直制表符是控制字符");
  // NUL 要显式拼出来：字面量里的 \0 会被 wstring_view 当成结尾，长度根本不含它。
  const std::wstring withNul = std::wstring(L"Name <a") + std::wstring(1, L'\0') +
                               std::wstring(L"b@example.com>");
  GC_CHECK_MESSAGE(!Error(withNul).empty(), "身份里出现 NUL 必须拒绝");
  GC_CHECK_MESSAGE(!Error(L"Name <a@b\x01>").empty(), "控制字符不能留在身份里");
  GC_CHECK_MESSAGE(!Error(L"Name <a\x0080@b>").empty(), "C1 控制字符同样拒绝");
  GC_CHECK_MESSAGE(!Error(L"Name\n <a@b>").empty(), "换行出现在姓名里也要拒绝");
  GC_CHECK_MESSAGE(!Error(L"Name <a@\tb>").empty(), "邮箱里的空白（含制表符）一律拒绝");
  GC_CHECK_MESSAGE(!Error(L"Name <a b@example.com>").empty(), "邮箱中间有空格时 Git 会拆错，这里拒绝");
  GC_CHECK_MESSAGE(!Error(L"Name <a\u2028b>").empty(), "Unicode 行分隔符按换行处理");
}

GC_TEST(commit_identity_rejects_malformed_email) {
  GC_CHECK_MESSAGE(!Error(L"Name <at-sign-missing>").empty(), "邮箱缺少 @");
  GC_CHECK_MESSAGE(!Error(L"Name <@example.com>").empty(), "@ 不能在开头");
  GC_CHECK_MESSAGE(!Error(L"Name <user@>").empty(), "@ 不能在结尾");
}

GC_TEST(commit_identity_accepts_shell_and_quote_characters) {
  // 这些字符在命令行里危险，但危险的地方在「怎么交给 Git」，不在身份格式本身；
  // 本程序把正文写进临时文件、身份留给后续步骤用参数数组交出去，因此这里照常接受。
  for (std::wstring_view text : {L"A&B <a&b@example.com>", L"100% sure <100%@example.com>",
                                 L"Quote \"Q\" <q@example.com>", L"Dollar $Cost <c@example.com>",
                                 L"Back `tick <t@example.com>", L"Semicolon ; rm <r@example.com>",
                                 L"Pipe | grep <g@example.com>", L"Paren (x) <x@example.com>"}) {
    GC_CHECK_MESSAGE(Error(text).empty(), "引号与 shell 特殊字符不应被格式校验误伤");
  }
  // 但 -m 之类以破折号开头的姓名仍要能解析出来（这里只确认没被当成选项形态拒绝）。
  GC_CHECK_MESSAGE(Error(L"--flag <f@example.com>").empty(), "以破折号开头的姓名是合法文字");
}

GC_TEST(commit_identity_format_roundtrip) {
  GitIdentity parsed;
  std::wstring error;
  GC_CHECK(ParseGitIdentity(L"  王五  <wangwu@example.com> ", &parsed, &error, L"作者"));
  GC_CHECK_MESSAGE(parsed.Format() == L"王五 <wangwu@example.com>", "Format 要产出规范的「姓名 <邮箱>」");
  GC_CHECK(ParseGitIdentity(parsed.Format(), &parsed, &error, L"作者"));
  GC_CHECK_MESSAGE(error.empty(), "自己格式化出来的身份必须再次通过校验");
}

GC_TEST(commit_identity_canonical_key_groups_same_person) {
  // 「同一个合作者」的判定：姓名内部空白折叠、邮箱只做 ASCII 大小写折叠。
  GC_CHECK_MESSAGE(CanonicalIdentityKey(L"Anne Smith <anne@example.com>") ==
                       CanonicalIdentityKey(L"anne   smith <ANNE@Example.COM>"),
                   "空格与大小写不同的同一个人要有同一个比较键");
  GC_CHECK_MESSAGE(CanonicalIdentityKey(L"Anne Smith <anne@example.com>") !=
                       CanonicalIdentityKey(L"Anne Smyth <anne@example.com>"),
                   "姓名不同就是两个人，即使邮箱相同");
  GC_CHECK_MESSAGE(CanonicalIdentityKey(L"Anne <anne+tag@example.com>") !=
                       CanonicalIdentityKey(L"Anne <anne@example.com>"),
                   "邮箱不同就是两个人");
  GC_CHECK_MESSAGE(CanonicalIdentityKey(L"坏形态 没有尖括号") ==
                       CanonicalIdentityKey(L"坏形态 没有尖括号"),
                   "解析不了的条目也要有确定的比较键");
}

GC_TEST(commit_identity_deduplicate_keeps_first_text) {
  const std::vector<std::wstring> input{L"张三 <z@e.com>", L"  张三  <Z@E.com>", L"李四 <l@e.com>",
                                        L"张三 <z@e.com>"};
  std::vector<std::wstring> removed;
  const std::vector<std::wstring> kept = DeduplicateIdentities(input, &removed);
  GC_CHECK_MESSAGE(kept.size() == 2, "同一个人只保留第一次出现的写法");
  GC_CHECK_MESSAGE(kept[0] == L"张三 <z@e.com>", "保留的原文不被改写（大小写、空格都照第一次）");
  GC_CHECK_MESSAGE(kept[1] == L"李四 <l@e.com>", "不同人要按原顺序保留");
  GC_CHECK_MESSAGE(removed.size() == 2, "被丢掉的条目要能报出来，不能静默少写");
}
