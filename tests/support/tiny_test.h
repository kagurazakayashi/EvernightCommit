#pragma once

#include <functional>
#include <stdexcept>
#include <string>

namespace gc::test {

using CaseBody = void (*)();

struct Location {
  const char* file;
  int line;
};

// 记录一次断言结果，返回是否通过。失败不中断当前用例，便于一次看到多处问题。
bool Report(bool passed, const std::string& description, Location location);

int FailureCount();

// 前置条件失败：环境不满足（找不到可用 Git、临时目录创建失败、夹具命令意外非 0 退出等）。
// 抛出即中止当前用例；运行器把它与“断言失败”分开报告，但同样计入失败、返回非 0 退出码，
// 绝不静默跳过集成测试。
struct PrerequisiteFailure : std::runtime_error {
  explicit PrerequisiteFailure(const std::string& reason) : std::runtime_error(reason) {}
};

struct Registrar {
  Registrar(const char* name, CaseBody body, const char* file);
};

// 运行全部用例；nameFilter 非空时只运行用例名包含该片段的用例。
// groupFilter 非空时按验收分组（pure/windows/git-readonly/git-mutating/all）再筛一道；
// 解析与统计都在 RunAll 内部完成（见 test_main.cpp 的分组表）。
// 过滤器命中 0 个用例时返回 2，绝不把「一个都没跑」报成通过。
int RunAll(std::string_view nameFilter = {}, std::string_view groupFilter = {});

// 按分打印全部注册用例（group<TAB>name），供 --list 与外部工具生成名单；同样接受过滤。
int ListCases(std::string_view nameFilter = {}, std::string_view groupFilter = {});

}  // namespace gc::test

#define GC_TEST(name)                              \
  static void name();                              \
  static const ::gc::test::Registrar kRegistrar_##name(#name, &name, __FILE__); \
  static void name()

#define GC_CHECK(condition) \
  ::gc::test::Report(static_cast<bool>(condition), #condition, ::gc::test::Location{__FILE__, __LINE__})

#define GC_CHECK_MESSAGE(condition, description) \
  ::gc::test::Report(static_cast<bool>(condition), (description), ::gc::test::Location{__FILE__, __LINE__})

// 前置条件不满足时中止用例并给出原因（reason 为 std::string 可构造的表达式）。
#define GC_REQUIRE(condition, reason) \
  do { \
    if (!static_cast<bool>(condition)) { \
      ::gc::test::Report(false, "前置条件失败: " + static_cast<std::string>(reason), \
                         ::gc::test::Location{__FILE__, __LINE__}); \
      throw ::gc::test::PrerequisiteFailure(reason); \
    } \
  } while (false)

// 同 GC_REQUIRE，但失败信息用可读描述替代条件文本本身。
#define GC_REQUIRE_MESSAGE(condition, description) \
  do { \
    if (!static_cast<bool>(condition)) { \
      ::gc::test::Report(false, "前置条件失败: " + static_cast<std::string>(description), \
                         ::gc::test::Location{__FILE__, __LINE__}); \
      throw ::gc::test::PrerequisiteFailure(description); \
    } \
  } while (false)
