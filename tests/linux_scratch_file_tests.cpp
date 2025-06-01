// Linux 安全临时文件（linux 组）：提交信息与 NUL 路径清单的创建、权限、回收与拒绝规则。
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "platform/linux/scratch_files.h"
#include "platform/scratch_files.h"
#include "platform/text_encoding.h"
#include "support/posix_test_support.h"
#include "support/tiny_test.h"

namespace {

using gc::test::PosixTempDirectory;
using gc::test::RequireMakeDirectory;
using gc::test::RequireReadFile;
using gc::test::RequireSymlink;
using gc::test::RequireWriteFile;
using gc::test::ScopedEnvironmentVariable;

bool Exists(const std::wstring& path) {
  std::string bytes;
  return gc::platform::TryPathBytesFromWide(path, bytes) && ::access(bytes.c_str(), F_OK) == 0;
}

mode_t ModeOf(const std::wstring& path) {
  std::string bytes;
  if (!gc::platform::TryPathBytesFromWide(path, bytes)) {
    return 0;
  }
  struct stat info {};
  return ::lstat(bytes.c_str(), &info) == 0 ? info.st_mode : 0;
}

}  // namespace

GC_TEST(linux_scratch_commit_message_is_private_and_exact) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  ScopedEnvironmentVariable xdg("XDG_RUNTIME_DIR");
  ScopedEnvironmentVariable tmp("TMPDIR", temp.PathBytes());

  const std::wstring message = L"标题：中文 😀\n\n正文带 ' \" - $&;\n";
  const gc::platform::CommitMessageFileWrite written = gc::platform::WriteCommitMessageFile(message);
  GC_REQUIRE_MESSAGE(written.written, gc::platform::WideToUtf8(written.failureReason));
  GC_CHECK(written.path.find(temp.Path()) == 0);
  GC_CHECK(written.payloadBytes == gc::platform::WideToUtf8(message).size());
  GC_CHECK_MESSAGE((ModeOf(written.path) & 07777) == 0600, "临时文件必须只对当前用户可读写");

  std::string pathBytes;
  GC_REQUIRE_MESSAGE(gc::platform::TryPathBytesFromWide(written.path, pathBytes), "路径应可无损编码");
  std::string content;
  RequireReadFile(pathBytes, content);
  GC_CHECK(content == gc::platform::WideToUtf8(message));

  gc::platform::RemoveCommitMessageFile(written.path);
  GC_CHECK_MESSAGE(!Exists(written.path), "Git 退出后应能回收自己写出的文件");

  const gc::platform::CommitMessageFileWrite empty = gc::platform::WriteCommitMessageFile(L"");
  GC_CHECK(!empty.written);
  GC_CHECK(!empty.failureReason.empty());

  std::wstring loneSurrogate = L"x";
  loneSurrogate.push_back(static_cast<wchar_t>(0xD800));
  const gc::platform::CommitMessageFileWrite invalid =
      gc::platform::WriteCommitMessageFile(loneSurrogate);
  GC_CHECK_MESSAGE(!invalid.written, "无法无损编码的提交信息必须拒绝写出");
  GC_CHECK(!invalid.failureReason.empty());

  std::wstring withNul = L"a";
  withNul.push_back(L'\0');
  withNul.push_back(L'b');
  const gc::platform::CommitMessageFileWrite nulMessage =
      gc::platform::WriteCommitMessageFile(withNul);
  GC_CHECK_MESSAGE(!nulMessage.written, "内嵌 NUL 的提交信息必须拒绝写出");
  GC_CHECK(!nulMessage.failureReason.empty());
}

