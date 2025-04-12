// 用真实临时仓库验证「作者默认身份取自有效 Git 配置」这条链路：驱动的是生产的
// platform::LoadAuthorIdentity（内含两次 git config --null --get），断言的是读到的身份与归类，
// 不是命令文本。覆盖：只有仓库本體配置、只有使用者配置、両者都有時仓库本体优先、
// 姓名与邮箱分属两层、include 递回包含、设成空值、两项都没设、配置档损坏，
// 以及「读身份绝不改写仓库」这条底线。
//
// 夹具把 GIT_CONFIG_GLOBAL 指向临时档案、GIT_CONFIG_NOSYSTEM=1，因此这里看到的
// 「使用者層」与「系统层」都只属于本次测试，绝不会读到本机真实配置，也不会写回任何一处。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "git/author_config.h"
#include "git/repository.h"
#include "platform/windows/author_config.h"
#include "platform/windows/utf_text.h"
#include "support/git_fixture.h"
#include "support/tiny_test.h"

namespace {

using gc::git::AuthorIdentityConfig;
using gc::git::CommitterIdentityState;
using gc::git::ConfigValueState;
using gc::git::RepoError;
using gc::platform::AuthorConfigRequest;
using gc::test::GitFixture;

void Prepare(GitFixture& fixture) {
  std::string reason;
  const bool prepared = fixture.Prepare(reason);
  GC_REQUIRE(prepared, reason);
  fixture.InitRepository();
}

AuthorIdentityConfig Load(GitFixture& fixture) {
  AuthorConfigRequest request;
  request.exePath = fixture.GitExe();
  request.repositoryDirectory = fixture.RepoDir();
  request.timeoutMilliseconds = 20000;
  // 直接走生产路径：夹具只提供隔离执行器，不参与任何判读。
  return gc::platform::LoadAuthorIdentity(request, fixture.MakeAuthorConfigDepsForTest());
}

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

std::wstring Utf16(std::string_view text) { return gc::platform::Utf8ToUtf16(std::string(text)); }

void SetLocal(GitFixture& fixture, std::wstring_view key, std::wstring_view value) {
  fixture.RunCheckedInRepo({L"config", L"--local", std::wstring(key), std::wstring(value)});
}

std::string ReadWholeFile(const std::wstring& path) {
  std::ifstream file(std::filesystem::path(path), std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::wstring GitHomePath(GitFixture& fixture, std::wstring_view fileName) {
  return fixture.PathInRoot(std::wstring(L"git-home\\") + std::wstring(fileName));
}

void WriteRawFile(const std::wstring& path, const std::string& utf8Content) {
  std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
  GC_REQUIRE_MESSAGE(file.is_open(), "无法写入测试配置文件: " + Utf8(path));
  file.write(utf8Content.data(), static_cast<std::streamsize>(utf8Content.size()));
  GC_REQUIRE_MESSAGE(!file.fail(), "写入测试配置文件失败: " + Utf8(path));
}

std::string Describe(const AuthorIdentityConfig& config) {
  const auto stateText = [](ConfigValueState state) {
    switch (state) {
      case ConfigValueState::absent:
        return "没设过";
      case ConfigValueState::present:
        return "有值";
      case ConfigValueState::failed:
        return "失败";
    }
    return "?";
  };
  std::string text = std::string("name=") + stateText(config.name.state) +
                     "[" + Utf8(config.name.value) + "]；email=" +
                     stateText(config.email.state) + "[" + Utf8(config.email.value) + "]";
  if (!config.name.detail.empty()) {
    text += "；name 原因：" + Utf8(config.name.detail);
  }
  if (!config.email.detail.empty()) {
    text += "；email 原因：" + Utf8(config.email.detail);
  }
  text += "；身份=[" + Utf8(config.Identity()) + "]";
  return text;
}

}  // namespace

GC_TEST(author_config_repository_local_wins_over_user_config) {
  GitFixture fixture;
  Prepare(fixture);
  // 使用者層（= 夾具的臨時 global 檔案）與倉庫本體層都設了身份，且值不同。
  fixture.WriteUserConfig("[user]\n\tname = Global Name\n\temail = global@example.invalid\n");
  SetLocal(fixture, L"user.name", L"仓库本地名字");
  SetLocal(fixture, L"user.email", L"local@example.invalid");

  const AuthorIdentityConfig config = Load(fixture);
  GC_CHECK_MESSAGE(config.CommitterState() == CommitterIdentityState::available, Describe(config));
  GC_CHECK_MESSAGE(config.Identity() == Utf16("仓库本地名字 <local@example.invalid>"),
                   "仓库本体配置必须优先于使用者配置：" + Describe(config));
  // 關鍵一點：優先序是 Git 自己给的答复，本程序沒有自己去猜哪一個檔案。
  GC_CHECK_MESSAGE(config.Identity().find(Utf16("Global")) == std::wstring::npos,
                   "上一层的值不该漏进来：" + Describe(config));
}

GC_TEST(author_config_falls_back_to_user_config) {
  GitFixture fixture;
  Prepare(fixture);
  fixture.WriteUserConfig("[user]\n\tname = 用户层名字\n\temail = user@example.invalid\n");

  const AuthorIdentityConfig config = Load(fixture);
  GC_CHECK_MESSAGE(config.CommitterState() == CommitterIdentityState::available, Describe(config));
  GC_CHECK_MESSAGE(config.Identity() == Utf16("用户层名字 <user@example.invalid>"), Describe(config));
}

GC_TEST(author_config_mixes_layers_per_key) {
  // 姓名只在使用者層、邮箱只在倉庫本體：Git 是逐 key 取優先序的，本程序照它的答复合成。
  GitFixture fixture;
  Prepare(fixture);
  fixture.WriteUserConfig("[user]\n\tname = OnlyName\n");
  SetLocal(fixture, L"user.email", L"only@local.invalid");

  const AuthorIdentityConfig config = Load(fixture);
  GC_CHECK_MESSAGE(config.name.state == ConfigValueState::present &&
                       config.email.state == ConfigValueState::present,
                   Describe(config));
  GC_CHECK_MESSAGE(config.Identity() == Utf16("OnlyName <only@local.invalid>"), Describe(config));
}

GC_TEST(author_config_follows_include_directive) {
  // include 遞回包含也由 Git 處理：本程序只要讀到「最後生效的那個值」就夠了。
  GitFixture fixture;
  Prepare(fixture);
  WriteRawFile(GitHomePath(fixture, L"included.gitconfig"),
               "[user]\n\tname = Included Name\n\temail = included@example.invalid\n");
  fixture.WriteUserConfig("[include]\n\tpath = included.gitconfig\n");

  const AuthorIdentityConfig config = Load(fixture);
  GC_CHECK_MESSAGE(config.CommitterState() == CommitterIdentityState::available, Describe(config));
  GC_CHECK_MESSAGE(config.Identity() == Utf16("Included Name <included@example.invalid>"),
                   Describe(config));
}

GC_TEST(author_config_reports_missing_identity) {
  GitFixture fixture;
  Prepare(fixture);
  // 夾具的用戶設定檔是空的、系統設定被屏蔽：兩層都沒有身份。
  const AuthorIdentityConfig config = Load(fixture);
  GC_CHECK_MESSAGE(config.name.state == ConfigValueState::absent &&
                       config.email.state == ConfigValueState::absent,
                   Describe(config));
  GC_CHECK_MESSAGE(config.CommitterState() == CommitterIdentityState::missing,
                   "没设过是「明确缺失」，不是「还不知道」：" + Describe(config));
  GC_CHECK_MESSAGE(!config.ReadFailed(), "查到了「没有」也算查成功");
  const std::wstring notice = gc::git::BuildIdentityConfigNotice(config);
  GC_CHECK_MESSAGE(notice.find(L"姓名") != std::wstring::npos && !notice.empty(),
                   "缺身份时要提示用户按「姓名 <邮箱>」填写：" + Utf8(notice));
  GC_CHECK_MESSAGE(notice.find(L"不会") != std::wstring::npos,
                   "提示里要说明本程序不代改配置：" + Utf8(notice));
}

GC_TEST(author_config_treats_empty_value_as_missing) {
  GitFixture fixture;
  Prepare(fixture);
  SetLocal(fixture, L"user.name", L"Real Name");
  SetLocal(fixture, L"user.email", L"");  // 设过但为空：Git 会回答空值，身份仍然凑不出。

  const AuthorIdentityConfig config = Load(fixture);
  GC_CHECK_MESSAGE(config.email.state == ConfigValueState::present && config.email.value.empty(),
                   Describe(config));
  GC_CHECK_MESSAGE(config.CommitterState() == CommitterIdentityState::missing,
                   "空邮箱不算可用身份：" + Describe(config));
  GC_CHECK_MESSAGE(gc::git::BuildIdentityConfigNotice(config).find(L"user.email") != std::wstring::npos,
                   "说明里要点出缺的是哪一项：" + Utf8(gc::git::BuildIdentityConfigNotice(config)));
}

GC_TEST(author_config_trims_spaces_like_git_does) {
  GitFixture fixture;
  Prepare(fixture);
  SetLocal(fixture, L"user.name", L"  前后带空格  ");
  SetLocal(fixture, L"user.email", L"  spa ce@local.invalid  ");

  const AuthorIdentityConfig config = Load(fixture);
  // 实测：`git config --get` 回原值（含头尾空白），但 Git 把它当身份用时会自动修剪；
  // 界面显示的是「实际会写进提交的身份」，所以这里也修剪。
  GC_CHECK_MESSAGE(config.name.value == Utf16("  前后带空格  "),
                   "判读阶段保留 Git 原值：" + Describe(config));
  GC_CHECK_MESSAGE(config.Identity() == Utf16("前后带空格 <spa ce@local.invalid>"),
                   "合成身份时修剪外围空白：" + Describe(config));
  // 邮箱内部有空白时身份仍会被合成出来（Git 用 %ae 时同样会带出这个值），
  // 但表單校验会拒收——那条规则在 commit_identity_tests 里守著，这里不重复判定。
  GC_CHECK_MESSAGE(!gc::git::ValidateGitIdentity(config.Identity(), L"作者").empty(),
                   "带空白的邮箱要被表单校验拒绝：" + Utf8(config.Identity()));
}

GC_TEST(author_config_failure_is_reported_not_guessed) {
  GitFixture fixture;
  Prepare(fixture);
  SetLocal(fixture, L"user.name", L"Will Be Broken");
  // 把仓库本体配置写成语法错误的档案：Git 会 fatal 退出，本程序照实报告而不是当成“没设过”。
  const std::wstring localConfig = fixture.PathInRoot(L"repo\\.git\\config");
  WriteRawFile(localConfig, "[user\nname = broken\n");

  const AuthorIdentityConfig config = Load(fixture);
  GC_CHECK_MESSAGE(config.ReadFailed(), "配置档损坏必须报失败：" + Describe(config));
  GC_CHECK_MESSAGE(config.CommitterState() == CommitterIdentityState::unknown,
                   "读失败时不能断言身份可用与否：" + Describe(config));
  GC_CHECK_MESSAGE(config.Identity().empty(), "读失败绝不能凭空给出身份");
  GC_CHECK_MESSAGE(gc::git::BuildIdentityConfigNotice(config).find(L"失败") != std::wstring::npos,
                   "界面说明里要看得见「失败」：" + Utf8(gc::git::BuildIdentityConfigNotice(config)));
}

GC_TEST(author_config_query_does_not_touch_the_repository) {
  GitFixture fixture;
  Prepare(fixture);
  fixture.WriteUserConfig("[user]\n\tname = Only User\n\temail = only@example.invalid\n");
  SetLocal(fixture, L"user.name", L"Local Name");
  SetLocal(fixture, L"user.email", L"local@example.invalid");
  fixture.WriteFile(L"tracked.txt", "content\n");
  fixture.StageAll();
  fixture.Commit(L"baseline");

  const std::wstring localConfigPath = fixture.PathInRoot(L"repo\\.git\\config");
  const std::string before = ReadWholeFile(localConfigPath);
  const std::wstring headBefore = fixture.HeadSha();
  const long countBefore = fixture.CommitCount();
  const std::vector<std::wstring> statusBefore = fixture.StatusPorcelain();

  const AuthorIdentityConfig config = Load(fixture);
  GC_CHECK_MESSAGE(config.CommitterState() == CommitterIdentityState::available, Describe(config));

  GC_CHECK_MESSAGE(ReadWholeFile(localConfigPath) == before, "读身份不得改写仓库本体配置");
  GC_CHECK_MESSAGE(fixture.HeadSha() == headBefore, "读身份不得移动 HEAD");
  GC_CHECK_MESSAGE(fixture.CommitCount() == countBefore, "读身份不得产生提交");
  GC_CHECK_MESSAGE(fixture.StatusPorcelain() == statusBefore, "读身份不得改动工作区或索引");
  // 锁文件也不该出现（--no-optional-locks 就是为这一条加的）。
  GC_CHECK_MESSAGE(!std::filesystem::exists(std::filesystem::path(
                       fixture.PathInRoot(L"repo\\.git\\index.lock"))),
                   "读身份不得留下 index.lock");
}
