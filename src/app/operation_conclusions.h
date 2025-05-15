#pragma once

#include <string>

namespace gc::app {

// 命令窗口操作拿到终态之后、界面把结论写进「任务状态」之前，按操作追加的那几句范围承诺。
// 这些文案只随「成功与否」变化，是纯文字；收到 app 层是为了能被单元测试钉住——
// 每一句都对应 AGENTS.md 里的长期承诺（fetch 的范围、pull 两步各自的结论、
// 推送在核实回来之前不下「送到」的断言），改字面之前先确认那份承诺本身有没有变。

// fetch 终态：成功只承诺「远端跟踪引用按确认的范围更新」，失败明确不自动重试、不改配置。
[[nodiscard]] std::wstring DescribeFetchConclusion(bool succeeded);

// pull 第一步（获取）终态：成功之后还要问关系，失败就停在这一步。
[[nodiscard]] std::wstring DescribePullFetchConclusion(bool succeeded);

// pull 第二步（整合）成功的结论。失败侧不在这里：那份要以「现场读回来的实况」为准，
// 由界面的 pull 流程在后台读取回来后另行组织。
[[nodiscard]] std::wstring DescribePullIntegrateSuccessConclusion();

// 推送命令的终态结论：这里只写到「命令窗口那头说了什么」，并预告紧随其后的核实——
// Git 报告成功不等于远端收到了那一份提交，真正的「送到没送到」以核实为准。
[[nodiscard]] std::wstring DescribePushCommandConclusion(bool succeeded);

// 冲突流程「继续」成功的结论（失败侧不在这里：那份要以现场读回来的实况为准，
// 由界面的冲突流程控制器在后台读取回来后另行组织）。成功只按退出码说话，
// 「痕迹清没清、还剩几个未合并文件」交给紧随其后的重读，不在这里替 Git 打保票。
[[nodiscard]] std::wstring DescribeConflictContinueSuccessConclusion();

// 冲突流程「中止」成功的结论。同样只写「那条命令按退出码成了」与本程序没做的事：
// 工作区与索引被 Git 改成什么样，以命令窗口里的输出与重读结果为准。
[[nodiscard]] std::wstring DescribeConflictAbortSuccessConclusion();

}  // namespace gc::app