GC_TEST(linux_scratch_pathspec_is_nul_separated_and_private) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  ScopedEnvironmentVariable xdg("XDG_RUNTIME_DIR");
  ScopedEnvironmentVariable tmp("TMPDIR", temp.PathBytes());

  const std::vector<std::wstring> elements{L"-leading-dash", L"中文 空格.txt", L"quote'\"",
                                           L"emoji😀", L"semi;$&-", L"tab\tname"};
  const gc::platform::PathspecFileWrite written =
      gc::platform::WriteNulPathspecFile(elements, L"gc-unstage");
  GC_REQUIRE_MESSAGE(written.written, gc::platform::WideToUtf8(written.failureReason));
  GC_CHECK(written.path.find(temp.Path()) == 0);
  GC_CHECK_MESSAGE((ModeOf(written.path) & 07777) == 0600, "路径清单必须只对当前用户可读写");

  std::string expected;
  for (const std::wstring& element : elements) {
    expected += gc::platform::WideToUtf8(element);
    expected.push_back('\0');
  }
  std::string pathBytes;
  GC_REQUIRE_MESSAGE(gc::platform::TryPathBytesFromWide(written.path, pathBytes), "路径应可无损编码");
  std::string content;
  RequireReadFile(pathBytes, content);
  GC_CHECK(content == expected);

  gc::platform::RemoveNulPathspecFile(written.path);
  GC_CHECK(!Exists(written.path));

  GC_CHECK(!gc::platform::WriteNulPathspecFile({}).written);
  GC_CHECK(!gc::platform::WriteNulPathspecFile({L""}).written);

  std::wstring withNul = L"a";
  withNul.push_back(L'\0');
  withNul.push_back(L'b');
  GC_CHECK_MESSAGE(!gc::platform::WriteNulPathspecFile({withNul}).written, "内嵌 NUL 必须拒绝");
  GC_CHECK(!gc::platform::WriteNulPathspecFile({L"ok"}, L"bad/prefix").written);
}

GC_TEST(linux_scratch_rejects_untrusted_temp_locations) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  ScopedEnvironmentVariable xdg("XDG_RUNTIME_DIR");

  const std::string safe = temp.PathIn("safe");
  RequireMakeDirectory(safe, 0700);
  const std::string link = temp.PathIn("temp-link");
  RequireSymlink(safe, link);
  {
    ScopedEnvironmentVariable tmp("TMPDIR", link);
    const gc::platform::ScratchDirectory resolved = gc::platform::ResolveScratchDirectory();
    GC_CHECK_MESSAGE(!resolved.ok, "TMPDIR 是符号链接时必须拒绝");
    GC_CHECK(resolved.failureReason.find(L"符号链接") != std::wstring::npos);
  }
  {
    // 结尾斜杠会迫使 lstat 跟随最终链接；必须先词法清理再判断。
    ScopedEnvironmentVariable tmp("TMPDIR", link + "/");
    const gc::platform::ScratchDirectory resolved = gc::platform::ResolveScratchDirectory();
    GC_CHECK_MESSAGE(!resolved.ok, "TMPDIR 以斜杠结尾也不能绕过符号链接检查");
  }
  {
    ScopedEnvironmentVariable tmp("TMPDIR", safe + "/.");
    const gc::platform::ScratchDirectory resolved = gc::platform::ResolveScratchDirectory();
    GC_CHECK_MESSAGE(!resolved.ok, "TMPDIR 含 \".\" 段必须拒绝");
  }
  {
    const std::string sharedRuntime = temp.PathIn("runtime-shared");
    RequireMakeDirectory(sharedRuntime, 0755);
    GC_REQUIRE(::chmod(sharedRuntime.c_str(), 0755) == 0, "chmod runtime-shared 失败");
    ScopedEnvironmentVariable xdgRuntime("XDG_RUNTIME_DIR", sharedRuntime);
    const gc::platform::ScratchDirectory resolved = gc::platform::ResolveScratchDirectory();
    GC_CHECK_MESSAGE(!resolved.ok, "XDG_RUNTIME_DIR 非 0700 必须拒绝");
  }
  {
    ScopedEnvironmentVariable tmp("TMPDIR", std::string("relative/tmp"));
    const gc::platform::ScratchDirectory resolved = gc::platform::ResolveScratchDirectory();
    GC_CHECK_MESSAGE(!resolved.ok, "相对 TMPDIR 必须拒绝");
    GC_CHECK(!resolved.failureReason.empty());
  }
  {
    const std::string file = temp.PathIn("not-a-dir");
    RequireWriteFile(file, "x");
    ScopedEnvironmentVariable tmp("TMPDIR", file);
    const gc::platform::ScratchDirectory resolved = gc::platform::ResolveScratchDirectory();
    GC_CHECK_MESSAGE(!resolved.ok, "TMPDIR 指向文件必须拒绝");
  }
  {
    const std::string openDir = temp.PathIn("open-dir");
    RequireMakeDirectory(openDir, 0777);
    GC_REQUIRE(::chmod(openDir.c_str(), 0777) == 0, "chmod 失败");
    ScopedEnvironmentVariable tmp("TMPDIR", openDir);
    const gc::platform::ScratchDirectory resolved = gc::platform::ResolveScratchDirectory();
    GC_CHECK_MESSAGE(!resolved.ok, "无 sticky 的组/其他人可写目录必须拒绝");
  }
  {
    // XDG_RUNTIME_DIR 优先于 TMPDIR，且必须是 0700。
    ScopedEnvironmentVariable xdgRuntime("XDG_RUNTIME_DIR", safe);
    ScopedEnvironmentVariable tmp("TMPDIR", temp.PathBytes());
    const gc::platform::ScratchDirectory resolved = gc::platform::ResolveScratchDirectory();
    GC_REQUIRE_MESSAGE(resolved.ok, gc::platform::WideToUtf8(resolved.failureReason));
    GC_CHECK(resolved.path == gc::platform::Utf8ToWide(safe));
  }
}

