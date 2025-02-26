#pragma once

#include <functional>
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

struct Registrar {
  Registrar(const char* name, CaseBody body);
};

int RunAll();

}  // namespace gc::test

#define GC_TEST(name)                              \
  static void name();                              \
  static const ::gc::test::Registrar kRegistrar_##name(#name, &name); \
  static void name()

#define GC_CHECK(condition) \
  ::gc::test::Report(static_cast<bool>(condition), #condition, ::gc::test::Location{__FILE__, __LINE__})

#define GC_CHECK_MESSAGE(condition, description) \
  ::gc::test::Report(static_cast<bool>(condition), (description), ::gc::test::Location{__FILE__, __LINE__})
