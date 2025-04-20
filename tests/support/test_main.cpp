#include "support/tiny_test.h"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "platform/windows/command_window_helper.h"

namespace gc::test {
namespace {

struct RegisteredCase {
  std::string name;
  CaseBody body;
};

std::vector<RegisteredCase>& Cases() {
  static std::vector<RegisteredCase> cases;
  return cases;
}

int& Failures() {
  static int failures = 0;
  return failures;
}

int& CurrentCaseFailures() {
  static int failures = 0;
  return failures;
}

}  // namespace

bool Report(bool passed, const std::string& description, Location location) {
  if (passed) {
    return true;
  }
  ++Failures();
  ++CurrentCaseFailures();
  std::fprintf(stderr, "  断言失败: %s\n    位于 %s:%d\n", description.c_str(), location.file, location.line);
  return false;
}

int FailureCount() { return Failures(); }

Registrar::Registrar(const char* name, CaseBody body) { Cases().push_back(RegisteredCase{name, body}); }

int RunAll(std::string_view nameFilter) {
  int failedCases = 0;
  int prerequisiteFailures = 0;
  for (const RegisteredCase& item : Cases()) {
    // 可选的姓名片段过滤：诊断某个用例（尤其是弹真实 Git/命令窗口的集成用例）时不必全量重跑。
    if (!nameFilter.empty() && item.name.find(nameFilter) == std::string::npos) {
      continue;
    }
    CurrentCaseFailures() = 0;
    std::fprintf(stdout, "[ 运行 ] %s\n", item.name.c_str());
    std::string abortReason;
    try {
      item.body();
    } catch (const PrerequisiteFailure& failure) {
      // 夹具自身的析构（清理临时目录）在栈展开中照常执行。
      abortReason = failure.what();
    } catch (const std::exception& failure) {
      abortReason = std::string("用例抛出未预期异常: ") + failure.what();
    } catch (...) {
      abortReason = "用例抛出未知异常";
    }
    if (!abortReason.empty()) {
      ++prerequisiteFailures;
      ++failedCases;
      std::fprintf(stdout, "[ 前置失败 ] %s：%s\n", item.name.c_str(), abortReason.c_str());
      continue;
    }
    if (CurrentCaseFailures() == 0) {
      std::fprintf(stdout, "[ 通过 ] %s\n", item.name.c_str());
    } else {
      ++failedCases;
      std::fprintf(stdout, "[ 失败 ] %s（%d 处断言失败）\n", item.name.c_str(), CurrentCaseFailures());
    }
  }
  if (prerequisiteFailures != 0) {
    std::fprintf(stdout,
                 "\n注意：有 %d 个用例因前置条件失败而中止（Git 不可用或临时夹具创建失败等），"
                 "不是断言失败，但同样必须解决后才能视为测试通过。\n",
                 prerequisiteFailures);
  }
  std::fprintf(stdout, "\n共 %zu 个用例，失败 %d 个，断言失败 %d 处。\n", Cases().size(), failedCases,
               Failures());
  return failedCases == 0 ? 0 : 1;
}

}  // namespace gc::test

int main(int argc, char** argv) {
  // 命令窗口执行器启动的是「当前可执行文件」的辅助模式：测试二进制同样要认这个入口，
  // 集成用例才会走与正式程序完全相同的那条执行链路（新控制台、说明书、CreateProcessW）。
  int helperExitCode = 0;
  if (gc::platform::RunCommandWindowHelperIfRequested(&helperExitCode)) {
    return helperExitCode;
  }
  // 行缓冲：重定向到文件时也能即时看到进度，不会把最后几行憋在块缓冲区里。
  // MSVC 调试版 CRT 要求显式给出缓冲区大小（0 会触发断言对话框）。
  std::setvbuf(stdout, nullptr, _IOLBF, 512);
  const std::string_view filter = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
  return gc::test::RunAll(filter);
}
