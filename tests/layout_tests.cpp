#include "support/tiny_test.h"

#include <algorithm>
#include <cmath>

#include "ui/layout.h"

namespace {

constexpr int Width(const RECT& rect) noexcept { return rect.right - rect.left; }
constexpr int Height(const RECT& rect) noexcept { return rect.bottom - rect.top; }

gc::ui::BandSpec MakeBandSpec() {
  gc::ui::BandSpec spec{};
  spec.repoBarHeight = 54;
  spec.infoRowHeight = 20;
  spec.actionRowHeight = 24;
  spec.gap = 6;
  spec.minChangesHeight = 100;
  spec.minFormHeight = 180;
  return spec;
}

gc::ui::ChangesSpec MakeChangesSpec() {
  gc::ui::ChangesSpec spec{};
  spec.splitterWidth = 7;
  spec.arrowColumnWidth = 118;
  spec.minListWidth = 168;
  spec.minArrowColumnWidth = 96;
  spec.gap = 6;
  return spec;
}

}  // namespace

GC_TEST(bands_cover_client_without_overlap) {
  const auto spec = MakeBandSpec();
  const RECT client{0, 0, 1100, 780};
  gc::ui::Bands bands{};

  GC_CHECK(gc::ui::ComputeBands(client, spec, &bands));
  GC_CHECK(bands.repoBar.bottom + spec.gap == bands.infoRow.top);
  GC_CHECK(bands.infoRow.bottom + spec.gap == bands.changes.top);
  GC_CHECK(bands.changes.bottom + spec.gap == bands.commitForm.top);
  GC_CHECK(bands.commitForm.bottom + spec.gap == bands.actions.top);
  GC_CHECK(bands.actions.bottom == client.bottom);
  GC_CHECK(Width(bands.repoBar) == Width(client));
  GC_CHECK(Width(bands.changes) == Width(client));

  const int sum = Height(bands.repoBar) + Height(bands.infoRow) + Height(bands.changes) +
                  Height(bands.commitForm) + Height(bands.actions) + 4 * spec.gap;
  GC_CHECK_MESSAGE(sum == Height(client), "横向带高度之和应等于客户区高度");
}

GC_TEST(bands_keep_minimums_and_grow_with_window) {
  const auto spec = MakeBandSpec();
  gc::ui::Bands small{};
  gc::ui::Bands large{};
  GC_CHECK(gc::ui::ComputeBands({0, 0, 1100, 620}, spec, &small));
  GC_CHECK(gc::ui::ComputeBands({0, 0, 1100, 1040}, spec, &large));

  GC_CHECK(Height(small.changes) >= spec.minChangesHeight);
  GC_CHECK(Height(small.commitForm) >= spec.minFormHeight);
  GC_CHECK(Height(large.changes) > Height(small.changes));
  GC_CHECK(Height(large.commitForm) >= Height(small.commitForm));
}

GC_TEST(bands_degrade_without_overlap_when_too_small) {
  const auto spec = MakeBandSpec();
  gc::ui::Bands bands{};
  const bool fits = gc::ui::ComputeBands({0, 0, 400, 120}, spec, &bands);
  GC_CHECK_MESSAGE(!fits, "空间不足时应报告无法容纳");
  GC_CHECK(bands.changes.bottom <= bands.commitForm.top);
  GC_CHECK(bands.commitForm.bottom <= bands.actions.top);
  GC_CHECK(Height(bands.changes) >= 0);
}

