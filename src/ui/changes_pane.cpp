#include "ui/changes_pane.h"

#include <commctrl.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <utility>

#include "git/commit_detail.h"
#include "git/commit_history.h"
#include "ui/commands.h"
#include "ui/controls.h"

namespace gc::ui {
namespace {

void Place(HWND target, const RECT& rect) { PlaceIfChanged(target, rect); }

constexpr RECT Box(int left, int top, int width, int height) noexcept {
  return RECT{left, top, left + width, top + height};
}

constexpr std::wstring_view kExplicitBreak = L"\r\n";

// 详情带在「还没有选中提交」时显示的那句引导语。单独放这里：Create() 的首屏与
// ShowCommitDetail 的空状态都用它，两处说法不会走偏。
constexpr std::wstring_view kCommitDetailGreeting =
    L"选中「最近提交」里的某一条，这里显示它的完整信息（哈希、标题、作者与邮箱、作者时间、提交者时间、描述）。";

// 中间那一栏四个按钮的标题：竖排次序自上而下就是它们的范围次序
// （把整份工作区加进来 / 把选中的加进来 / 把选中的移出去 / 把整个索引移出去）。
// 建控件与摆位置共用这几份文字，两处不会因拷具而走偏。
constexpr std::wstring_view kStageAddAllLabel = L"全部加入 →";
constexpr std::wstring_view kStageAddLabel = L"加入暂存区 →";
constexpr std::wstring_view kStageRemoveLabel = L"← 移出暂存区";
constexpr std::wstring_view kStageRemoveAllLabel = L"← 全部移出";

[[nodiscard]] std::wstring TwoLine(std::wstring_view first, std::wstring_view second) {
  return std::wstring(first) + std::wstring(kExplicitBreak) + std::wstring(second);
}

// 选项卡标签条自身占掉的高度：内容区上边沿要在标签条之下，最小尺寸那一份高度也得把它算进去。
[[nodiscard]] int TabStripHeight(const gc::ui::UiMetrics& metrics) noexcept {
  return metrics.LabelHeight() + metrics.Scale(8);
}

}  // namespace

// 分组标题 + 列表表头 + 两行空状态说明 + 边框与留白。与 LayoutList 共用同一组函数，
// 保证窗口缩到最小尺寸时空状态说明仍完整可见。
int ChangesPane::GroupCaption(const UiMetrics& metrics) noexcept { return metrics.LabelHeight() + metrics.Scale(5); }
int ChangesPane::ListHeaderHeight(const UiMetrics& metrics) noexcept { return metrics.LabelHeight() + metrics.Scale(10); }
int ChangesPane::HintTextHeight(const UiMetrics& metrics) noexcept { return 2 * metrics.LabelHeight(); }
int ChangesPane::HintVerticalPadding(const UiMetrics& metrics) noexcept { return metrics.Scale(16); }
int ChangesPane::InnerInset(const UiMetrics& metrics) noexcept { return metrics.Scale(7); }

int ChangesPane::ButtonStackHeight(const UiMetrics& metrics) noexcept {
  // 四个按钮各占一行 + 三个行距。四行是同一栏里的竖排，自上而下：
  // 「全部加入 →」、「加入暂存区 →」、「← 移出暂存区」、「← 全部移出」——
  // 两个批量按钮就是把逐条按钮那一对向上下各撑开一格，中间两行仍是逐条操作。
  // （暂存方向与导航方向已经不在一栏里了：进入/返回子模块在「上游：」那一行右对齐。）
  return 4 * metrics.ControlHeight() + 3 * metrics.RowGap();
}

int ChangesPane::DetailHeight(const UiMetrics& metrics) noexcept {
  // 分组框标题那一行（复制按钮就摆在这一行的右端，不额外占高）+ 正文行 + 上下内边距。
  // 正文取七行：这份文本自己占六行（提交哈希、标题、作者、作者时间、提交者时间各一行，
  // 第六行是「描述：」这一句），多出来的一行是编辑框的边框与内部留白——实测按六行给的话
  // 露出来的只有五行半，「描述」那一行永远看不见，用户就不知道下面还有正文。
  // 再往上加就是把两份列表再压矮一截，而完整描述本来还有双击进命令窗口那一条路。
  return metrics.ControlHeight() + metrics.Scale(2) + 7 * metrics.LabelHeight() +
         2 * InnerInset(metrics) + metrics.Scale(4);
}

int ChangesPane::MinimumHeight(const UiMetrics& metrics) noexcept {
  const int listNeed = GroupCaption(metrics) + ListHeaderHeight(metrics) + HintTextHeight(metrics) +
                       HintVerticalPadding(metrics) + InnerInset(metrics);
  // 中间那一栏的四个按钮是同一栏里的竖排：窗口缩到最小时，按钮列比列表列更需要这点高度，
  // 否则下面那两个按钮会被摆到表单那一带上去。
  const int page = std::max(listNeed, ButtonStackHeight(metrics));
  // 「提交历史」那一页在两份列表下面还压着一条详情带，整页下限按它算：
  // 「变化信息」那一页只会比这个下限更宽裕，不会因此摆不下。
  return TabStripHeight(metrics) + page + DetailHeight(metrics) + metrics.ColGap();
}

void ChangesPane::Create(HWND parent) {
  // 选项卡容器先建、最后压到兄弟 z 序最底：它只负责画标签条，
  // 页面内容（分组框、列表）必须画在它上面，否则它整块客户区的底色会把内容糊掉。
  tabs_ = CreateTabControl(parent, kIdChangesTab);
  AddTabItem(tabs_, 0, L"变化信息");
  AddTabItem(tabs_, 1, L"提交历史");

  unstagedGroup_ = CreateGroupBox(parent, L"未暂存的更改", kIdUnstagedGroup);
  unstagedList_ = CreateReportListView(parent, kIdUnstagedList, changeColumns_);
  unstagedHint_ = CreateCenteredLabel(unstagedList_, L"", kIdUnstagedHint);

  stageAddAll_ = CreatePushButton(parent, kStageAddAllLabel, kIdStageAddAllButton);
  stageAdd_ = CreatePushButton(parent, kStageAddLabel, kIdStageAddButton);
  stageRemove_ = CreatePushButton(parent, kStageRemoveLabel, kIdStageRemoveButton);
  stageRemoveAll_ = CreatePushButton(parent, kStageRemoveAllLabel, kIdStageRemoveAllButton);

  stagedGroup_ = CreateGroupBox(parent, L"已暂存的更改", kIdStagedGroup);
  stagedList_ = CreateReportListView(parent, kIdStagedList, changeColumns_);
  stagedHint_ = CreateCenteredLabel(stagedList_, L"", kIdStagedHint);

  historyGroup_ = CreateGroupBox(parent, L"最近提交", kIdHistoryGroup);
  historyList_ = CreateReportListView(parent, kIdHistoryList, commitColumns_);
  historyHint_ = CreateCenteredLabel(historyList_, L"", kIdHistoryHint);

  filesGroup_ = CreateGroupBox(parent, L"文件变化", kIdFilesGroup);
  filesList_ = CreateReportListView(parent, kIdFilesList, changeColumns_);
  filesHint_ = CreateCenteredLabel(filesList_, L"", kIdFilesHint);
  ShowCommitFiles(CommitFilesView{});  // 首屏引导语：还没有选中提交

  // 「提交历史」页底部那条带：只读多行编辑框（可选中、可 Ctrl+C）+ 标题行右端的复制按钮。
  // 内容不在这里填：首屏是「还没选中」那一句引导语，由 ShowCommitDetail 落地。
  detailGroup_ = CreateGroupBox(parent, L"提交详情", kIdCommitDetailGroup);
  detailEdit_ = CreateReadOnlyMultilineEdit(parent, kCommitDetailGreeting, kIdCommitDetailEdit);
  detailCopyButton_ = CreatePushButton(parent, L"复制详情", kIdCommitDetailCopyButton);
  ::EnableWindow(detailCopyButton_, FALSE);
  ShowCommitDetail(CommitDetailView{});

  // 分组框与选项卡都压到兄弟 z 序最底：分组框要在自己的内部填底色（否则列表移动后旧像素没人擦），
  // 而新建的子窗口默认排在最前面，不压下去它就会把随后创建的列表整块糊掉。
  SendToBack(unstagedGroup_);
  SendToBack(stagedGroup_);
  SendToBack(historyGroup_);
  SendToBack(filesGroup_);
  SendToBack(detailGroup_);
  SendToBack(tabs_);
}

RECT ChangesPane::PlaceTabs(const RECT& bounds) {
  return PlaceTabControl(tabs_, bounds);
}

int ChangesPane::ActiveTab() const noexcept {
  const int index = GetActiveTab(tabs_);
  return index == 1 ? 1 : 0;  // 还没建好（返回 -1）时按第一页处理，与「变化信息」一致。
}

void ChangesPane::ShowControls(bool visible, std::initializer_list<HWND> controls) {
  for (HWND control : controls) {
    if (control != nullptr) {
      ::ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    }
  }
}RECT ChangesPane::InnerRect(const RECT& group, const UiMetrics& metrics) noexcept {
  const int inset = InnerInset(metrics);
  return RECT{group.left + inset, group.top + GroupCaption(metrics), group.right - inset, group.bottom - inset};
}

void ChangesPane::LayoutList(HWND group, HWND list, HWND hint, const RECT& bounds, const UiMetrics& metrics,
                             const std::vector<ListColumn>& columns) {
  Place(group, bounds);
  const RECT inner = InnerRect(bounds, metrics);
  RECT listBefore{};
  ::GetWindowRect(list, &listBefore);   // 必须在移动列表之前取旧位置
  Place(list, inner);
  const int listWidth = inner.right - inner.left;
  ApplyListColumnWidths(list, columns, listWidth);

  // 表头高度按实际控件测量，提示文字在其下方居中，避免与表头或滚动条重叠。
  RECT area{};
  ::GetClientRect(list, &area);
  int headerBottom = ListHeaderHeight(metrics);
  auto header = reinterpret_cast<HWND>(::SendMessageW(list, LVM_GETHEADER, 0, 0));
  if (header != nullptr) {
    RECT headerRect{};
    ::GetWindowRect(header, &headerRect);
    POINT topLeft{headerRect.left, headerRect.top};
    ::ScreenToClient(list, &topLeft);
    RECT headerClient{};
    ::GetClientRect(header, &headerClient);
    headerBottom = topLeft.y + (headerClient.bottom - headerClient.top) + metrics.Scale(2);
  }

  // 提示文字在表头以下的可用区域内居中；列表尺寸变化时整块重绘，
  // 因为 ListView 不会自动擦除子控件旧位置，否则缩放时会留下文字重影。
  const int textHeight = HintTextHeight(metrics);
  const int available = static_cast<int>(area.bottom) - headerBottom - metrics.Scale(4);
  const int top = headerBottom + std::max(0, (available - textHeight) / 2);
  const RECT hintRect{metrics.Scale(3), top, static_cast<int>(area.right) - metrics.Scale(3),
                      top + textHeight + metrics.Scale(2)};
  Place(hint, hintRect);
  if (listBefore.right - listBefore.left != inner.right - inner.left ||
      listBefore.bottom - listBefore.top != inner.bottom - inner.top) {
    ::InvalidateRect(list, nullptr, TRUE);
  }
}

void ChangesPane::LayoutStaging(const StagingColumns& columns, const UiMetrics& metrics) {
  // 空状态说明不在这里显隐：它是各自那个列表的子窗口，列表一藏它自然跟着藏，列表露出来时
  // 它该不该露着只由「这一列有没有条目」决定（RebuildList 落地的那个结论）。
  // 整页一律 SW_SHOW 等于把已经藏起来的说明又摆回到行上面——用户看到的就是叠在清单上的空提示。
  ShowControls(true, {unstagedGroup_, unstagedList_, stagedGroup_, stagedList_, stageAddAll_, stageAdd_,
                      stageRemove_, stageRemoveAll_});
  ShowControls(false, {historyGroup_, historyList_, filesGroup_, filesList_, detailGroup_, detailEdit_,
                       detailCopyButton_});

  LayoutList(unstagedGroup_, unstagedList_, unstagedHint_, columns.unstaged, metrics, changeColumns_);
  LayoutList(stagedGroup_, stagedList_, stagedHint_, columns.staged, metrics, changeColumns_);

  const int gap = metrics.RowGap();
  const int rowHeight = metrics.ControlHeight();
  const int arrowWidth = columns.arrows.right - columns.arrows.left;
  // 四行竖排居中：全部加入 → / 加入暂存区 → / ← 移出暂存区 / ← 全部移出。
  // 每一行各自按自己的文字量宽，横向在箭头列里居中，宽窄不一也不会错位。
  const int stackHeight = ChangesPane::ButtonStackHeight(metrics);
  int top = columns.arrows.top + ((columns.arrows.bottom - columns.arrows.top) - stackHeight) / 2;
  top = std::max<int>(columns.arrows.top, top);

  const std::pair<HWND, std::wstring_view> rows[] = {
      {stageAddAll_, kStageAddAllLabel},
      {stageAdd_, kStageAddLabel},
      {stageRemove_, kStageRemoveLabel},
      {stageRemoveAll_, kStageRemoveAllLabel},
  };
  for (const auto& [button, label] : rows) {
    const int width = std::min(arrowWidth, metrics.ButtonWidth(label));
    Place(button, Box(columns.arrows.left + (arrowWidth - width) / 2, top, width, rowHeight));
    top += rowHeight + gap;
  }
}

void ChangesPane::LayoutHistory(const HistoryColumns& columns, const UiMetrics& metrics) {
  ShowControls(false, {unstagedGroup_, unstagedList_, stagedGroup_, stagedList_, stageAddAll_, stageAdd_,
                       stageRemove_, stageRemoveAll_});
  ShowControls(true, {historyGroup_, historyList_, filesGroup_, filesList_, detailGroup_, detailEdit_,
                      detailCopyButton_});

  LayoutList(historyGroup_, historyList_, historyHint_, columns.commits, metrics, commitColumns_);
  LayoutList(filesGroup_, filesList_, filesHint_, columns.files, metrics, changeColumns_);

  // 详情带横跨整页（两份列表让出来的只有高度）。栏位面积为 0 时（详情带被压掉的那一档窗口）
  // 三个控件一起收成空矩形：PlaceIfChanged 对零尺寸矩形本来就是空操作，摆出负宽高才是问题。
  const RECT& band = columns.detail;
  const int bandWidth = band.right - band.left;
  const int bandHeight = band.bottom - band.top;
  if (bandWidth <= 0 || bandHeight <= 0) {
    const RECT empty{0, 0, 0, 0};
    Place(detailGroup_, empty);
    Place(detailEdit_, empty);
    Place(detailCopyButton_, empty);
    return;
  }
  Place(detailGroup_, band);

  // 复制按钮摆在分组框标题那一行的右端：它不下移正文，详情文本因此多留一行可读高度。
  const int copyWidth = std::min(bandWidth - 2 * InnerInset(metrics), metrics.ButtonWidth(L"复制详情"));
  Place(detailCopyButton_,
        Box(band.right - copyWidth - InnerInset(metrics), band.top + metrics.Scale(1), copyWidth,
            metrics.ControlHeight()));

  const RECT inner{band.left + InnerInset(metrics), band.top + metrics.ControlHeight() + metrics.Scale(2),
                   band.right - InnerInset(metrics), band.bottom - InnerInset(metrics)};
  Place(detailEdit_, inner);
}

void ChangesPane::ShowCommitDetail(const CommitDetailView& view) {
  // 四种状态各说各的：没有选中、正在读、读失败、读到了。后两种都不能显示成一份空详情，
  // 否则用户会把「没读出来」的那块空白当成「这条提交没有描述」。
  std::wstring text;
  if (view.commit == nullptr) {
    text = std::wstring(kCommitDetailGreeting);
  } else if (view.loading) {
    text = TwoLine(L"正在读取这条提交的完整信息", L"后台执行 git log -n 1 --format=…，只读，不改动仓库。");
  } else if (!view.error.empty()) {
    text = TwoLine(L"这条提交的完整信息没能读出来", view.error);
  } else {
    text = view.text;
  }

  detailCommitId_ = view.commit != nullptr ? view.commit->objectId : std::wstring();
  // 只有真详情才进可复制文本：引导语与失败说明粘出去会被当成提交信息，那是更糟的误导。
  detailText_ = (view.commit != nullptr && !view.loading && view.error.empty()) ? view.text : std::wstring();

  // 编辑框只认 CRLF：Git 记下的正文用 LF，失败原因里也可能带换行，一律在落地前统一。
  SetControlText(detailEdit_, git::NormalizeTextForEdit(text));
  ::EnableWindow(detailCopyButton_, detailText_.empty() ? FALSE : TRUE);
}

template <typename Item>
app::ListViewMemory ChangesPane::CaptureMemory(HWND list, const std::vector<Item>& shown) {
  app::ListViewMemory memory;
  for (const int row : GetListSelectedRows(list)) {
    if (row >= 0 && static_cast<size_t>(row) < shown.size()) {
      memory.selectedKeys.push_back(app::ViewKeyForItem(shown[static_cast<size_t>(row)]));
    }
  }
  return memory;
}

std::vector<std::wstring> ChangesPane::CellsFor(const git::ChangeItem& item) {
  return {item.StatusLabel(), item.PathLabel()};
}

std::vector<std::wstring> ChangesPane::CellsFor(const git::CommitItem& item) {
  // 提交列展示短 ID（只是前缀，供阅读），一切命令都用条目里的完整对象 ID。
  return {git::ShortObjectId(item.objectId), item.summary, item.author, item.authoredAt};
}

template <typename Item>
void ChangesPane::RebuildList(HWND list, HWND hint, const std::vector<Item>& shown,
                              const app::ListViewMemory& memory, const std::vector<Item>& items,
                              const std::wstring& hintText) {
  // 按差异就地更新，不整列清空：报表视图不响应 LVM_SCROLL，一旦清空重建，
  // 用户正在看的滚动位置就再也放不回去，只剩顶部一个选择。
  // 行与条目的对应关系由下面这份 keys 维护，全程不从单元格文本反查。
  const ListRedrawPause pause(list);  // 中间态不绘制，更新完一次性重绘，避免闪一下。
  std::vector<std::wstring> keys;
  keys.reserve(shown.size());
  for (const Item& item : shown) {
    keys.push_back(app::ViewKeyForItem(item));
  }
  std::vector<std::wstring> wanted;
  wanted.reserve(items.size());
  for (const Item& item : items) {
    wanted.push_back(app::ViewKeyForItem(item));
  }

  // 先认头部与尾部的公共段：绝大多数刷新只是中间有一两条进出，两端根本不用动。
  size_t head = 0;
  while (head < keys.size() && head < wanted.size() && keys[head] == wanted[head]) {
    ++head;
  }
  size_t oldTail = keys.size();
  size_t newTail = wanted.size();
  while (oldTail > head && newTail > head && keys[oldTail - 1] == wanted[newTail - 1]) {
    --oldTail;
    --newTail;
  }
  // 中段：旧的多余行自后往前删掉（删除不会让前面已算好的行号失准）。
  for (int row = static_cast<int>(oldTail) - 1; row >= static_cast<int>(head); --row) {
    RemoveListRow(list, row);
  }
  keys.erase(keys.begin() + static_cast<ptrdiff_t>(head), keys.begin() + static_cast<ptrdiff_t>(oldTail));
  // 中段：新的条目按顺序插在各自的位置上。
  for (size_t index = head; index < newTail; ++index) {
    InsertListRow(list, static_cast<int>(index), CellsFor(items[index]));
    keys.insert(keys.begin() + static_cast<ptrdiff_t>(index), wanted[index]);
  }
  // 结构对齐以后，逐行刷新显示文本：同一条目的内容可能已经变了（状态字改变、时间重算）。
  for (size_t index = 0; index < items.size(); ++index) {
    SetListRowCells(list, static_cast<int>(index), CellsFor(items[index]));
  }
  // 选中项按条目身份恢复：行可能已经上移、下移或整条消失，消失的那条不会凭空选到别人。
  RestoreListSelection(list, app::MapListViewMemory(items, memory).selectedRows);

  // 空状态说明只在确实没有条目时占用列表区域；有条目时隐藏，避免叠在行上。
  SetControlText(hint, hintText);
  ::ShowWindow(hint, items.empty() && !hintText.empty() ? SW_SHOW : SW_HIDE);
}

void ChangesPane::ShowWorkspace(const git::WorkspaceModel& model, const git::EmptyStateTexts& texts) {

  // 行内容与模型下标一一对应：单元格列都只是显示文本，
  // 后续步骤按行号回到模型取原始路径/对象 ID 与状态，绝不从单元格文字反解 Git 命令参数。
  // 记忆必须在改动行之前取：行号一旦移动，原来的选择就对不上条目了。
  const app::ListViewMemory unstagedMemory = CaptureMemory(unstagedList_, shown_.unstaged);
  const app::ListViewMemory stagedMemory = CaptureMemory(stagedList_, shown_.staged);
  const app::ListViewMemory historyMemory = CaptureMemory(historyList_, shown_.recentCommits);

  // 重建期间发出的选中项恢复通知是中间态，主窗口不能按它发后台查询（见 UpdatingLists 的说明）。
  updatingLists_ = true;
  RebuildList(unstagedList_, unstagedHint_, shown_.unstaged, unstagedMemory, model.unstaged, texts.unstaged);
  RebuildList(stagedList_, stagedHint_, shown_.staged, stagedMemory, model.staged, texts.staged);
  // 提交历史同一套就地更新：刷新后仍存在的提交保持选中，新提交插入到应有的位置。
  RebuildList(historyList_, historyHint_, shown_.recentCommits, historyMemory, model.recentCommits,
              texts.history);
  updatingLists_ = false;
  shown_ = model;
}

void ChangesPane::ShowCommitFiles(const CommitFilesView& view) {
  // 「文件变化」的清单按差异就地更新：刷新历史列表不该把用户正看的那份清单清空重建。
  const app::ListViewMemory memory = CaptureMemory(filesList_, shownFiles_);
  const bool wasUpdating = updatingLists_;
  updatingLists_ = true;
  filesLoading_ = view.loading;
  // 上一份清单必须先挪出来当「控件里现在有哪几行」：RebuildList 是按新旧两侧的差异就地增删行的，
  // 把新清单同时当作两侧传进去，它永远看不出内容有变化——新行一条不会插进来，旧行一条不会掉，
  // 于是清单停在上一次的内容上，而状态栏已经在说这一条提交有多少个文件。
  std::vector<git::ChangeItem> previous;
  previous.swap(shownFiles_);
  if (view.commit == nullptr) {
    filesCommit_ = git::CommitItem{};
    filesCommitId_.clear();
  } else {
    filesCommit_ = *view.commit;
    filesCommitId_ = view.commit->objectId;
    shownFiles_ = view.changes;
  }
  // 说明文字按「这一段读取怎么样」给：还没选提交、正在读、读失败、读到了但没有变化，
  // 四种空都不能显示成同一句「没有文件」。
  if (view.commit == nullptr) {
    filesHintText_ = TwoLine(L"选择一条提交查看它的文件变化", L"选中「最近提交」里的某一条，这里会列出它改动的文件；双击文件可查看详细差异。");
  } else if (view.loading) {
    filesHintText_ = TwoLine(L"正在读取这条提交的文件变化", L"后台执行 git show --name-status…");
  } else if (!view.error.empty()) {
    filesHintText_ = TwoLine(L"文件变化读取失败", view.error);
  } else if (view.changes.empty()) {
    filesHintText_ = TwoLine(L"这条提交没有文件变化", view.note);
  } else {
    filesHintText_.clear();
  }
  RebuildList(filesList_, filesHint_, previous, memory, shownFiles_, filesHintText_);
  updatingLists_ = wasUpdating;
}

const git::ChangeItem* ChangesPane::ItemAt(HWND list, int row, git::ChangeSide* side) const {
  const std::vector<git::ChangeItem>* items = nullptr;
  if (list == unstagedList_) {
    items = &shown_.unstaged;
    if (side != nullptr) {
      *side = git::ChangeSide::unstaged;
    }
  } else if (list == stagedList_) {
    items = &shown_.staged;
    if (side != nullptr) {
      *side = git::ChangeSide::staged;
    }
  } else {
    return nullptr;  // 提交历史或陌生句柄：历史条目经 CommitItemAt 按行号取，类型不同不从这里走。
  }
  if (row < 0 || static_cast<size_t>(row) >= items->size()) {
    return nullptr;  // 行已不在当前显示内容里（外部改动后被刷新移除）。
  }
  return &(*items)[static_cast<size_t>(row)];
}

const git::CommitItem* ChangesPane::CommitItemAt(int row) const {
  if (row < 0 || static_cast<size_t>(row) >= shown_.recentCommits.size()) {
    return nullptr;  // 与 ItemAt 同一约定：行号只对最后一次落地的内容有效。
  }
  return &shown_.recentCommits[static_cast<size_t>(row)];
}

bool ChangesPane::CommitFileAt(int row, const git::CommitItem** commit,
                               const git::ChangeItem** change) const {
  if (filesCommitId_.empty() || row < 0 || static_cast<size_t>(row) >= shownFiles_.size()) {
    return false;  // 还没有正在展示的提交，或行号已不在当前清单里。
  }
  if (commit != nullptr) {
    *commit = &filesCommit_;
  }
  if (change != nullptr) {
    *change = &shownFiles_[static_cast<size_t>(row)];
  }
  return true;
}

int ChangesPane::ListRowCount(HWND list) const {
  if (list != unstagedList_ && list != stagedList_ && list != historyList_ && list != filesList_) {
    return -1;
  }
  return GetListItemCount(list);
}

std::vector<int> ChangesPane::SelectedUnstagedRows() const { return GetListSelectedRows(unstagedList_); }

std::vector<int> ChangesPane::SelectedStagedRows() const { return GetListSelectedRows(stagedList_); }

int ChangesPane::SelectedHistoryRow() const {
  const std::vector<int> rows = GetListSelectedRows(historyList_);
  return rows.empty() ? -1 : rows.front();
}

}  // namespace gc::ui
