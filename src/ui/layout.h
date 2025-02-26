#pragma once

#include <windows.h>

namespace gc::ui {

// 纯几何计算，不触碰任何窗口句柄：可被单元测试直接验证，也保证 DPI/缩放变化时只有一处布局规则。

struct BandSpec {
  int repoBarHeight = 0;    // “本地仓库 / Git 程序”两行
  int infoRowHeight = 0;    // 当前分支、上游、任务状态
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

struct ChangesSpec {
  int splitterWidth = 7;
  int arrowColumnWidth = 118;
  int minListWidth = 168;
  int minArrowColumnWidth = 96;
  int gap = 6;
  double leftRatio = 0.34;    // 未暂存列表占可用宽度的比例
  double middleRatio = 0.34;  // 已暂存列表占可用宽度的比例
};

struct ChangesColumns {
  RECT unstaged{};
  RECT arrows{};
  RECT splitterLeft{};
  RECT staged{};
  RECT splitterRight{};
  RECT history{};
};

void ComputeChangesColumns(const RECT& area, const ChangesSpec& spec, ChangesColumns* out);

// 把拖动分隔条时的鼠标 X 坐标换算成新的比例（索引 0 = 左分隔条，1 = 右分隔条）。
void RatioFromMouseX(const RECT& area, const ChangesSpec& spec, int splitterIndex, int mouseX, double* ratioLeft,
                     double* ratioMiddle);

struct MinimumClientSize {
  int width = 0;
  int height = 0;
};

// 取三栏所需宽度与不可压缩单行文本所需宽度中的较大者。
MinimumClientSize MinimumClientFor(const BandSpec& bands, const ChangesSpec& changes);

}  // namespace gc::ui
