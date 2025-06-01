// Linux 路径与流程痕迹探测的真实文件系统测试（linux 组）。
// 全部在测试自己拥有的临时根内进行：不启动 Git、不碰用户真实仓库或配置。
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "platform/path.h"
#include "platform/text_encoding.h"
#include "support/posix_test_support.h"
#include "support/tiny_test.h"

namespace {

using gc::platform::PathEntryKind;
using gc::platform::PathExistence;
using gc::platform::PathProbeStatus;
using gc::test::PosixTempDirectory;
using gc::test::RequireMakeDirectory;
using gc::test::RequireWriteFile;

}  // namespace

GC_TEST(linux_path_probe_handles_special_names_losslessly) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());

  const std::vector<std::string> names{
      "中文 文件.txt", "emoji-😀.txt", "quote'\"名.txt", "-leading-dash.txt",
      "semi;colon&amp dollar$.txt", "A", "a"};
  for (const std::string& name : names) {
    RequireWriteFile(temp.PathIn(name), "content");
  }

  unsigned long long inodeA = 0;
  unsigned long long inodeLowerA = 0;
  for (const std::string& name : names) {
    const std::wstring path = gc::platform::Utf8ToWide(temp.PathIn(name));
    const gc::platform::PathFacts facts = gc::platform::ProbePath(path);
    GC_CHECK_MESSAGE(facts.status == PathProbeStatus::ok, "特殊文件名必须能无损探测：" + name);
    GC_CHECK(facts.existence == PathExistence::present);
    GC_CHECK(facts.kind == PathEntryKind::regularFile);
    GC_CHECK(facts.targetKind == PathEntryKind::regularFile);
    GC_CHECK(facts.device != 0 && facts.inode != 0);
    GC_CHECK_MESSAGE(facts.ownerIsCurrentUser, "临时文件由当前用户创建");
    GC_CHECK((facts.permissions & 0777u) == 0600u || (facts.permissions & 0777u) == 0644u);
    if (name == "A") {
      inodeA = facts.inode;
    } else if (name == "a") {
      inodeLowerA = facts.inode;
    }

    const gc::git::WorktreeFileFacts preview = gc::platform::ProbeWorktreeFileForPreview(path);
    GC_CHECK(preview.probed && preview.exists && preview.readable);
    GC_CHECK(!preview.isDirectory && !preview.containsNullByte);
  }
  GC_CHECK_MESSAGE(inodeA != 0 && inodeLowerA != 0 && inodeA != inodeLowerA,
                   "Linux 路径区分大小写：A 与 a 是两个不同的文件");

  // 同一路径反复探测得到同一份身份。
  const gc::platform::PathFacts again =
      gc::platform::ProbePath(gc::platform::Utf8ToWide(temp.PathIn("中文 文件.txt")));
  GC_CHECK(again.device != 0 && again.inode != 0);
}

GC_TEST(linux_path_probe_records_symlink_identity_separately) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  const std::string target = temp.PathIn("target.txt");
  const std::string link = temp.PathIn("link.txt");
  const std::string broken = temp.PathIn("broken-link");
  RequireWriteFile(target, "hello");
  gc::test::RequireSymlink(target, link);
  gc::test::RequireSymlink(temp.PathIn("missing-target"), broken);

  const gc::platform::PathFacts targetFacts = gc::platform::ProbePath(gc::platform::Utf8ToWide(target));
  const gc::platform::PathFacts linkFacts = gc::platform::ProbePath(gc::platform::Utf8ToWide(link));
  GC_CHECK(targetFacts.status == PathProbeStatus::ok);
  GC_CHECK(linkFacts.status == PathProbeStatus::ok);
  GC_CHECK(linkFacts.isSymlink && linkFacts.kind == PathEntryKind::symbolicLink);
  GC_CHECK(linkFacts.targetExists && linkFacts.targetKind == PathEntryKind::regularFile);
  GC_CHECK_MESSAGE(linkFacts.inode != targetFacts.inode, "链接本身与目标必须是两个身份");
  GC_CHECK(linkFacts.targetInode == targetFacts.inode);
  GC_CHECK(linkFacts.readable);

  // 不跟随最终链接时，目标类型保留为 symbolicLink，不会折叠成普通文件。
  const gc::platform::PathFacts raw = gc::platform::ProbePath(gc::platform::Utf8ToWide(link), false);
  GC_CHECK(raw.isSymlink && raw.targetKind == PathEntryKind::symbolicLink);

  const gc::platform::PathFacts brokenFacts = gc::platform::ProbePath(gc::platform::Utf8ToWide(broken));
  GC_CHECK(brokenFacts.status == PathProbeStatus::ok && brokenFacts.isSymlink);
  GC_CHECK(!brokenFacts.targetExists && brokenFacts.targetKind == PathEntryKind::missing);
  const gc::git::WorktreeFileFacts preview = gc::platform::ProbeWorktreeFileForPreview(gc::platform::Utf8ToWide(broken));
  GC_CHECK(preview.probed && preview.exists && !preview.readable);
  GC_CHECK(!preview.failureReason.empty());

  // ToAbsolutePath 只做词法折叠，不解析符号链接：路径文本里仍保留 link 段。
  const std::wstring absolute = gc::platform::ToAbsolutePath(gc::platform::Utf8ToWide(link));
  GC_CHECK(absolute.find(L"link.txt") != std::wstring::npos);
}

