#include "ui/layout.h"

#include <algorithm>
#include <cmath>

namespace gc::ui {
namespace {

constexpr int Width(const RECT& rect) noexcept { return rect.right - rect.left; }
constexpr int Height(const RECT& rect) noexcept { return rect.bottom - rect.top; }

constexpr RECT MakeRect(int x, int y, int w, int h) noexcept {
  return RECT{x, y, x + (w > 0 ? w : 0), y + (h > 0 ? h : 0)};
}

int AvailableListWidth(const RECT& area, const ChangesSpec& spec) noexcept {
  // 未暂存 | 箭头列 | 分隔条 | 已暂存 | 分隔条 | 最近提交 —— 六段之间有五个间隔。
  return Width(area) - 2 * spec.splitterWidth - spec.arrowColumnWidth - 5 * spec.gap;
}

double Clamp(double value, double low, double high) noexcept {
  if (high < low) {
    return low;
  }
  return std::clamp(value, low, high);
}

}  // namespace

bool ComputeBands(const RECT& client, const BandSpec& spec, Bands* out) {
  const int gap = std::max(0, spec.gap);
  const int width = std::max(0, Width(client));
  int y = client.top;

  Bands bands{};
  bands.repoBar = MakeRect(client.left, y, width, spec.repoBarHeight);
  y += spec.repoBarHeight + gap;
  bands.infoRow = MakeRect(client.left, y, width, spec.infoRowHeight);
  y += spec.infoRowHeight + gap;

  bands.actions = MakeRect(client.left, client.bottom - spec.actionRowHeight, width, spec.actionRowHeight);

  // 变更区与表单叠放在两者之间以及表单与底部按钮之间各留一个间隔。
  const int stack = std::max(0, static_cast<int>(bands.actions.top - y) - 2 * gap);
  int formHeight = static_cast<int>(std::lround(static_cast<double>(stack) * spec.formPercent / 100.0));
  formHeight = std::min(std::max(formHeight, spec.minFormHeight), std::max(0, stack - spec.minChangesHeight));
  formHeight = std::min(formHeight, stack);
  const int changesHeight = std::max(0, stack - formHeight);

  bands.changes = MakeRect(client.left, y, width, changesHeight);
  bands.commitForm = MakeRect(client.left, y + changesHeight + gap, width, formHeight);
  *out = bands;
  return stack >= spec.minChangesHeight + spec.minFormHeight;
}

void ComputeChangesColumns(const RECT& area, const ChangesSpec& spec, ChangesColumns* out) {
  const int gap = std::max(0, spec.gap);
  const int splitter = std::max(1, spec.splitterWidth);
  const int minList = std::max(1, spec.minListWidth);
  const int arrowWidth = std::max(spec.minArrowColumnWidth, spec.arrowColumnWidth);
  const int free = AvailableListWidth(area, spec);
  const int height = std::max(0, Height(area));

  int left = 0;
  int middle = 0;
  int right = 0;
  if (free < 3 * minList) {
    left = minList;
    middle = minList;
    right = std::max(minList, free - 2 * minList);
  } else {
    const int span = free;
    left = static_cast<int>(std::lround(Clamp(spec.leftRatio, 0.0, 1.0) * span));
    left = std::clamp(left, minList, span - 2 * minList);
    middle = static_cast<int>(std::lround(Clamp(spec.middleRatio, 0.0, 1.0) * span));
    middle = std::clamp(middle, minList, span - left - minList);
    right = span - left - middle;
  }

  int x = area.left;
  out->unstaged = MakeRect(x, area.top, left, height);
  x += left + gap;
  out->arrows = MakeRect(x, area.top, arrowWidth, height);
  x += arrowWidth + gap;
  out->splitterLeft = MakeRect(x, area.top, splitter, height);
  x += splitter + gap;
  out->staged = MakeRect(x, area.top, middle, height);
  x += middle + gap;
  out->splitterRight = MakeRect(x, area.top, splitter, height);
  x += splitter + gap;
  out->history = MakeRect(x, area.top, right, height);
}

void RatioFromMouseX(const RECT& area, const ChangesSpec& spec, int splitterIndex, int mouseX, double* ratioLeft,
                     double* ratioMiddle) {
  const int gap = std::max(0, spec.gap);
  const int splitter = std::max(1, spec.splitterWidth);
  const int arrowWidth = std::max(spec.minArrowColumnWidth, spec.arrowColumnWidth);
  const int free = std::max(1, AvailableListWidth(area, spec));
  const double minRatio = static_cast<double>(std::max(1, spec.minListWidth)) / free;

  if (splitterIndex == 0) {
    const int left = mouseX - area.left - arrowWidth - 2 * gap;
    const int maxLeft = free - 2 * std::max(1, spec.minListWidth);
    *ratioLeft = Clamp(static_cast<double>(left) / free, minRatio, static_cast<double>(maxLeft) / free);
    return;
  }

  const int left =
      std::clamp(static_cast<int>(std::lround((*ratioLeft) * free)), 0, free - 2 * std::max(1, spec.minListWidth));
  const int middle = mouseX - area.left - left - arrowWidth - splitter - 4 * gap;
  const int maxMiddle = free - left - std::max(1, spec.minListWidth);
  *ratioMiddle = Clamp(static_cast<double>(middle) / free, minRatio, static_cast<double>(maxMiddle) / free);
}

MinimumClientSize MinimumClientFor(const BandSpec& bands, const ChangesSpec& changes) {
  MinimumClientSize size{};
  const int columnsWidth = 3 * changes.minListWidth +
                           std::max(changes.minArrowColumnWidth, changes.arrowColumnWidth) +
                           2 * changes.splitterWidth + 5 * changes.gap;
  size.width = std::max(columnsWidth, bands.minWidth);
  size.height = bands.repoBarHeight + bands.infoRowHeight + bands.actionRowHeight + bands.minChangesHeight +
                bands.minFormHeight + 4 * bands.gap;
  return size;
}

}  // namespace gc::ui
