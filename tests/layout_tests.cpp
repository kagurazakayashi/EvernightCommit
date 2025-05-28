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

GC_TEST(staging_columns_are_ordered_and_fill_area) {
  const auto spec = MakeChangesSpec();
  const RECT area{0, 0, 1082, 400};
  gc::ui::StagingColumns columns{};
  gc::ui::ComputeStagingColumns(area, spec, &columns);

  GC_CHECK(columns.unstaged.left == area.left);
  GC_CHECK(columns.unstaged.right <= columns.arrows.left);
  GC_CHECK(columns.arrows.right <= columns.splitter.left);
  GC_CHECK(columns.splitter.right <= columns.staged.left);
  GC_CHECK(columns.staged.right <= area.right);

  const int sum = Width(columns.unstaged) + Width(columns.arrows) + Width(columns.splitter) +
                  Width(columns.staged) + 3 * spec.gap;
  GC_CHECK_MESSAGE(sum == Width(area), "未暂存 / 箭头列 / 分隔条 / 已暂存之和应铺满区域");

  GC_CHECK(Width(columns.unstaged) >= spec.minListWidth);
  GC_CHECK(Width(columns.staged) >= spec.minListWidth);
  GC_CHECK(Width(columns.arrows) >= spec.minArrowColumnWidth);
  GC_CHECK(Height(columns.staged) == Height(area));
}

GC_TEST(staging_columns_scale_width_with_window) {
  const auto spec = MakeChangesSpec();
  gc::ui::StagingColumns small{};
  gc::ui::StagingColumns large{};
  gc::ui::ComputeStagingColumns({0, 0, 900, 400}, spec, &small);
  gc::ui::ComputeStagingColumns({0, 0, 1400, 400}, spec, &large);
  GC_CHECK(Width(large.unstaged) > Width(small.unstaged));
  GC_CHECK(Width(large.staged) > Width(small.staged));
}

GC_TEST(staging_columns_respect_dragged_ratio) {
  auto spec = MakeChangesSpec();
  spec.stagingRatio = 0.60;
  const RECT area{0, 0, 1200, 400};
  gc::ui::StagingColumns columns{};
  gc::ui::ComputeStagingColumns(area, spec, &columns);
  const int free = Width(area) - spec.splitterWidth - spec.arrowColumnWidth - 3 * spec.gap;
  const int expected = static_cast<int>(std::lround(0.60 * free));
  GC_CHECK_MESSAGE(std::abs(Width(columns.unstaged) - expected) <= 1, "未暂存列表宽度应按比例分配");
  GC_CHECK(Width(columns.unstaged) > Width(columns.staged));
}

GC_TEST(history_columns_are_ordered_and_fill_area) {
  const auto spec = MakeChangesSpec();
  const RECT area{0, 0, 1082, 400};
  gc::ui::HistoryColumns columns{};
  gc::ui::ComputeHistoryColumns(area, spec, &columns);

  GC_CHECK(columns.commits.left == area.left);
  GC_CHECK(columns.commits.right <= columns.splitter.left);
  GC_CHECK(columns.splitter.right <= columns.files.left);
  GC_CHECK(columns.files.right <= area.right);

  const int sum = Width(columns.commits) + Width(columns.splitter) + Width(columns.files) + 2 * spec.gap;
  GC_CHECK_MESSAGE(sum == Width(area), "最近提交 / 分隔条 / 文件变化之和应铺满区域");

  GC_CHECK(Width(columns.commits) >= spec.minListWidth);
  GC_CHECK(Width(columns.files) >= spec.minListWidth);
  GC_CHECK(Height(columns.files) == Height(area));
}