GC_TEST(linux_path_probe_distinguishes_missing_inaccessible_and_failed) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());

  const gc::platform::PathFacts missing =
      gc::platform::ProbePath(gc::platform::Utf8ToWide(temp.PathIn("no-such-file")));
  GC_CHECK(missing.status == PathProbeStatus::missing);
  GC_CHECK(missing.existence == PathExistence::missing);
  GC_CHECK(!missing.failureReason.empty());

  // 中间路径是符号链接环：lstat 直接报 ELOOP，既不是“不存在”也不是“权限不足”。
  const std::string loop = temp.PathIn("loop");
  gc::test::RequireSymlink(loop, loop);
  const gc::platform::PathFacts failed =
      gc::platform::ProbePath(gc::platform::Utf8ToWide(loop + "/file"));
  GC_CHECK_MESSAGE(failed.status == PathProbeStatus::failed, "ELOOP 必须归入其它失败");
  GC_CHECK(failed.existence == PathExistence::unknown);
  GC_CHECK(!failed.failureReason.empty());

  // 无法无损表示的名字在发起系统调用前就被拒绝（Linux 上 wchar_t 可承载代理项码位）。
  std::wstring unrepresentable = temp.Path();
  unrepresentable.push_back(L'/');
  unrepresentable.push_back(static_cast<wchar_t>(0xD800));
  const gc::platform::PathFacts badName = gc::platform::ProbePath(unrepresentable);
  GC_CHECK_MESSAGE(badName.status == PathProbeStatus::unrepresentable, "非 UTF-8 路径必须明确失败");
  GC_CHECK(badName.existence == PathExistence::unknown);

  if (::geteuid() == 0) {
    // root 不受目录执行位限制，无法在本机复现 EACCES；其余断言仍然有效。
    GC_CHECK_MESSAGE(true, "root 环境：跳过 EACCES 构造");
    return;
  }

  const std::string blockedDir = temp.PathIn("blocked");
  RequireMakeDirectory(blockedDir, 0700);
  const std::string blockedFile = blockedDir + "/inside.txt";
  RequireWriteFile(blockedFile, "secret");
  GC_REQUIRE(::chmod(blockedDir.c_str(), 0000) == 0, "chmod blocked 目录失败");

  const gc::platform::PathFacts denied =
      gc::platform::ProbePath(gc::platform::Utf8ToWide(blockedFile));
  GC_CHECK_MESSAGE(denied.status == PathProbeStatus::inaccessible, "EACCES 必须与“不存在”分开");
  GC_CHECK(denied.existence == PathExistence::unknown);
  GC_CHECK(!denied.failureReason.empty());

  const gc::git::WorktreeFileFacts deniedPreview =
      gc::platform::ProbeWorktreeFileForPreview(gc::platform::Utf8ToWide(blockedFile));
  GC_CHECK_MESSAGE(!deniedPreview.probed, "权限不足不能伪装成确定结论");
  GC_CHECK(!deniedPreview.exists);

  GC_REQUIRE(::chmod(blockedDir.c_str(), 0700) == 0, "恢复 blocked 目录权限失败");

  // 文件存在但当前用户不可读：存在性是确定的，可读性单独为 false。
  const std::string unreadable = temp.PathIn("unreadable.txt");
  RequireWriteFile(unreadable, "content", 0600);
  GC_REQUIRE(::chmod(unreadable.c_str(), 0000) == 0, "chmod unreadable 文件失败");
  const gc::platform::PathFacts unreadableFacts =
      gc::platform::ProbePath(gc::platform::Utf8ToWide(unreadable));
  GC_CHECK(unreadableFacts.status == PathProbeStatus::ok);
  GC_CHECK(unreadableFacts.existence == PathExistence::present);
  GC_CHECK(!unreadableFacts.readable);
  const gc::git::WorktreeFileFacts unreadablePreview =
      gc::platform::ProbeWorktreeFileForPreview(gc::platform::Utf8ToWide(unreadable));
  GC_CHECK_MESSAGE(unreadablePreview.probed && unreadablePreview.exists && !unreadablePreview.readable,
                   "存在但不可读必须是“存在 + 不可读”");
  GC_REQUIRE(::chmod(unreadable.c_str(), 0600) == 0, "恢复 unreadable 文件权限失败");

  // 符号链接本身可 lstat，但目标在不可读目录里：目标存在性必须是 unknown，不能报“目标不存在”。
  const std::string secretDir = temp.PathIn("secret-dir");
  RequireMakeDirectory(secretDir, 0700);
  const std::string secretFile = secretDir + "/secret.txt";
  RequireWriteFile(secretFile, "secret");
  const std::string secretLink = temp.PathIn("secret-link");
  gc::test::RequireSymlink(secretFile, secretLink);
  GC_REQUIRE(::chmod(secretDir.c_str(), 0000) == 0, "chmod secret-dir 失败");
  const gc::platform::PathFacts linkFacts =
      gc::platform::ProbePath(gc::platform::Utf8ToWide(secretLink));
  GC_CHECK(linkFacts.status == PathProbeStatus::ok && linkFacts.isSymlink);
  GC_CHECK_MESSAGE(linkFacts.targetExistence == PathExistence::unknown,
                   "目标 EACCES 必须归为 unknown");
  GC_CHECK(!linkFacts.failureReason.empty());
  const gc::git::WorktreeFileFacts linkPreview =
      gc::platform::ProbeWorktreeFileForPreview(gc::platform::Utf8ToWide(secretLink));
  GC_CHECK(!linkPreview.readable);
  GC_REQUIRE(::chmod(secretDir.c_str(), 0700) == 0, "恢复 secret-dir 权限失败");
}

