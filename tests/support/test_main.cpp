#include "support/tiny_test.h"

#include <cstdio>
#include <string>
#include <vector>

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

int RunAll() {
  int failedCases = 0;
  for (const RegisteredCase& item : Cases()) {
    CurrentCaseFailures() = 0;
    std::fprintf(stdout, "[ 运行 ] %s\n", item.name.c_str());
    item.body();
    if (CurrentCaseFailures() == 0) {
      std::fprintf(stdout, "[ 通过 ] %s\n", item.name.c_str());
    } else {
      ++failedCases;
      std::fprintf(stdout, "[ 失败 ] %s（%d 处断言失败）\n", item.name.c_str(), CurrentCaseFailures());
    }
  }
  std::fprintf(stdout, "\n共 %zu 个用例，失败 %d 个，断言失败 %d 处。\n", Cases().size(), failedCases, Failures());
  return failedCases == 0 ? 0 : 1;
}

}  // namespace gc::test

int main() { return gc::test::RunAll(); }
