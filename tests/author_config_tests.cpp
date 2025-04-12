// 有效 Git 身份查询的判读测试：用桩回答（GitQueryResult）驱动生产判读逻辑，不起真实 Git。
// 覆盖：参数形态（只读、显式 -C、带 --no-optional-locks）、--null 输出的取值与格式违例、
// 「没设过」与「查不到」的分别、身份拼装时的修剪、提交者身份三种状态与面向界面的说明文案。
// 真实 Git 的优先级行为在 author_config_fixture_tests.cpp 里用临时仓库验证。
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "git/author_config.h"
#include "git/repository.h"
#include "support/tiny_test.h"

namespace {

using gc::git::AuthorIdentityConfig;
using gc::git::CommitterIdentityState;
using gc::git::ConfigValueRead;
using gc::git::ConfigValueState;
using gc::git::GitQueryResult;
using gc::git::RepoError;

GitQueryResult Answer(int exitCode, std::wstring_view output = {}, std::wstring_view error = {}) {
  GitQueryResult result;
  result.started = true;
  result.exited = true;
  result.exitCode = exitCode;
  result.utf16Output = std::wstring(output);
  result.utf16Error = std::wstring(error);
  return result;
}

ConfigValueRead Read(std::wstring_view key, const GitQueryResult& result) {
  return gc::git::ParseIdentityConfigQuery(result, key);
}

ConfigValueRead Present(std::wstring_view value) { return Read(L"user.name", Answer(0, value)); }

// --null 的輸出是「值 + NUL」。字面量裡的 \0 會被 wstring_view 的指標構造當成結尾，
// 長度根本吃不到它，因此帶 NUL 的樣本必須顯式拼出來。
std::wstring NulTerminated(std::wstring_view value) {
  std::wstring text(value);
  text.push_back(L'\0');
  return text;
}

ConfigValueRead Absent() { return Read(L"user.email", Answer(1)); }

std::string Utf8(const std::wstring& text) {
  std::string out;
  for (const wchar_t value : text) {
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

bool Contains(const std::vector<std::wstring>& arguments, std::wstring_view value) {
  for (const std::wstring& argument : arguments) {
    if (argument == value) {
      return true;
    }
  }
  return false;
}

}  // namespace

GC_TEST(author_config_arguments_are_read_only_and_bound_to_the_repository) {
  const std::vector<std::wstring> arguments =
      gc::git::BuildIdentityConfigArguments(L"P:\\tmp\\repo", L"user.name");
  GC_CHECK_MESSAGE(Contains(arguments, L"-C"), "必须显式带 -C，不依赖进程全局当前目录");
  GC_CHECK_MESSAGE(Contains(arguments, L"P:\\tmp\\repo"), "-C 后面要跟仓库工作区根");
  GC_CHECK_MESSAGE(Contains(arguments, L"--no-optional-locks"), "读配置不该顺手写任何锁文件");
  GC_CHECK_MESSAGE(Contains(arguments, L"--null"), "取值要用 --null，避免与值里的空白纠缠");
  GC_CHECK_MESSAGE(Contains(arguments, L"--get"), "只用 --get：读，不写");
  GC_CHECK_MESSAGE(Contains(arguments, L"user.name"), "要查的 key 原样出现在参数里");
  // 反向底线：读身份的查询里绝不能出现任何写入形态。
  GC_CHECK_MESSAGE(!Contains(arguments, L"--add") && !Contains(arguments, L"--replace-all") &&
                       !Contains(arguments, L"set") && !Contains(arguments, L"unset"),
                   "身份查询必须是只读的");
  // 2.56 实测「一次问两个 key」不成立（第二个参数被当成值样板），因此两个 key 分两次问。
  GC_CHECK_MESSAGE(!Contains(arguments, L"user.email"), "一条查询只问一个 key");
}

GC_TEST(author_config_reads_value_after_nul) {
  const ConfigValueRead ok = Present(NulTerminated(L"张三"));
  GC_CHECK(ok.state == ConfigValueState::present);
  GC_CHECK_MESSAGE(ok.value == L"张三", "值按 NUL 之前的部分整段取出：" + Utf8(ok.value));

  const ConfigValueRead spaces = Present(NulTerminated(L"  Spa ced  "));
  GC_CHECK(spaces.state == ConfigValueState::present);
  GC_CHECK_MESSAGE(spaces.value == L"  Spa ced  ", "头尾空白是值的一部分，判读阶段不修剪");

  const ConfigValueRead emptyValue = Present(NulTerminated(L""));
  GC_CHECK_MESSAGE(emptyValue.state == ConfigValueState::present && emptyValue.value.empty(),
                   "设过但为空值仍算 present：界面要区分“没设”与“设成空”");

  // 形态不合约定就整份拒绝，不採用「看起来像」的一半。
  const ConfigValueRead noNul = Present(L"张三");
  GC_CHECK_MESSAGE(noNul.state == ConfigValueState::failed && noNul.error == RepoError::badOutput,
                   "缺少结尾 NUL 要报格式违例");
  const ConfigValueRead twoSegments =
      Read(L"user.name", Answer(0, NulTerminated(L"甲") + NulTerminated(L"乙")));
  GC_CHECK_MESSAGE(twoSegments.state == ConfigValueState::failed &&
                       twoSegments.error == RepoError::badOutput,
                   "多出一段输出同样要报格式违例");
}

GC_TEST(author_config_distinguishes_absent_from_failure) {
  const ConfigValueRead absent = Absent();
  GC_CHECK_MESSAGE(absent.state == ConfigValueState::absent,
                   "退出码 1 是“这个 key 没设过”，不是失败");

  const ConfigValueRead notStarted = Read(L"user.name", [] {
    GitQueryResult result;
    result.started = false;
    return result;
  }());
  GC_CHECK_MESSAGE(notStarted.state == ConfigValueState::failed &&
                       notStarted.error == RepoError::gitLaunchFailed,
                   "进程没起来要报启动失败");

  const ConfigValueRead timedOut = Read(L"user.name", [] {
    GitQueryResult result;
    result.started = true;
    result.timedOut = true;
    return result;
  }());
  GC_CHECK_MESSAGE(timedOut.state == ConfigValueState::failed && timedOut.error == RepoError::gitTimeout,
                   "超时不能被当成“没设过”");

  const ConfigValueRead broken =
      Read(L"user.name", Answer(128, L"", L"fatal: bad config line 1 in file .git/config"));
  GC_CHECK_MESSAGE(broken.state == ConfigValueState::failed, "配置档损坏必须报失败");
  GC_CHECK_MESSAGE(broken.detail.find(L"bad config line") != std::wstring::npos,
                   "失败原因要把 Git 原话带回去：" + Utf8(broken.detail));
  GC_CHECK_MESSAGE(broken.detail.find(L"user.name") != std::wstring::npos,
                   "原因里要说明查的是哪个 key：" + Utf8(broken.detail));
}

GC_TEST(author_config_committer_state_and_identity) {
  const AuthorIdentityConfig complete = gc::git::CombineIdentityConfig(
      Present(NulTerminated(L"张三")), Read(L"user.email", Answer(0, NulTerminated(L"z@e.com"))));
  GC_CHECK_MESSAGE(complete.CommitterState() == CommitterIdentityState::available,
                   "两项都设过才算身份可用");
  GC_CHECK_MESSAGE(complete.Identity() == L"张三 <z@e.com>",
                   "身份要拼成「姓名 <邮箱>」：" + Utf8(complete.Identity()));
  GC_CHECK_MESSAGE(!complete.ReadFailed(), "完整读取不算失败");
  GC_CHECK_MESSAGE(gc::git::BuildIdentityConfigNotice(complete).empty(),
                   "读到完整身份时不需要额外提示");

  // 配置值本身带空白时，界面显示的身份取修剪后的写法（实测 Git 用作身份时也是这么处理的）。
  const AuthorIdentityConfig padded = gc::git::CombineIdentityConfig(
      Present(NulTerminated(L"  Spa ced  ")),
      Read(L"user.email", Answer(0, NulTerminated(L"  a@b.com  "))));
  GC_CHECK_MESSAGE(padded.Identity() == L"Spa ced <a@b.com>",
                   "外围空白不该留在显示的身份里：" + Utf8(padded.Identity()));

  const AuthorIdentityConfig nameOnly =
      gc::git::CombineIdentityConfig(Present(NulTerminated(L"张三")), Absent());
  GC_CHECK_MESSAGE(nameOnly.CommitterState() == CommitterIdentityState::missing,
                   "只有 user.name 不算可用");
  GC_CHECK_MESSAGE(nameOnly.Identity().empty(), "凑不出完整身份时不给默认文字");
  GC_CHECK_MESSAGE(gc::git::BuildIdentityConfigNotice(nameOnly).find(L"user.email") !=
                       std::wstring::npos,
                   "提示要指出缺哪一项：" + Utf8(gc::git::BuildIdentityConfigNotice(nameOnly)));

  const AuthorIdentityConfig blankValue = gc::git::CombineIdentityConfig(
      Present(NulTerminated(L"")), Read(L"user.email", Answer(0, NulTerminated(L"z@e.com"))));
  GC_CHECK_MESSAGE(blankValue.CommitterState() == CommitterIdentityState::missing,
                   "设成空值与没设过一样，都凑不出身份");

  const AuthorIdentityConfig bothAbsent = gc::git::CombineIdentityConfig(Absent(), Absent());
  GC_CHECK_MESSAGE(bothAbsent.CommitterState() == CommitterIdentityState::missing,
                   "两项都没设过：明确缺失，而不是“还不知道”");
  GC_CHECK_MESSAGE(!gc::git::BuildIdentityConfigNotice(bothAbsent).empty(),
                   "缺身份时要给出「请自己填写」的说明");
  GC_CHECK_MESSAGE(
      gc::git::BuildIdentityConfigNotice(bothAbsent).find(L"不会替你改动 Git 配置") != std::wstring::npos,
      "说明里要讲清本程序不写配置");

  const ConfigValueRead broken = Read(L"user.name", Answer(128, L"", L"fatal: bad config line 2"));
  const AuthorIdentityConfig failed = gc::git::CombineIdentityConfig(broken, broken);
  GC_CHECK_MESSAGE(failed.CommitterState() == CommitterIdentityState::unknown,
                   "查询失败时不能断言提交者身份可用或不可用");
  GC_CHECK_MESSAGE(failed.ReadFailed(), "查询失败要被界面看见");
  GC_CHECK_MESSAGE(!gc::git::BuildIdentityConfigNotice(failed).empty(), "失败也要有面向界面的说明");
}