GC_TEST(linux_path_probe_preview_distinguishes_text_directory_and_binary) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  const std::string text = temp.PathIn("text.txt");
  const std::string binary = temp.PathIn("binary.bin");
  const std::string empty = temp.PathIn("empty.txt");
  const std::string dir = temp.PathIn("a-dir");
  const std::string content = "hello 世界\nsecond line\n";
  RequireWriteFile(text, content, 0644);
  RequireWriteFile(binary, std::string("PK\1\2\0rest", 9), 0644);
  RequireWriteFile(empty, "", 0644);
  RequireMakeDirectory(dir);

  const gc::git::WorktreeFileFacts textFacts =
      gc::platform::ProbeWorktreeFileForPreview(gc::platform::Utf8ToWide(text));
  GC_CHECK(textFacts.probed && textFacts.exists && textFacts.readable);
  GC_CHECK(textFacts.sizeBytes == content.size());
  GC_CHECK(!textFacts.containsNullByte);

  const gc::git::WorktreeFileFacts binaryFacts =
      gc::platform::ProbeWorktreeFileForPreview(gc::platform::Utf8ToWide(binary));
  GC_CHECK(binaryFacts.probed && binaryFacts.readable && binaryFacts.containsNullByte);

  const gc::git::WorktreeFileFacts emptyFacts =
      gc::platform::ProbeWorktreeFileForPreview(gc::platform::Utf8ToWide(empty));
  GC_CHECK(emptyFacts.probed && emptyFacts.readable && emptyFacts.sizeBytes == 0);
  GC_CHECK(!emptyFacts.containsNullByte);

  const gc::git::WorktreeFileFacts dirFacts =
      gc::platform::ProbeWorktreeFileForPreview(gc::platform::Utf8ToWide(dir));
  GC_CHECK(dirFacts.probed && dirFacts.exists && dirFacts.isDirectory);

  // FIFO 没有可预览的文本；预览不能因为打开管道而挂住。
  const std::string fifo = temp.PathIn("pipe.fifo");
  GC_REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0, "建 FIFO 失败");
  const gc::git::WorktreeFileFacts fifoFacts =
      gc::platform::ProbeWorktreeFileForPreview(gc::platform::Utf8ToWide(fifo));
  GC_CHECK(fifoFacts.probed && fifoFacts.exists && !fifoFacts.readable);
  GC_CHECK(!fifoFacts.isDirectory);
}