GC_TEST(linux_scratch_removal_only_touches_owned_paths) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  ScopedEnvironmentVariable xdg("XDG_RUNTIME_DIR");
  ScopedEnvironmentVariable tmp("TMPDIR", temp.PathBytes());

  const gc::platform::PathspecFileWrite written =
      gc::platform::WriteNulPathspecFile({L"keep.txt"}, L"gc-add");
  GC_REQUIRE_MESSAGE(written.written, gc::platform::WideToUtf8(written.failureReason));

  // 不是本模块写出的文件：即使路径存在也不能删。
  const std::string unrelated = temp.PathIn("unrelated.txt");
  RequireWriteFile(unrelated, "keep");
  gc::platform::RemoveNulPathspecFile(gc::platform::Utf8ToWide(unrelated));
  GC_CHECK(Exists(gc::platform::Utf8ToWide(unrelated)));

  // 注册过的文件被换成符号链接：dev/inode 不再匹配，只放弃回收，绝不跟随链接删目标。
  std::string writtenBytes;
  GC_REQUIRE_MESSAGE(gc::platform::TryPathBytesFromWide(written.path, writtenBytes), "路径应可无损编码");
  GC_REQUIRE(::unlink(writtenBytes.c_str()) == 0, "替换前删除文件失败");
  RequireSymlink(unrelated, writtenBytes);
  gc::platform::RemoveNulPathspecFile(written.path);
  GC_CHECK_MESSAGE(Exists(written.path), "被替换成别的 inode 后不再认领这条路径");
  GC_CHECK_MESSAGE(Exists(gc::platform::Utf8ToWide(unrelated)), "绝不能跟随链接删除目标");
}

GC_TEST(linux_scratch_cleanup_tolerates_missing_file) {
  PosixTempDirectory temp;
  GC_REQUIRE_MESSAGE(temp.Valid(), temp.FailureReason());
  ScopedEnvironmentVariable xdg("XDG_RUNTIME_DIR");
  ScopedEnvironmentVariable tmp("TMPDIR", temp.PathBytes());
  const gc::platform::CommitMessageFileWrite written =
      gc::platform::WriteCommitMessageFile(L"x");
  GC_REQUIRE_MESSAGE(written.written, gc::platform::WideToUtf8(written.failureReason));
  gc::platform::RemoveCommitMessageFile(written.path);
  gc::platform::RemoveCommitMessageFile(written.path);  // 第二次是无操作
  GC_CHECK(!Exists(written.path));
}
