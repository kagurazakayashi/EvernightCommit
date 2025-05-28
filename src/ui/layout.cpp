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

// 「变化信息」选项卡：未暂存 | 箭头列 | 分隔条 | 已暂存 —— 四段之间有 3 个间隔。
int StagingAvailableWidth(const RECT& area, const ChangesSpec& spec) noexcept {
  return Width(area) - spec.splitterWidth - spec.arrowColumnWidth - 3 * spec.gap;
}

// 「提交历史」选项卡：最近提交 | 分隔条 | 文件变化 —— 三段之间有 2 个间隔。
int HistoryAvailableWidth(const RECT& area, const ChangesSpec& spec) noexcept {
  return Width(area) - spec.splitterWidth - 2 * spec.gap;
}

double Clamp(double value, double low, double high) noexcept {
  if (high < low) {
    return low;
  }
  return std::clamp(value, low, high);
}

// 比例拖动换算的公共部分：空间不足时 high < low，Clamp 一律回到最小比例。
void ApplyRatio(int mouseX, int areaLeft, int offset, int free, int minList, double* ratio) noexcept {
  const double minRatio = static_cast<double>(minList) / free;
  const int maxSize = free - minList;
  *ratio = Clamp(static_cast<double>(mouseX - areaLeft - offset) / free, minRatio,
                 static_cast<double>(maxSize) / free);
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

void ComputeStagingColumns(const RECT& area, const ChangesSpec& spec, StagingColumns* out) {
  const int gap = std::max(0, spec.gap);
  const int splitter = std::max(1, spec.splitterWidth);
  const int minList = std::max(1, spec.minListWidth);
  const int arrowWidth = std::max(spec.minArrowColumnWidth, spec.arrowColumnWidth);
  const int free = StagingAvailableWidth(area, spec);
  const int height = std::max(0, Height(area));

  int left = 0;
  int right = 0;
  if (free < 2 * minList) {
    // 空间不够分：先把未暂存与已暂存顶到最小宽度，剩余给已暂存侧（与从前三栏时的退化行为一致）。
    left = minList;
    right = std::max(minList, free - minList);
  } else {
    left = static_cast<int>(std::lround(Clamp(spec.stagingRatio, 0.0, 1.0) * free));
    left = std::clamp(left, minList, free - minList);
    right = free - left;
  }

  int x = area.left;
  out->unstaged = MakeRect(x, area.top, left, height);
  x += left + gap;
  out->arrows = MakeRect(x, area.top, arrowWidth, height);
  x += arrowWidth + gap;
  out->splitter = MakeRect(x, area.top, splitter, height);
  x += splitter + gap;
  out->staged = MakeRect(x, area.top, right, height);
}

void ComputeHistoryColumns(const RECT& area, const ChangesSpec& spec, HistoryColumns* out) {
  const int gap = std::max(0, spec.gap);
  const int splitter = std::max(1, spec.splitterWidth);
  const int minList = std::max(1, spec.minListWidth);
  const int free = HistoryAvailableWidth(area, spec);
  const int areaHeight = std::max(0, Height(area));
  // 详情带先从整页高度里扣掉（连同它与列表之间的那一个间隔），两份列表分的是剩下那一截。
  // 上限取整页的一半：这是防御性的一条，正常窗口碰不到——整页下限已经按
  // 「列表最低高度 + 详情带 + 间隔」算过了。真被压到那一档时宁可让详情带变矮，
  // 也不把两份列表压成 0 高（列表成了 0 高就是两块看不见的空框）。
  const int detail = std::min(std::clamp(spec.detailHeight, 0, areaHeight), areaHeight / 2);
  const int listHeight = std::max(0, areaHeight - detail - (detail > 0 ? gap : 0));

  int commits = 0;
  int files = 0;
  if (free < 2 * minList) {
    commits = minList;
    files = std::max(minList, free - minList);
  } else {
    commits = static_cast<int>(std::lround(Clamp(spec.historyRatio, 0.0, 1.0) * free));
    commits = std::clamp(commits, minList, free - minList);
    files = free - commits;
  }

  int x = area.left;
  out->commits = MakeRect(x, area.top, commits, listHeight);
  x += commits + gap;
  out->splitter = MakeRect(x, area.top, splitter, listHeight);
  x += splitter + gap;
  out->files = MakeRect(x, area.top, files, listHeight);
  // 详情带横跨整页：两份列表让出的只有高度，宽度分配与拖动换算都不受影响。
  out->detail = MakeRect(area.left, area.top + listHeight + (detail > 0 ? gap : 0), Width(area), detail);
}

void StagingRatioFromMouseX(const RECT& area, const ChangesSpec& spec, int mouseX, double* ratio) {
  const int gap = std::max(0, spec.gap);
  const int arrowWidth = std::max(spec.minArrowColumnWidth, spec.arrowColumnWidth);
  const int free = std::max(1, StagingAvailableWidth(area, spec));
  const int minList = std::max(1, spec.minListWidth);
  // 分隔条左边缘 = area.left + 未暂存宽 + 间隔 + 箭头列 + 间隔。
  ApplyRatio(mouseX, area.left, arrowWidth + 2 * gap, free, minList, ratio);
}

void HistoryRatioFromMouseX(const RECT& area, const ChangesSpec& spec, int mouseX, double* ratio) {
  const int gap = std::max(0, spec.gap);
  const int free = std::max(1, HistoryAvailableWidth(area, spec));
  const int minList = std::max(1, spec.minListWidth);
  // 分隔条左边缘 = area.left + 最近提交宽 + 间隔。
  ApplyRatio(mouseX, area.left, gap, free, minList, ratio);
}

MinimumClientSize MinimumClientFor(const BandSpec& bands, const ChangesSpec& changes) {
  MinimumClientSize size{};
  const int splitter = std::max(1, changes.splitterWidth);
  const int arrow = std::max(changes.minArrowColumnWidth, changes.arrowColumnWidth);
  const int minList = std::max(1, changes.minListWidth);
  const int gap = std::max(0, changes.gap);
  // 两个选项卡各要两个列表；变化信息选项卡多一条箭头列。取较宽的那个作为下限。
  const int stagingWidth = 2 * minList + arrow + splitter + 3 * gap;
  const int historyWidth = 2 * minList + splitter + 2 * gap;
  size.width = std::max(std::max(stagingWidth, historyWidth), bands.minWidth);
  size.height = bands.repoBarHeight + bands.infoRowHeight + bands.actionRowHeight + bands.minChangesHeight +
                bands.minFormHeight + 4 * bands.gap;
  return size;
}

}  // namespace gc::ui