GC_TEST(linux_workflow_probe_reads_git_marker_files) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  const std::string gitDir = temp.PathIn("repo/.git");
  RequireMakeDirectory(temp.PathIn("repo"));
  RequireMakeDirectory(gitDir);
  RequireWriteFile(gitDir + "/MERGE_HEAD", "0123456789abcdef\n");
  RequireWriteFile(gitDir + "/index.lock", "");
  RequireMakeDirectory(gitDir + "/rebase-merge");

  const gc::git::WorkflowProbeResult strict =
      gc::platform::ProbeRepositoryWorkflowStateStrict(gc::platform::Utf8ToWide(gitDir));
  GC_CHECK_MESSAGE(strict.readable, "每个痕迹都应问出确定答案");
  GC_CHECK(strict.state.mergeInProgress);
  GC_CHECK(strict.state.indexLocked);
  GC_CHECK(strict.state.rebaseInProgress);
  GC_CHECK(!strict.state.revertInProgress);
  GC_CHECK(!strict.state.cherryPickInProgress);

  const gc::git::RepositoryWorkflowState loose =
      gc::platform::ProbeRepositoryWorkflowState(gc::platform::Utf8ToWide(gitDir));
  GC_CHECK(loose.mergeInProgress && loose.indexLocked && loose.rebaseInProgress);

  // 读不到 Git 目录：严格版本必须 readable=false，不能把“看不见”当成“没有流程”。
  const gc::git::WorkflowProbeResult missing =
      gc::platform::ProbeRepositoryWorkflowStateStrict(gc::platform::Utf8ToWide(temp.PathIn("no.git")));
  GC_CHECK(!missing.readable);
  GC_CHECK(!missing.failure.empty());

  if (::geteuid() != 0) {
    const std::string blockedGit = temp.PathIn("blocked.git");
    RequireMakeDirectory(blockedGit, 0700);
    GC_REQUIRE(::chmod(blockedGit.c_str(), 0000) == 0, "chmod blocked.git 失败");
    const gc::git::WorkflowProbeResult blocked =
        gc::platform::ProbeRepositoryWorkflowStateStrict(gc::platform::Utf8ToWide(blockedGit));
    GC_CHECK_MESSAGE(!blocked.readable, "目录里的痕迹问不成时整轮必须 readable=false");
    GC_CHECK(!blocked.failure.empty());
    GC_REQUIRE(::chmod(blockedGit.c_str(), 0700) == 0, "恢复 blocked.git 权限失败");
  }
}

GC_TEST(linux_path_normalization_keeps_posix_rules) {
  GC_CHECK(gc::platform::NormalizePathSeparators(L"a\\b/c") == L"a\\b/c");
  GC_CHECK(gc::platform::IsAbsolutePath(L"/tmp"));
  GC_CHECK(!gc::platform::IsAbsolutePath(L"tmp"));
  GC_CHECK_MESSAGE(!gc::platform::IsAbsolutePath(L"C:\\tmp"), "Linux 不认盘符绝对路径");
  GC_CHECK(gc::platform::ToAbsolutePath(L"").empty());

  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  const std::wstring base = temp.Path();
  const std::wstring combined =
      gc::platform::ToAbsolutePathInDirectory(base, L"a/../b c");
  GC_CHECK(combined.find(L"b c") != std::wstring::npos);
  GC_CHECK(combined.find(L"..") == std::wstring::npos);

  // 词法折叠不能借机改大小写。
  const std::wstring mixed = gc::platform::ToAbsolutePath(base + L"/MiXeD");
  GC_CHECK(mixed.find(L"MiXeD") != std::wstring::npos);
}