GC_TEST(history_columns_reserve_full_width_detail_band_below_both_lists) {
  auto spec = MakeChangesSpec();
  spec.detailHeight = 90;
  const RECT area{0, 0, 1082, 400};
  gc::ui::HistoryColumns columns{};
  gc::ui::ComputeHistoryColumns(area, spec, &columns);

  // 详情带横跨整页：两份列表让出的只有高度，宽度分配与上例完全一致。
  GC_CHECK(columns.detail.left == area.left);
  GC_CHECK(columns.detail.right == area.right);
  GC_CHECK(Height(columns.detail) == spec.detailHeight);
  GC_CHECK(columns.detail.bottom == area.bottom);
  GC_CHECK(columns.commits.bottom + spec.gap == columns.detail.top);
  GC_CHECK(columns.files.bottom + spec.gap == columns.detail.top);
  GC_CHECK(Height(columns.commits) == Height(area) - spec.detailHeight - spec.gap);

  // 比例拖动只看宽度，加了详情带之后同一鼠标位置换出的比例不变。
  double ratio = 0.0;
  gc::ui::HistoryRatioFromMouseX(area, spec, 400, &ratio);
  gc::ui::HistoryColumns withoutBand{};
  auto plain = MakeChangesSpec();
  gc::ui::ComputeHistoryColumns(area, plain, &withoutBand);
  double sameRatio = 0.0;
  gc::ui::HistoryRatioFromMouseX(area, plain, 400, &sameRatio);
  GC_CHECK_MESSAGE(std::abs(ratio - sameRatio) < 1e-9, "详情带不得影响横向比例换算");
}

GC_TEST(history_columns_yield_detail_band_when_area_is_too_short) {
  auto spec = MakeChangesSpec();
  spec.detailHeight = 500;
  const RECT area{0, 0, 1082, 120};
  gc::ui::HistoryColumns columns{};
  gc::ui::ComputeHistoryColumns(area, spec, &columns);

  // 整页矮到放不下那条带时，带最多只占一半，两份列表仍然留下可读的高度：
  // 列表被压成 0 高就是两块看不见的空框，比详情带变矮更糟。
  GC_CHECK(Height(columns.detail) == 60);
  GC_CHECK(Height(columns.commits) == 120 - 60 - spec.gap);
  GC_CHECK(Height(columns.commits) > 0);
  GC_CHECK(columns.commits.bottom + spec.gap == columns.detail.top);
  GC_CHECK(columns.detail.bottom == area.bottom);
  GC_CHECK(columns.commits.top >= area.top && columns.detail.bottom <= area.bottom);
}

GC_TEST(history_columns_respect_dragged_ratio) {
  auto spec = MakeChangesSpec();
  spec.historyRatio = 0.65;
  const RECT area{0, 0, 1200, 400};
  gc::ui::HistoryColumns columns{};
  gc::ui::ComputeHistoryColumns(area, spec, &columns);
  const int free = Width(area) - spec.splitterWidth - 2 * spec.gap;
  const int expected = static_cast<int>(std::lround(0.65 * free));
  GC_CHECK_MESSAGE(std::abs(Width(columns.commits) - expected) <= 1, "最近提交宽度应按比例分配");
  GC_CHECK(Width(columns.commits) > Width(columns.files));
}

GC_TEST(staging_splitter_drag_clamps_to_min_widths) {
  const RECT area{0, 0, 1200, 400};
  gc::ui::ChangesSpec spec = MakeChangesSpec();

  double staging = spec.stagingRatio;
  gc::ui::StagingRatioFromMouseX(area, spec, -5000, &staging);
  spec.stagingRatio = staging;
  gc::ui::StagingColumns clamped{};
  gc::ui::ComputeStagingColumns(area, spec, &clamped);
  GC_CHECK(Width(clamped.unstaged) >= spec.minListWidth);
  GC_CHECK(Width(clamped.staged) >= spec.minListWidth);

  double pushed = spec.stagingRatio;
  gc::ui::StagingRatioFromMouseX(area, spec, 999999, &pushed);
  spec.stagingRatio = pushed;
  gc::ui::StagingColumns other{};
  gc::ui::ComputeStagingColumns(area, spec, &other);
  GC_CHECK(Width(other.staged) >= spec.minListWidth);
}

GC_TEST(history_splitter_drag_clamps_to_min_widths) {
  const RECT area{0, 0, 1200, 400};
  gc::ui::ChangesSpec spec = MakeChangesSpec();

  double history = spec.historyRatio;
  gc::ui::HistoryRatioFromMouseX(area, spec, -5000, &history);
  spec.historyRatio = history;
  gc::ui::HistoryColumns clamped{};
  gc::ui::ComputeHistoryColumns(area, spec, &clamped);
  GC_CHECK(Width(clamped.commits) >= spec.minListWidth);
  GC_CHECK(Width(clamped.files) >= spec.minListWidth);
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
