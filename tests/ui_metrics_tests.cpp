#include "support/tiny_test.h"

#include <windows.h>

#include "ui/ui_metrics.h"

GC_TEST(ui_metrics_scale_is_proportional_to_dpi) {
  gc::ui::UiMetrics metrics;
  metrics.UpdateForDpi(96);
  GC_CHECK(metrics.Dpi() == 96u);
  GC_CHECK(metrics.Scale(100) == 100);
  GC_CHECK(metrics.Margin() > 0);
  const int baseControl = metrics.ControlHeight();

  metrics.UpdateForDpi(144);  // 150%
  GC_CHECK(metrics.Scale(100) == 150);
  GC_CHECK(metrics.ControlHeight() * 2 == baseControl * 3);
  GC_CHECK(metrics.SplitterWidth() > 0);

  metrics.UpdateForDpi(192);  // 200%
  GC_CHECK(metrics.Scale(100) == 200);
  GC_CHECK(metrics.Margin() > 0);
}

GC_TEST(ui_metrics_font_and_text_measurements_stay_usable) {
  gc::ui::UiMetrics metrics;
  metrics.UpdateForDpi(96);
  GC_CHECK(metrics.Font() != nullptr);
  const int narrow = metrics.LabelWidth(L"提交");
  const int wide = metrics.LabelWidth(L"提交提交提交");
  GC_CHECK(narrow > 0);
  GC_CHECK(wide > narrow);
  GC_CHECK(metrics.ButtonWidth(L"浏览…") > narrow);
  GC_CHECK(metrics.LabelHeight() > 0);

  metrics.UpdateForDpi(192);
  GC_CHECK(metrics.Font() != nullptr);
  GC_CHECK(metrics.LabelWidth(L"提交") > narrow);
}

GC_TEST(ui_metrics_recovers_from_zero_dpi) {
  gc::ui::UiMetrics metrics;
  metrics.UpdateForDpi(0);
  GC_CHECK(metrics.Dpi() == 96u);
  GC_CHECK(metrics.Scale(50) == 50);
}
