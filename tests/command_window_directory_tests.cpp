// 操作目录的生命周期测试：时间阈值的参考值、陈旧判定的保守取向、原子认领的所有权边界，
// 以及回收例程在“还有人用 / 删不动 / 来历不明”三种情况下的保留行为。
// 全部落在测试自己拥有的隔离临时目录里：不碰开发仓库，也不碰用户真实的 %TEMP% 内容
// （需要让回收例程看得见时，只改本进程的 TEMP/TMP 环境变量，用例结束立刻还原）。
#include "support/git_fixture.h"
#include "support/tiny_test.h"

#include <windows.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "git/command_window.h"
#include "platform/windows/command_window_runner.h"
#include "platform/windows/raii.h"
#include "platform/windows/utf_text.h"

namespace {

using gc::platform::CommandWindowRunner;
using gc::test::TempDirectory;

constexpr std::uint64_t kMinuteTicks = 60ULL * 10'000'000ULL;  // 一分钟在 100ns 单位下是多少

std::wstring WideName(std::string_view asciiName) {
  return gc::platform::Utf8ToUtf16(std::string(asciiName));
}

std::wstring JoinTest(std::wstring_view base, std::wstring_view relative) {
  std::wstring result(base);
  if (!result.empty() && result.back() != L'\\') {
    result.push_back(L'\\');
  }
  result.append(relative);
  return result;
}

// 名单内的文件名常量是 ASCII 字节，拼路径时逐字符提升（与生产代码同一做法）。
std::wstring JoinTest(std::wstring_view base, std::string_view asciiRelative) {
  return JoinTest(base, WideName(asciiRelative));
}

bool WriteTestFile(std::wstring_view path, std::string_view bytes, DWORD shareMode = 0) {
  gc::platform::UniqueHandle handle(
      ::CreateFileW(std::wstring(path).c_str(), GENERIC_WRITE, shareMode, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!handle) {
    return false;
  }
  DWORD written = 0;
  return ::WriteFile(handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                     nullptr) != 0 &&
         written == static_cast<DWORD>(bytes.size());
}

std::string ReadTestFile(std::wstring_view path) {
  gc::platform::UniqueHandle handle(
      ::CreateFileW(std::wstring(path).c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!handle) {
    return {};
  }
  std::string bytes;
  char buffer[512];
  for (;;) {
    DWORD got = 0;
    if (::ReadFile(handle.get(), buffer, static_cast<DWORD>(sizeof(buffer)), &got, nullptr) == 0) {
      return {};
    }
    if (got == 0) {
      return bytes;
    }
    bytes.append(buffer, got);
  }
}

bool CreateTestDirectory(std::wstring_view path) {
  return ::CreateDirectoryW(std::wstring(path).c_str(), nullptr) != 0;
}

bool PathExists(std::wstring_view path) {
  return ::GetFileAttributesW(std::wstring(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 把一个目录的“最后写入时间”推到阈值之前，制造出“上次会话留下来的残骸”这一形态。
// 只用 FILE_WRITE_ATTRIBUTES：不改内容、不改其他时间戳，作用范围就是这个测试自己的目录。
bool MarkOlderThanThreshold(std::wstring_view directory) {
  gc::platform::UniqueHandle handle(
      ::CreateFileW(std::wstring(directory).c_str(), FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!handle) {
    return false;
  }
  FILETIME now{};
  ::GetSystemTimeAsFileTime(&now);
  const std::uint64_t back =
      ((static_cast<std::uint64_t>(now.dwHighDateTime) << 32u) | now.dwLowDateTime) -
      gc::platform::kStaleOperationDirectoryAgeTicks - kMinuteTicks;  // 阈值之外再多留一分钟
  FILETIME stale{};
  stale.dwLowDateTime = static_cast<DWORD>(back & 0xFFFFFFFFu);
  stale.dwHighDateTime = static_cast<DWORD>(back >> 32u);
  return ::SetFileTime(handle.get(), nullptr, nullptr, &stale) != 0;
}

// 独占持有目录里的一个文件：共享模式 0，与执行器的租约一模一样。
// 句柄一放手（作用域结束或进程结束）系统就收回它，于是“能独占打开”正是“前主人已经不在了”的证据。
gc::platform::UniqueHandle HoldFileExclusively(std::wstring_view path) {
  return gc::platform::UniqueHandle(
      ::CreateFileW(std::wstring(path).c_str(), GENERIC_WRITE, /*dwShareMode=*/0, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}

// 辅助进程持有开始标记的形态：可读、可标记删除（观察端要读它、回收端要能删它），
// 但别人用“0 访问权 + 共享 0”探测它时必定撞墙。
gc::platform::UniqueHandle HoldStartMarker(std::wstring_view path) {
  return gc::platform::UniqueHandle(::CreateFileW(
      std::wstring(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}

// 只改本进程的 TEMP/TMP，用例结束还原；不碰系统设置，也不碰注册表。
class TempRootOverride {
public:
  TempRootOverride() {
    Save(L"TEMP");
    Save(L"TMP");
  }
  ~TempRootOverride() {
    for (const auto& entry : saved_) {
      static_cast<void>(::SetEnvironmentVariableW(entry.first.c_str(), entry.second.c_str()));
    }
  }
  void Apply(std::wstring_view value) {
    static_cast<void>(::SetEnvironmentVariableW(L"TEMP", std::wstring(value).c_str()));
    static_cast<void>(::SetEnvironmentVariableW(L"TMP", std::wstring(value).c_str()));
  }

private:
  void Save(std::wstring_view name) {
    std::wstring buffer(4096, L'\0');
    const DWORD length = ::GetEnvironmentVariableW(std::wstring(name).c_str(), buffer.data(),
                                                   static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
      buffer.resize(length);
    } else {
      buffer.clear();
    }
    saved_.emplace_back(std::wstring(name), std::move(buffer));
  }
  std::vector<std::pair<std::wstring, std::wstring>> saved_;
};

// 测试自己造一个“执行器会用的目录名”：进程号 + 每次不同的随机段。
std::wstring MakeToken(unsigned long pid, std::string_view randomSegment) {
  return std::wstring(gc::git::kOperationDirectoryPrefix) + std::to_wstring(pid) + L"x" +
         WideName(randomSegment);
}

// 测试报告用 UTF-8：宽字符的失败原因与目录名都先转成字节再拼进断言消息。
std::string DescribeFailure(const std::wstring& failure) {
  return gc::platform::Utf16ToUtf8(failure);
}

}  // namespace

GC_TEST(directory_stale_threshold_reference_values) {
  // 独立参考值：100ns 刻度下，1 秒是 10'000'000 个刻度，1 分钟是 60 秒。
  GC_CHECK(gc::platform::kFileTimeTicksPerSecond == 10'000'000ULL);
  GC_CHECK(gc::platform::FileTimeTicksFromSeconds(1ULL) == 10'000'000ULL);
  GC_CHECK(gc::platform::FileTimeTicksFromSeconds(3ULL) == 30'000'000ULL);
  GC_CHECK(gc::platform::FileTimeTicksFromMinutes(1ULL) == 60ULL * 10'000'000ULL);

  // 本任务钉死的那一件事：回收阈值是 60 分钟，也就是 3600 秒，一个刻度都不能少。
  GC_CHECK(gc::platform::FileTimeTicksFromMinutes(60ULL) == 3'600ULL * 10'000'000ULL);
  GC_CHECK(gc::platform::FileTimeTicksFromMinutes(60ULL) == 36'000'000'000ULL);
  GC_CHECK(gc::platform::kStaleOperationDirectoryAgeTicks ==
           gc::platform::FileTimeTicksFromMinutes(60ULL));
  GC_CHECK(gc::platform::kStaleOperationDirectoryAgeTicks /
               gc::platform::kFileTimeTicksPerSecond ==
           3600ULL);  // 60 分钟 == 3600 秒

  // 旧实现里的写法 60 * 60 * 10000 在 100ns 单位下是 36'000'000 个刻度，也就是 3.6 秒：
  // 比阈值小一千倍，等于“操作刚开始就在回收”。这条断言把那个数量级钉在历史上，
  // 防止又有人手工乘一次乘法却把单位记错。
  constexpr std::uint64_t kOldBuggyLiteral = 60ULL * 60ULL * 10000ULL;
  GC_CHECK(kOldBuggyLiteral == 36'000'000ULL);
  GC_CHECK(kOldBuggyLiteral * 10ULL == 36ULL * gc::platform::kFileTimeTicksPerSecond);  // 3.6 秒
  GC_CHECK(kOldBuggyLiteral < gc::platform::FileTimeTicksFromMinutes(1ULL));           // 连一分钟都不到
  GC_CHECK(gc::platform::kStaleOperationDirectoryAgeTicks == kOldBuggyLiteral * 1000ULL);
}

GC_TEST(directory_stale_verdict_is_conservative) {
  using gc::platform::FileTimeTicksFromMinutes;
  using gc::platform::IsOperationDirectoryOldEnoughToReclaim;
  using gc::platform::kStaleOperationDirectoryAgeTicks;

  const std::uint64_t now = 1'600'000'000ULL * 10'000'000ULL;  // 任意的“现在”
  const std::uint64_t age = kStaleOperationDirectoryAgeTicks;

  GC_CHECK(IsOperationDirectoryOldEnoughToReclaim(now - age, now, age));          // 正好到阈值
  GC_CHECK(IsOperationDirectoryOldEnoughToReclaim(now - age - 1ULL, now, age));   // 再老一点
  GC_CHECK(!IsOperationDirectoryOldEnoughToReclaim(now - age + 1ULL, now, age));  // 差一个刻度也不行
  GC_CHECK(!IsOperationDirectoryOldEnoughToReclaim(now - 59ULL * kMinuteTicks, now, age));
  GC_CHECK(IsOperationDirectoryOldEnoughToReclaim(now - 61ULL * kMinuteTicks, now, age));

  // 时钟不可信时一律不回收：时间戳缺失、正好等于当前、来自未来（时钟被回拨过或改过）。
  GC_CHECK(!IsOperationDirectoryOldEnoughToReclaim(0ULL, now, age));
  GC_CHECK(!IsOperationDirectoryOldEnoughToReclaim(now, 0ULL, age));
  GC_CHECK(!IsOperationDirectoryOldEnoughToReclaim(0ULL, 0ULL, age));
  GC_CHECK(!IsOperationDirectoryOldEnoughToReclaim(now, now, age));
  GC_CHECK(!IsOperationDirectoryOldEnoughToReclaim(now + age, now, age));
  GC_CHECK(!IsOperationDirectoryOldEnoughToReclaim(now + 1ULL, now, age));

  // 阈值本身不受换算写法影响：60 分钟永远等于 3600 秒。
  GC_CHECK(FileTimeTicksFromMinutes(60ULL) == 36'000'000'000ULL);
}

GC_TEST(directory_claim_requires_atomic_create) {
  TempDirectory scratch;
  std::string reason;
  GC_REQUIRE_MESSAGE(scratch.Create(L"gc-claim", reason), reason);
  const std::wstring root = scratch.Path();
  const unsigned long pid = ::GetCurrentProcessId();

  // ① 亲手新建成功才拿到所有权：目录与租约都归本次认领，租约句柄在调用方手里。
  gc::platform::UniqueHandle lease;
  std::wstring claimedPath;
  std::wstring failure;
  const std::wstring token = MakeToken(pid, "abcd1234ef567890");
  GC_REQUIRE_MESSAGE(gc::platform::ClaimOperationDirectory(root, token, &lease, &claimedPath, &failure),
                     DescribeFailure(failure));
  GC_CHECK(claimedPath == JoinTest(root, token));
  GC_CHECK(bool(lease));
  GC_CHECK(PathExists(JoinTest(claimedPath, gc::git::kLeaseFileName)));
  // 认领只做“目录 + 租约”这一件事：说明书、标记、结果都还没写，也不该存在。
  for (std::string_view name : {std::string_view(gc::git::kSpecFileName),
                                std::string_view(gc::git::kStartMarkerFileName),
                                std::string_view(gc::git::kResultFileName)}) {
    GC_CHECK(!PathExists(JoinTest(claimedPath, name)));
  }

  // ② 租约把“这个目录还在被使用”暴露给别的开启者：以“只读 + 共享 0”再开一次必定撞共享冲突
  //    （0 访问权或纯属性查询都探不出来：系统对这类访问不做共享判定 —— 实测踩过，探测会静默失效）；
  //    同时目录里的其他文件照样能建能写（否则辅助进程就没法工作），目录本身也删不掉。
  gc::platform::UniqueHandle probe(::CreateFileW(
      JoinTest(claimedPath, gc::git::kLeaseFileName).c_str(), GENERIC_READ, 0, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  GC_CHECK_MESSAGE(probe.get() == INVALID_HANDLE_VALUE &&
                       ::GetLastError() == ERROR_SHARING_VIOLATION,
                   "租约句柄没能被别的独占打开查到（错误码 " + std::to_string(::GetLastError()) + "）");
  const std::wstring probePath = JoinTest(claimedPath, L"probe.txt");
  GC_CHECK(WriteTestFile(probePath, "hello"));  // 目录里的文件不受租约影响
  static_cast<void>(::DeleteFileW(probePath.c_str()));
  GC_CHECK(::RemoveDirectoryW(claimedPath.c_str()) == 0);  // 还有内容，删不掉

  // ③ 同名第二次认领：失败，且不覆盖、不复用、不往里面写任何东西。
  const std::string planted = "result\toldoldoldold0\t0\r\n";
  const std::wstring resultPath = JoinTest(claimedPath, gc::git::kResultFileName);
  GC_REQUIRE(WriteTestFile(resultPath, planted), "预置旧结果失败");
  gc::platform::UniqueHandle secondLease;
  std::wstring secondPath;
  failure.clear();
  GC_CHECK(!gc::platform::ClaimOperationDirectory(root, token, &secondLease, &secondPath, &failure));
  GC_CHECK(secondPath.empty());
  GC_CHECK(!bool(secondLease));
  GC_CHECK(!failure.empty());
  GC_CHECK(ReadTestFile(resultPath) == planted);  // 旧内容原样留着：没被改写，也没被当成自己的

  // ④ 放手租约后，独占探测就能成功，租约文件也才读得出来（持有期间它谁都不共享）：
  //    这就是“前主人已经不在了”的可查证据。
  lease.Reset();
  gc::platform::UniqueHandle reopened(::CreateFileW(
      JoinTest(claimedPath, gc::git::kLeaseFileName).c_str(), GENERIC_READ, 0, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  GC_CHECK_MESSAGE(reopened.get() != INVALID_HANDLE_VALUE,
                   "放手租约后目录仍不可独占探测（错误码 " + std::to_string(::GetLastError()) + "）");
  reopened.Reset();  // 探测句柄本身也是“共享 0”的持有者：不先关掉，下面就读不到内容
  GC_CHECK(ReadTestFile(JoinTest(claimedPath, gc::git::kLeaseFileName)) ==
           "lease\t" + DescribeFailure(token) + "\r\n");

  // ⑤ 形态不合格的名字一律不认领，也不在临时根里留下任何目录。
  for (const std::wstring& bad : {MakeToken(pid, "short"), MakeToken(pid, "XYZ"),
                                  std::wstring(L"not-a-directory-token"),
                                  std::wstring(L"..\\evil"), std::wstring(L"GcOp1x1")}) {
    std::wstring badPath;
    gc::platform::UniqueHandle badLease;
    failure.clear();
    GC_CHECK_MESSAGE(!gc::platform::ClaimOperationDirectory(root, bad, &badLease, &badPath, &failure),
                     "不合格的操作目录名竟然被认领了：" + DescribeFailure(bad));
    GC_CHECK(badPath.empty());
    GC_CHECK(!bool(badLease));
  }
}

GC_TEST(directory_sweep_keeps_live_and_unknown_and_removes_only_stale) {
  TempDirectory scratch;
  std::string reason;
  GC_REQUIRE_MESSAGE(scratch.Create(L"gc-sweep", reason), reason);
  const std::wstring root = scratch.Path();
  const unsigned long pid = ::GetCurrentProcessId();

  TempRootOverride tempOverride;
  tempOverride.Apply(root);  // 只改本进程的环境变量：回收例程因此只看这个隔离根

  // ① 真正陈旧的目录：超过阈值、名单内的文件无人持有 → 回收（文件与目录一起消失）。
  const std::wstring stale = JoinTest(root, MakeToken(pid, "aaaa1111bbbb2222"));
  GC_REQUIRE(CreateTestDirectory(stale), "造陈旧目录失败");
  GC_REQUIRE(WriteTestFile(JoinTest(stale, gc::git::kLeaseFileName), "lease\r\n"), "造租约文件失败");
  GC_REQUIRE(WriteTestFile(JoinTest(stale, gc::git::kSpecFileName), "evernight\r\n"), "造说明书失败");
  GC_REQUIRE(WriteTestFile(JoinTest(stale, gc::git::kStartMarkerFileName), "start\r\n"), "造标记失败");
  GC_REQUIRE(WriteTestFile(JoinTest(stale, gc::git::kResultFileName), "result\told\t0\r\n"),
             "造结果文件失败");
  GC_REQUIRE(MarkOlderThanThreshold(stale), "把陈旧目录的时间推到过去失败");

  // ② 同一次操作还在进行中：辅助进程像生产代码那样持有开始标记（可读、可标记删除）。
  //    目录即使早就过了阈值也不能动 —— 这一条同时覆盖“GUI 已退出但命令窗口还在跑”与“凭据输入等待”。
  const std::wstring live = JoinTest(root, MakeToken(pid, "cccc3333dddd4444"));
  GC_REQUIRE(CreateTestDirectory(live), "造活动目录失败");
  const std::wstring liveSpec = JoinTest(live, gc::git::kSpecFileName);
  const std::wstring liveStart = JoinTest(live, gc::git::kStartMarkerFileName);
  GC_REQUIRE(WriteTestFile(liveSpec, "evernight\r\n"), "造活动说明书失败");
  GC_REQUIRE(WriteTestFile(liveStart, "start\tsomenonce\r\n", FILE_SHARE_READ | FILE_SHARE_DELETE),
             "造活动标记失败");
  GC_REQUIRE(MarkOlderThanThreshold(live), "把活动目录的时间推到过去失败");
  auto markerHold = HoldStartMarker(liveStart);
  GC_REQUIRE_MESSAGE(markerHold, "模拟辅助进程持有开始标记失败");

  // ③ 别的实例的租约还握着：同样必须保留（跨实例、进程 ID 复用都不误删）。
  const std::wstring leased = JoinTest(root, MakeToken(pid, "eeee5555ffff6666"));
  GC_REQUIRE(CreateTestDirectory(leased), "造租约目录失败");
  const std::wstring leasedLease = JoinTest(leased, gc::git::kLeaseFileName);
  GC_REQUIRE(WriteTestFile(leasedLease, "lease\r\n"), "造租约文件失败");
  GC_REQUIRE(WriteTestFile(JoinTest(leased, gc::git::kSpecFileName), "evernight\r\n"),
             "造租约说明书失败");
  GC_REQUIRE(MarkOlderThanThreshold(leased), "把租约目录的时间推到过去失败");
  auto leaseHold = HoldFileExclusively(leasedLease);
  GC_REQUIRE_MESSAGE(leaseHold, "模拟另一个实例持有租约失败");

  // ④ 目录里有名单之外的内容：名单内的可以删，别人的东西一个都不碰，目录因此留着。
  const std::wstring foreign = JoinTest(root, MakeToken(pid, "7777aaaa8888bbbb"));
  GC_REQUIRE(CreateTestDirectory(foreign), "造混内容目录失败");
  GC_REQUIRE(WriteTestFile(JoinTest(foreign, gc::git::kSpecFileName), "evernight\r\n"), "造文件失败");
  GC_REQUIRE(WriteTestFile(JoinTest(foreign, gc::git::kStartMarkerFileName), "start\r\n"), "造文件失败");
  const std::wstring foreignFile = JoinTest(foreign, L"别人放的文件.txt");
  GC_REQUIRE(WriteTestFile(foreignFile, "不许动"), "造名单之外的文件失败");
  GC_REQUIRE(MarkOlderThanThreshold(foreign), "把混内容目录的时间推到过去失败");

  // ⑤ 删除被拒绝（只读属性）：目录保留并记一笔，绝不因为“删不掉就递归清空”。
  const std::wstring denied = JoinTest(root, MakeToken(pid, "9999ccccddddeeee"));
  GC_REQUIRE(CreateTestDirectory(denied), "造只读场景目录失败");
  GC_REQUIRE(WriteTestFile(JoinTest(denied, gc::git::kSpecFileName), "evernight\r\n"), "造文件失败");
  GC_REQUIRE(WriteTestFile(JoinTest(denied, gc::git::kStartMarkerFileName), "start\r\n"), "造文件失败");
  const std::wstring deniedResult = JoinTest(denied, gc::git::kResultFileName);
  GC_REQUIRE(WriteTestFile(deniedResult, "result\told\t0\r\n"), "造文件失败");
  GC_REQUIRE(::SetFileAttributesW(deniedResult.c_str(), FILE_ATTRIBUTE_READONLY) != 0, "设只读失败");
  GC_REQUIRE(MarkOlderThanThreshold(denied), "把只读场景目录的时间推到过去失败");

  // ⑥ 名字根本不像本程序造的：连看都不看里面有什么。
  const std::wstring stranger = JoinTest(root, L"GcOpNotARealForm");
  GC_REQUIRE(CreateTestDirectory(stranger), "造陌生名字目录失败");
  GC_REQUIRE(WriteTestFile(JoinTest(stranger, gc::git::kSpecFileName), "evernight\r\n"), "造文件失败");
  GC_REQUIRE(MarkOlderThanThreshold(stranger), "把陌生目录的时间推到过去失败");

  // ⑦ 新目录（没过阈值）：里面全是名单内的文件也不能动 —— “老”只是资格，不是结论。
  const std::wstring fresh = JoinTest(root, MakeToken(pid, "1111222233334444"));
  GC_REQUIRE(CreateTestDirectory(fresh), "造新目录失败");
  GC_REQUIRE(WriteTestFile(JoinTest(fresh, gc::git::kSpecFileName), "evernight\r\n"), "造文件失败");

  CommandWindowRunner runner;
  runner.Startup(nullptr);  // Startup 自己就会扫一遍：这里走真实入口
  const std::vector<std::wstring> preservedFirst = runner.PreservedOperationDirectories();
  bool mentionsInUse = false;
  bool mentionsKept = false;
  const auto scanPreserved = [&mentionsInUse, &mentionsKept](const std::vector<std::wstring>& entries) {
    for (const std::wstring& entry : entries) {
      if (entry.find(WideName(gc::git::kStartMarkerFileName)) != std::wstring::npos ||
          entry.find(WideName(gc::git::kLeaseFileName)) != std::wstring::npos) {
        mentionsInUse = true;
      }
      if (entry.find(L"保留") != std::wstring::npos) {
        mentionsKept = true;
      }
    }
  };
  scanPreserved(preservedFirst);
  GC_CHECK_MESSAGE(mentionsInUse, "第一次清扫（Startup）没把“还在被使用”这件事记下来");
  GC_CHECK_MESSAGE(mentionsKept, "第一次清扫（Startup）保留目录时没留下任何原因");

  mentionsInUse = false;
  mentionsKept = false;
  runner.SweepStaleOperationDirectories();  // 再扫一次：反复回收不会把该留的东西磨掉
  scanPreserved(runner.PreservedOperationDirectories());
  GC_CHECK_MESSAGE(mentionsInUse && mentionsKept, "第二次清扫把保留原因弄丢了");

  GC_CHECK(!PathExists(stale));
  GC_CHECK(PathExists(live));
  GC_CHECK(PathExists(liveSpec));  // 仍可能被读取的输入文件原样留着
  GC_CHECK(PathExists(liveStart));
  GC_CHECK(PathExists(leased));
  GC_CHECK(PathExists(JoinTest(leased, gc::git::kSpecFileName)));
  GC_CHECK(PathExists(foreign));
  GC_CHECK(ReadTestFile(foreignFile) == "不许动");  // 名单之外的内容一个字没动
  GC_CHECK(PathExists(denied));
  GC_CHECK(PathExists(deniedResult));
  GC_CHECK(PathExists(stranger));
  GC_CHECK(PathExists(JoinTest(stranger, gc::git::kSpecFileName)));
  GC_CHECK(PathExists(fresh));
  GC_CHECK(PathExists(JoinTest(fresh, gc::git::kSpecFileName)));

  // 每一条保留都要有迹可循，而不是“静默没删”（上面两次清扫已经检查过记录内容）。

  // 放手之后再看：同一批目录这次就该被回收 —— 证明之前保留只是因为“还有人用”，不是永远不动。
  // 注意：删掉目录里的文件会把目录自己的最后写入时间刷新成“现在”，所以一轮没删干净的目录
  // 要再等一个阈值周期 —— 这里把它重新推回过去，是为了检查“无人持有之后确实能删干净”这件事，
  // 不是靠放开来放宽生产规则。
  markerHold.Reset();
  leaseHold.Reset();
  GC_CHECK(::SetFileAttributesW(deniedResult.c_str(), FILE_ATTRIBUTE_NORMAL) != 0, "还原只读失败");
  static_cast<void>(MarkOlderThanThreshold(live));
  static_cast<void>(MarkOlderThanThreshold(leased));
  static_cast<void>(MarkOlderThanThreshold(denied));
  runner.SweepStaleOperationDirectories();
  GC_CHECK(!PathExists(live));
  GC_CHECK(!PathExists(leased));
  GC_CHECK(!PathExists(denied));
  GC_CHECK(PathExists(foreign));  // 名单之外的内容仍在：目录归它真正的主人
  GC_CHECK(PathExists(foreignFile));
  GC_CHECK(PathExists(stranger));
  GC_CHECK(PathExists(fresh));
  runner.Shutdown();
}

GC_TEST(directory_sweep_does_not_follow_junctions) {
  // 回收与认领都不许顺着链接走到目录之外：在隔离临时根里造一个指向另一个目录的联接，
  // 里面放一个“重要文件”，然后跑一次启动清扫。唯一允许的结局是：链接指向的内容一个字都不少。
  TempDirectory scratch;
  std::string reason;
  GC_REQUIRE_MESSAGE(scratch.Create(L"gc-link", reason), reason);
  const std::wstring root = scratch.Path();
  const std::wstring target = JoinTest(root, L"联接指向的地方");
  GC_REQUIRE(CreateTestDirectory(target), "造联接目标失败");
  const std::wstring important = JoinTest(target, L"重要文件.txt");
  GC_REQUIRE(WriteTestFile(important, "一点都不能少"), "造重要文件失败");

  const std::wstring link = JoinTest(root, MakeToken(::GetCurrentProcessId(), "abcdabcdabcdabcd"));
  std::wstring cmdLine = L"cmd.exe /D /C mklink /J \"" + link + L"\" \"" + target + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION information{};
  const BOOL launched = ::CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                                         CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information);
  GC_REQUIRE_MESSAGE(launched != 0,
                     "本用例需要一个目录联接，但 cmd.exe 起不来（错误码 " +
                         std::to_string(::GetLastError()) + "）");
  gc::platform::UniqueHandle process(information.hProcess);
  gc::platform::UniqueHandle thread(information.hThread);
  static_cast<void>(::WaitForSingleObject(process.get(), 15000));
  GC_REQUIRE_MESSAGE(PathExists(link), "目录联接没能创建：环境不允许 mklink /J");

  static_cast<void>(MarkOlderThanThreshold(link));  // 能推多远算多远：推不动也只是这条不参与
  TempRootOverride tempOverride;
  tempOverride.Apply(root);
  CommandWindowRunner runner;
  runner.Startup(nullptr);
  runner.SweepStaleOperationDirectories();
  GC_CHECK(ReadTestFile(important) == "一点都不能少");  // 绝不越界删到链接外面
  GC_CHECK(PathExists(target));
  runner.Shutdown();
  static_cast<void>(::RemoveDirectoryW(link.c_str()));  // 只摘链接本身，不碰它指向的内容
}

GC_TEST(directory_observer_rejects_stale_and_partial_results_from_disk) {
  // 把生产用的观察器接到真实文件上：上一次留下的“成功”、写了一半的结果，都不许当成
  // 本次操作的回答；只有口令对得上且完整发布过的那一条才算。
  TempDirectory scratch;
  std::string reason;
  GC_REQUIRE_MESSAGE(scratch.Create(L"gc-observe", reason), reason);
  const std::wstring directory = scratch.Path();
  static constexpr std::string_view kOurNonce = "ourourour1234567";
  static constexpr std::string_view kForeignNonce = "oldoldold9876543";

  const auto reader = [&directory](std::string_view name) -> std::optional<std::string> {
    const std::string bytes = ReadTestFile(JoinTest(directory, name));
    if (bytes.empty()) {
      return std::nullopt;
    }
    return bytes;
  };

  long exitCode = -1;
  gc::git::CommandWindowObservation facts =
      gc::git::ObserveCommandWindow(reader, true, false, kOurNonce);
  GC_CHECK(!facts.startMarkerSeen && !facts.resultParsed);
  GC_CHECK(gc::git::DecideCommandCompletion(facts, &exitCode) ==
           gc::git::CommandCompletion::launched);

  // 陈旧的一条“成功”：格式完全正确，只是口令属于上一次的操作。
  GC_REQUIRE(WriteTestFile(JoinTest(directory, gc::git::kStartMarkerFileName),
                           "start\t" + std::string(kForeignNonce) + "\r\n"),
             "写陈旧标记失败");
  GC_REQUIRE(WriteTestFile(JoinTest(directory, gc::git::kResultFileName),
                           "result\t" + std::string(kForeignNonce) + "\t0\r\n"),
             "写陈旧结果失败");
  facts = gc::git::ObserveCommandWindow(reader, true, true, kOurNonce);
  GC_CHECK(!facts.startMarkerSeen);
  GC_CHECK(!facts.resultParsed);
  GC_CHECK(gc::git::DecideCommandCompletion(facts, &exitCode) ==
           gc::git::CommandCompletion::helperNeverStarted);  // 宁可不认，也不认别人的成功

  // 半写：本次口令，但行尾还没落下（发布协议里改名之前就是这个样子）。
  GC_REQUIRE(WriteTestFile(JoinTest(directory, gc::git::kStartMarkerFileName),
                           "start\t" + std::string(kOurNonce) + "\r\n"),
             "写标记失败");
  GC_REQUIRE(WriteTestFile(JoinTest(directory, gc::git::kResultFileName),
                           "result\t" + std::string(kOurNonce) + "\t0"),
             "写半截结果失败");
  facts = gc::git::ObserveCommandWindow(reader, true, true, kOurNonce);
  GC_CHECK(facts.startMarkerSeen);
  GC_CHECK(!facts.resultParsed);
  GC_CHECK(gc::git::DecideCommandCompletion(facts, &exitCode) ==
           gc::git::CommandCompletion::terminated);  // 窗口已退、结果没写完：只能算结果未知

  // 完整发布 + 本次口令：这时候才允许判定完成，退出码也才可用。
  GC_REQUIRE(WriteTestFile(JoinTest(directory, gc::git::kResultFileName),
                           "result\t" + std::string(kOurNonce) + "\t128\r\n"),
             "写完整结果失败");
  facts = gc::git::ObserveCommandWindow(reader, true, true, kOurNonce);
  GC_CHECK(facts.resultParsed && facts.exitCode == 128);
  GC_CHECK(gc::git::DecideCommandCompletion(facts, &exitCode) ==
           gc::git::CommandCompletion::finished);
  GC_CHECK(exitCode == 128);
}
