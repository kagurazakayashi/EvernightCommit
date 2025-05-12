#pragma once

#include <string>
#include <string_view>

namespace gc::app {

// 主窗口接通的五个有编排过程的 Git 操作。每一个都有自己的后台只读预检、确认框、
// 点头之后的执行前复核等中间阶段；这些阶段之间谁能与谁并存、谁要拦谁、拦住之后怎么向
// 用户解释，是一套有明确取舍的规则（见 AdmitWriteFlow 的实现）。把这套规则收在与窗口
// 无关的纯逻辑里，界面入口只需要问一句「现在可以开始吗」。
enum class GitFlow {
  commit,  // 创建提交
  undo,    // 撤回最近提交
  fetch,   // 抓取
  pull,    // 拉取（获取 + 整合两步）
  push,    // 推送
};

// 五个操作此刻各自是否正走在自己的流程里（预检、复核、命令窗口、核实等任何非终态阶段）。
struct GitFlowActivity {
  bool commit = false;
  bool undo = false;
  bool fetch = false;
  bool pull = false;
  bool push = false;
};

// 写操作共同前提的输入。全部是界面一侧已经知道的事实值——准入判定不自己去读窗口状态，
// 这样同一套规则既能被生产入口使用，也能被单元测试钉住。
struct WritePrerequisites {
  bool gitUsable = false;            // Git 程序已通过验证可用
  bool repoUsable = false;           // 仓库已识别，且形态允许工作区操作
  bool commandWindowBusy = false;    // 已有一个命令窗口操作在途（单槽）
  bool workspaceStillBound = false;  // 界面显示的工作区根仍是协调器绑定的那一个
};

// 写操作共同前提的判定：Git/仓库可用、命令窗口单槽空闲、界面工作区与协调器身份一致。
// 返回空串 = 前提成立；否则为要写进「任务状态」的完整拒绝说明。
// 暂存与查看类操作只走这一层（它们不与五个被编排的操作互斥——这是既有行为的原样保留）；
// AdmitWriteFlow 在此基础上再加「谁的流程正占路」的裁决。
[[nodiscard]] std::wstring DescribeWritePrerequisiteRefusal(const WritePrerequisites& prerequisites,
                                                            std::wstring_view actionLabel);

// 用户点击某个写操作按钮之前的裁决：返回空串 = 允许开始；否则返回要原样写进
// 「任务状态」的完整说明（点名占路的操作与等待之后怎么办，绝不笼统说“稍后再试”）。
// 检查次序与既有界面逐项一致：
//   1) 共同前提（Git/仓库不可用 → 命令窗口占用 → 工作区已换）；
//   2) 自己已有流程在走（各操作另有其明确的“再点不会排队”说法）；
//   3) 会拦住这个操作别的流程（注意这套互斥是刻意不对称的：
//      「创建提交」的只读核对不拦任何人；fetch 与 pull 的预检可以并存——它们都只是
//      只读查询，命令窗口那一步另有单槽裁决。任何一条被改宽都必须先说明理由）。
[[nodiscard]] std::wstring AdmitWriteFlow(GitFlow requested, const GitFlowActivity& activity,
                                          const WritePrerequisites& prerequisites,
                                          std::wstring_view actionLabel);

// 仓库导航（「进入子模块」/「返回父仓库」）的准入裁决。导航本身不写任何东西——不改索引、
// 不产生提交、不访问远端——但它会把界面绑定的仓库整个换掉。任何在途的被编排操作（只读预检、
// 执行前复核、命令窗口里那条命令、推送后的核实、上游写入）等的都是「它自己核对过的那份现状」，
// 仓库一换，回来的结果就不属于那个现状了。因此这里比写操作之间的互斥更严：五个流程只要有
// 任何一个在走、或命令窗口槽被占，导航就拒绝，并点名是谁占着。返回空串 = 可以导航。
// 放宽其中任何一条之前，必须先想清楚「哪一步的等结果会因此落到别的仓库上」。
[[nodiscard]] std::wstring DescribeNavigationRefusal(const WritePrerequisites& prerequisites,
                                                     const GitFlowActivity& activity,
                                                     std::wstring_view actionLabel);

}  // namespace gc::app
