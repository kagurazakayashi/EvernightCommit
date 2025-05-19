// ProbeWorktreeFileForPreview 的真实 Windows 测试（windows 组：只碰文件系统，不启动 Git）。
// 存在的必要性：预览的纯逻辑用例（diff_view_tests.cpp）是自己手搓 WorktreeFileFacts 的，
// 因此「平台函数把错误码翻译成哪个事实」这一环从来没有被钉过——而 R8 的缺陷恰好就在这一环
// （明确不存在被标成 exists=true）。这里全部在用例自己拥有的临时目录里跑，
// 覆盖：不存在、上级目录不存在、名称非法、目录、空文件、普通文本、二进制，
// 以及预览计划如何消费这些真实事实（措辞必须与判定同源）。
#include "support/git_fixture.h"
#include "support/tiny_test.h"

#include <windows.h>

#include <string>

#include "git/diff_view.h"
#include "git/workspace_model.h"
#include "platform/windows/win_path.h"

namespace {

using gc::git::ChangeItem;
using gc::git::ChangeKind;
using gc::git::ChangeSide;
using gc::git::DiffViewKind;
using gc::git::WorktreeFileFacts;
using gc::platform::ProbeWorktreeFileForPreview;
using gc::test::TempDirectory;

// 在用例自己的临时根里落一个文件；失败即前置条件失败（不静默跳过）。
void WriteFile(const std::wstring& path, const std::string& bytes) {
  const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
  GC_REQUIRE_MESSAGE(handle != INVALID_HANDLE_VALUE, "测试建文件失败：CreateFileW");
  DWORD written = 0;
  const bool ok = bytes.empty() ||
                  (::WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                               nullptr) != 0 &&
                   written == static_cast<DWORD>(bytes.size()));
  ::CloseHandle(handle);
  GC_REQUIRE_MESSAGE(ok, "测试建文件失败：WriteFile");
}

// 临时根 + 用例自己拥有的目录：TempDirectory 由调用方持有，出作用域即清理。
void MakeOwnedRoot(TempDirectory* temp, std::wstring_view prefix) {
  std::string reason;
  GC_REQUIRE_MESSAGE(temp->Create(prefix, reason), "测试临时根创建失败：" + reason);
  GC_REQUIRE_MESSAGE(::CreateDirectoryW(temp->Path().c_str(), nullptr) != 0 ||
                         ::GetLastError() == ERROR_ALREADY_EXISTS,
                     "测试建目录失败");
}

ChangeItem UntrackedItem(std::wstring_view relativePath) {
  ChangeItem item;
  item.kind = ChangeKind::untracked;
  item.statusCode = L"??";
  item.path = relativePath;
  return item;
}

}  // namespace

GC_TEST(win_path_probe_missing_file_is_not_reported_as_existing) {
  TempDirectory temp;
  MakeOwnedRoot(&temp, L"evernight-probe-");
  const std::wstring root = temp.Path();

  // 文件明确不存在（目录在）：probed=true + exists=false，绝不能是 exists=true。
  const WorktreeFileFacts gone = ProbeWorktreeFileForPreview(root + L"\\missing.txt");
  GC_CHECK_MESSAGE(gone.probed, "「明确找不到」是确定的探测结论");
  GC_CHECK_MESSAGE(!gone.exists, "R8：明确不存在不得标成存在");
  GC_CHECK(!gone.isDirectory);
  GC_CHECK(!gone.readable);
  GC_CHECK(!gone.failureReason.empty());

  // 上级目录也不存在：同样是「明确不存在」这一类，不是「读不到属性」。
  const WorktreeFileFacts noParent = ProbeWorktreeFileForPreview(root + L"\\no-such-dir\\missing.txt");
  GC_CHECK_MESSAGE(noParent.probed, "上级目录不存在同样是确定的「不存在」");
  GC_CHECK(!noParent.exists);

  // 名称非法（NTFS 保留字符）：GetFileAttributesW 报的既不是 2 也不是 3，
  // 存在性无从判定，只能按「读不到属性」返回——不得冒充「文件消失了」。
  const WorktreeFileFacts badName = ProbeWorktreeFileForPreview(root + L"\\bad<>name.txt");
  GC_CHECK_MESSAGE(!badName.probed, "非「找不到」类错误必须判为无法判定");
  GC_CHECK(!badName.exists);
  GC_CHECK(!badName.failureReason.empty());
}

