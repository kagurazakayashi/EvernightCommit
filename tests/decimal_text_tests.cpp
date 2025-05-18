// app/decimal_text.h 的纯逻辑测试：带上下界的十进制解析必须「先判界再乘加」，
// 任何长度的输入都只会得到拒绝，绝不出现有符号溢出后的回绕值（这份数据来自可能
// 已损坏的本地记录文件，回绕成负数再被采信就是 R6 的原始缺陷）。
// 公开的解析入口（ParseOperationLog / ParsePersistentState）上的同类用例见
// operation_history_tests.cpp 与 persistent_state_tests.cpp。
#include <string>

#include "app/decimal_text.h"
#include "support/tiny_test.h"

namespace {

using gc::app::kDecimalLongLongMax;

}  // namespace

GC_TEST(decimal_text_non_negative_accepts_only_digits_in_range) {
  long long out = -1;
  // 0 与合法上界。
  GC_CHECK(gc::app::ParseNonNegativeDecimal(L"0", 0, &out) && out == 0);
  GC_CHECK(gc::app::ParseNonNegativeDecimal(L"999", kDecimalLongLongMax, &out) && out == 999);
  GC_CHECK(gc::app::ParseNonNegativeDecimal(L"3650", 3650, &out) && out == 3650);
  GC_CHECK(gc::app::ParseNonNegativeDecimal(L"1000", 1000, &out) && out == 1000);
  // 恰好等于 long long 上界：能表示，就不是坏数据。
  GC_CHECK(gc::app::ParseNonNegativeDecimal(L"9223372036854775807", kDecimalLongLongMax, &out) &&
           out == kDecimalLongLongMax);
  // 上界本身就拒绝。
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(L"3651", 3650, &out));
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(L"1", 0, &out));
}

GC_TEST(decimal_text_non_negative_rejects_overflows_and_junk) {
  long long out = -1;
  // R6 的 UBSan 复现值：19 个 9 曾经在这条路径上有符号溢出并返回 true（得到负数）。
  GC_CHECK_MESSAGE(!gc::app::ParseNonNegativeDecimal(L"9999999999999999999", kDecimalLongLongMax, &out),
                   "19 位全 9 必须拒绝，不能回绕");
  // 上界 +1 与更长的串同样拒绝（长度不设死上限，靠边界判定）。
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(L"9223372036854775808", kDecimalLongLongMax, &out));
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(std::wstring(60, L'9'), kDecimalLongLongMax, &out));
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(std::wstring(60, L'9'), 1000, &out));
  // 空、符号、空白、非数字、全角数字：都不是本程序写出来的数字。
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(L"", 1000, &out));
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(L"-1", 1000, &out));
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(L"+1", 1000, &out));
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(L" 1", 1000, &out));
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(L"1 ", 1000, &out));
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(L"1a", 1000, &out));
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(L"１２３", 1000, &out));
  // 输出指针为空、上界为负：不成立。
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(L"1", 1000, nullptr));
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(L"1", -1, &out));
  // 被拒绝时不得写出结果。
  out = 42;
  GC_CHECK(!gc::app::ParseNonNegativeDecimal(L"99999999999999999999", 1000, &out) && out == 42);
}

GC_TEST(decimal_text_signed_respects_both_bounds) {
  long long out = 0;
  GC_CHECK(gc::app::ParseSignedDecimal(L"0", -10, 10, &out) && out == 0);
  GC_CHECK(gc::app::ParseSignedDecimal(L"-10", -10, 10, &out) && out == -10);
  GC_CHECK(gc::app::ParseSignedDecimal(L"10", -10, 10, &out) && out == 10);
  // 「非负」区间不接受负号；「-0」是 0，落在区间里就成立。
  GC_CHECK(!gc::app::ParseSignedDecimal(L"-1", 0, 10, &out));
  GC_CHECK(gc::app::ParseSignedDecimal(L"-0", 0, 10, &out) && out == 0);
  // 界外与畸形。
  GC_CHECK(!gc::app::ParseSignedDecimal(L"11", -10, 10, &out));
  GC_CHECK(!gc::app::ParseSignedDecimal(L"-11", -10, 10, &out));
  GC_CHECK(!gc::app::ParseSignedDecimal(L"-", -10, 10, &out));
  GC_CHECK(!gc::app::ParseSignedDecimal(L"--1", -10, 10, &out));
  GC_CHECK(!gc::app::ParseSignedDecimal(L"1-", -10, 10, &out));
  GC_CHECK(!gc::app::ParseSignedDecimal(L"", -10, 10, &out));
  // 退出码那一档用满 long long 区间：19 个 9 依然拒绝，而不是溢出后放行。
  GC_CHECK(!gc::app::ParseSignedDecimal(L"9999999999999999999", -kDecimalLongLongMax,
                                        kDecimalLongLongMax, &out));
  GC_CHECK(gc::app::ParseSignedDecimal(L"9223372036854775807", -kDecimalLongLongMax,
                                       kDecimalLongLongMax, &out) &&
           out == kDecimalLongLongMax);
  GC_CHECK(gc::app::ParseSignedDecimal(L"-9223372036854775807", -kDecimalLongLongMax,
                                       kDecimalLongLongMax, &out) &&
           out == -kDecimalLongLongMax);
  // long long 下界不可表示（取负即溢出），这里按「不接受该下界参数」处理。
  GC_CHECK(!gc::app::ParseSignedDecimal(L"1", -kDecimalLongLongMax - 1, 10, &out));
  // 整个区间都在 0 之外：任何数字幅度都不成立。
  GC_CHECK(!gc::app::ParseSignedDecimal(L"5", -10, -1, &out));
}

GC_TEST(decimal_text_int32_narrows_only_after_range_check) {
  int out = 0;
  GC_CHECK(gc::app::ParseInt32Decimal(L"0", 0, 2147483647, &out) && out == 0);
  GC_CHECK(gc::app::ParseInt32Decimal(L"2147483647", 0, 2147483647, &out) && out == 2147483647);
  GC_CHECK(gc::app::ParseInt32Decimal(L"-2147483648", -2147483648LL, 2147483647LL, &out) &&
           out == -2147483648);
  GC_CHECK(!gc::app::ParseInt32Decimal(L"2147483648", 0, 2147483647, &out));
  GC_CHECK(!gc::app::ParseInt32Decimal(L"9999999999999999999", -2147483648LL, 2147483647LL, &out));
  // 参数给的区间本身宽于 int：那是调用方没核对范围，拒绝而不是截断。
  GC_CHECK(!gc::app::ParseInt32Decimal(L"5", 0, 4294967296LL, &out));
  GC_CHECK(!gc::app::ParseInt32Decimal(L"5", -4294967296LL, 4294967296LL, &out));
  GC_CHECK(!gc::app::ParseInt32Decimal(L"5", 0, 2147483647, nullptr));
}