GC_TEST(changes_columns_are_ordered_and_fill_area) {
  const auto spec = MakeChangesSpec();
  const RECT area{0, 0, 1082, 400};
  gc::ui::ChangesColumns columns{};
  gc::ui::ComputeChangesColumns(area, spec, &columns);

  GC_CHECK(columns.unstaged.left == area.left);
  GC_CHECK(columns.unstaged.right <= columns.arrows.left);
  GC_CHECK(columns.arrows.right <= columns.splitterLeft.left);
  GC_CHECK(columns.splitterLeft.right <= columns.staged.left);
  GC_CHECK(columns.staged.right <= columns.splitterRight.left);
  GC_CHECK(columns.splitterRight.right <= columns.history.left);
  GC_CHECK(columns.history.right <= area.right);

  const int sum = Width(columns.unstaged) + Width(columns.arrows) + Width(columns.splitterLeft) +
                  Width(columns.staged) + Width(columns.splitterRight) + Width(columns.history) + 5 * spec.gap;
  GC_CHECK_MESSAGE(sum == Width(area), "三栏与分隔条宽度之和应铺满区域");

  GC_CHECK(Width(columns.unstaged) >= spec.minListWidth);
  GC_CHECK(Width(columns.staged) >= spec.minListWidth);
  GC_CHECK(Width(columns.history) >= spec.minListWidth);
  GC_CHECK(Height(columns.history) == Height(area));
}

GC_TEST(changes_columns_scale_width_with_window) {
  const auto spec = MakeChangesSpec();
  gc::ui::ChangesColumns small{};
  gc::ui::ChangesColumns large{};
  gc::ui::ComputeChangesColumns({0, 0, 900, 400}, spec, &small);
  gc::ui::ComputeChangesColumns({0, 0, 1400, 400}, spec, &large);
  GC_CHECK(Width(large.unstaged) > Width(small.unstaged));
  GC_CHECK(Width(large.staged) > Width(small.staged));
  GC_CHECK(Width(large.history) > Width(small.history));
}

GC_TEST(changes_columns_respect_dragged_ratios) {
  auto spec = MakeChangesSpec();
  spec.leftRatio = 0.60;
  spec.middleRatio = 0.20;
  const RECT area{0, 0, 1200, 400};
  gc::ui::ChangesColumns columns{};
  gc::ui::ComputeChangesColumns(area, spec, &columns);
  const int free = Width(area) - 2 * spec.splitterWidth - spec.arrowColumnWidth - 5 * spec.gap;
  const int expected = static_cast<int>(std::lround(0.60 * free));
  GC_CHECK_MESSAGE(std::abs(Width(columns.unstaged) - expected) <= 1, "未暂存列表宽度应按比例分配");
  GC_CHECK(Width(columns.unstaged) > Width(columns.staged));
}

GC_TEST(splitter_drag_clamps_to_min_widths) {
  const RECT area{0, 0, 1200, 400};
  auto spec = MakeChangesSpec();

  double left = spec.leftRatio;
  double middle = spec.middleRatio;
  gc::ui::RatioFromMouseX(area, spec, 0, -5000, &left, &middle);
  spec.leftRatio = left;
  gc::ui::ChangesColumns clamped{};
  gc::ui::ComputeChangesColumns(area, spec, &clamped);
  GC_CHECK(Width(clamped.unstaged) >= spec.minListWidth);
  GC_CHECK(Width(clamped.staged) >= spec.minListWidth);
  GC_CHECK(Width(clamped.history) >= spec.minListWidth);

  double reusedLeft = spec.leftRatio;
  double reusedMiddle = spec.middleRatio;
  gc::ui::RatioFromMouseX(area, spec, 1, 999999, &reusedLeft, &reusedMiddle);
  spec.middleRatio = reusedMiddle;
  gc::ui::ChangesColumns dragged{};
  gc::ui::ComputeChangesColumns(area, spec, &dragged);
  GC_CHECK(Width(dragged.unstaged) >= spec.minListWidth);
  GC_CHECK(Width(dragged.staged) >= spec.minListWidth);
  GC_CHECK(Width(dragged.history) >= spec.minListWidth);
  GC_CHECK_MESSAGE(std::abs(reusedLeft - left) < 1e-9, "拖动右侧分隔条不应改变左侧比例");
}

GC_TEST(minimum_client_size_is_below_initial_window) {
  const auto bands = MakeBandSpec();
  const auto changes = MakeChangesSpec();
  const gc::ui::MinimumClientSize minimum = gc::ui::MinimumClientFor(bands, changes);
  GC_CHECK(minimum.width > 0);
  GC_CHECK(minimum.height > 0);
  GC_CHECK(minimum.width < 1100);
  GC_CHECK(minimum.height < 780);
}