GC_TEST(win_path_probe_reads_real_file_shapes) {
  TempDirectory temp;
  MakeOwnedRoot(&temp, L"evernight-probe-");
  const std::wstring root = temp.Path();
  const std::wstring subDir = root + L"\\a-dir";
  GC_REQUIRE(::CreateDirectoryW(subDir.c_str(), nullptr) != 0, "测试建子目录失败");

  // 目录：存在但不是文件。
  const WorktreeFileFacts dir = ProbeWorktreeFileForPreview(subDir);
  GC_CHECK(dir.probed && dir.exists);
  GC_CHECK_MESSAGE(dir.isDirectory, "目录条目的 isDirectory 必须为真");

  // 空文件：可读、大小 0、没有 NUL 可谈。
  WriteFile(root + L"\\empty.txt", "");
  const WorktreeFileFacts empty = ProbeWorktreeFileForPreview(root + L"\\empty.txt");
  GC_CHECK(empty.probed && empty.exists && empty.readable);
  GC_CHECK(!empty.isDirectory);
  GC_CHECK(empty.sizeBytes == 0);
  GC_CHECK(!empty.containsNullByte);

  // 普通文本（含中文，UTF-8 字节）：大小按实际字节、不含 NUL。
  const std::string text = "hello 世界\nsecond line\n";
  WriteFile(root + L"\\text.txt", text);
  const WorktreeFileFacts plain = ProbeWorktreeFileForPreview(root + L"\\text.txt");
  GC_CHECK(plain.probed && plain.exists && plain.readable);
  GC_CHECK(plain.sizeBytes == static_cast<unsigned long long>(text.size()));
  GC_CHECK_MESSAGE(!plain.containsNullByte, "纯文本不得被判成二进制");

  // 二进制：采样窗口内出现 NUL 即按二进制对待。
  std::string binary = "PK\1\2";
  binary.push_back('\0');
  binary += "rest";
  WriteFile(root + L"\\binary.bin", binary);
  const WorktreeFileFacts blob = ProbeWorktreeFileForPreview(root + L"\\binary.bin");
  GC_CHECK(blob.probed && blob.exists && blob.readable);
  GC_CHECK_MESSAGE(blob.containsNullByte, "含 NUL 的采样必须判为二进制");

  // 空路径：连探测对象都没有，一律按不可用（probed=false）。
  const WorktreeFileFacts nothing = ProbeWorktreeFileForPreview(L"");
  GC_CHECK(!nothing.probed && !nothing.exists);
  GC_CHECK(!nothing.failureReason.empty());
}

GC_TEST(win_path_probe_facts_drive_preview_plan_wording) {
  TempDirectory temp;
  MakeOwnedRoot(&temp, L"evernight-probe-");
  const std::wstring root = temp.Path();
  const ChangeItem item = UntrackedItem(L"missing.txt");

  // 「明确不存在」→ 计划给的措辞必须是「已不在工作区」，而不是「没有读取权限」。
  const WorktreeFileFacts gone = ProbeWorktreeFileForPreview(root + L"\\missing.txt");
  const gc::git::DiffViewPlan gonePlan = gc::git::BuildDiffViewPlan(ChangeSide::unstaged, item, gone);
  GC_CHECK(gonePlan.kind == DiffViewKind::blocked);
  GC_CHECK_MESSAGE(gonePlan.blockedReason.find(L"已不在工作区") != std::wstring::npos,
                   "不存在的措辞要与「明确不存在」同源");
  GC_CHECK(gonePlan.blockedReason.find(L"权限") == std::wstring::npos);
  GC_CHECK(gonePlan.arguments.empty());

  // 「无法判定」→ 措辞必须落在「读不到属性」，不能暗示文件消失了。
  const WorktreeFileFacts unknown = ProbeWorktreeFileForPreview(root + L"\\bad<>name.txt");
  const gc::git::DiffViewPlan unknownPlan =
      gc::git::BuildDiffViewPlan(ChangeSide::unstaged, item, unknown);
  GC_CHECK(unknownPlan.kind == DiffViewKind::blocked);
  GC_CHECK_MESSAGE(unknownPlan.blockedReason.find(L"无法读取该文件的属性") != std::wstring::npos,
                   "无法判定的措辞要与「读不到属性」同源");
  GC_CHECK(unknownPlan.blockedReason.find(L"已不在工作区") == std::wstring::npos);
}
