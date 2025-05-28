#pragma once

#include <windows.h>

namespace gc::ui {

// 纯几何计算，不触碰任何窗口句柄：可被单元测试直接验证，也保证 DPI/缩放变化时只有一处布局规则。

struct BandSpec {
  int repoBarHeight = 0;    // “本地仓库 / Git 程序”两行
  int infoRowHeight = 0;    // 仓库类型、当前分支、上游（子模块导航按钮也在这条带上，右对齐）
  int actionRowHeight = 0;  // 底部操作按钮
  int gap = 0;              // 横向带之间的间隔
  int minChangesHeight = 0;
  int minFormHeight = 0;
  int formPercent = 42;     // 剩余高度里分给提交表单的百分比
  int minWidth = 0;         // 信息行等单行文本不可压缩所需的最小客户区宽度
};

struct Bands {
  RECT repoBar{};
  RECT infoRow{};
  RECT changes{};
  RECT commitForm{};
  RECT actions{};
};

bool ComputeBands(const RECT& client, const BandSpec& spec, Bands* out);

// 变更区的布局参数。两个选项卡各自只有一条分隔条，因此只保留两个比例：
//   stagingRatio —— 「变化信息」选项卡里未暂存列表占可用宽度的比例；
//   historyRatio —— 「提交历史」选项卡里最近提交列表占可用宽度的比例。
// detailHeight 是「提交历史」页底部那条「提交详情」带的高度（含分组框标题）：
// 它横跨整页，两份列表因此各让出这一截高度。取 0 时不留这条带，也不扣它与列表之间的间隔。
struct ChangesSpec {
  int splitterWidth = 7;
  int arrowColumnWidth = 118;
  int minListWidth = 168;
  int minArrowColumnWidth = 96;
  int gap = 6;
  int detailHeight = 0;
  double stagingRatio = 0.34;
  double historyRatio = 0.34;
};

// 「变化信息」选项卡内容区：未暂存 | 箭头列 | 分隔条 | 已暂存。
struct StagingColumns {
  RECT unstaged{};
  RECT arrows{};
  RECT splitter{};
  RECT staged{};
};

// 「提交历史」选项卡内容区：上半是最近提交 | 分隔条 | 文件变化，下半是横跨整页的提交详情。
// 三份上排的栏位高 = detail 高 = 区域高 − 详情带高 − 一个间隔（detailHeight 为 0 时不扣间隔）。
struct HistoryColumns {
  RECT commits{};
  RECT splitter{};
  RECT files{};
  RECT detail{};
};

// area 是选项卡内容区（已去掉标签条与内边距），各栏都填满它的高度。
void ComputeStagingColumns(const RECT& area, const ChangesSpec& spec, StagingColumns* out);
void ComputeHistoryColumns(const RECT& area, const ChangesSpec& spec, HistoryColumns* out);

// 把拖动分隔条时的鼠标 X 坐标（相对分隔条所在窗口的客户区）换算成新的比例。
void StagingRatioFromMouseX(const RECT& area, const ChangesSpec& spec, int mouseX, double* ratio);
void HistoryRatioFromMouseX(const RECT& area, const ChangesSpec& spec, int mouseX, double* ratio);

struct MinimumClientSize {
  int width = 0;
  int height = 0;
};

// 取两个选项卡各自所需宽度（含标签条）与不可压缩单行文本所需宽度中的较大者。
MinimumClientSize MinimumClientFor(const BandSpec& bands, const ChangesSpec& changes);

}  // namespace gc::ui
