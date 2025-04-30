// 提交表单会话状态的纯逻辑测试：只管「这份内容是谁写的、换仓库时要不要问」这一件事。
// 覆盖：默认值不算用户内容、用户编辑后不再被默认值覆盖、清空作者栏重新接受默认值、
// 保留时旧默认值转成用户内容、放弃时记号全部解除，同一仓库刷新不重复追问，
// 以及提交成功后只清正文三个栏位、作者与绑定仓库都不动。
#include <string>

#include "app/commit_form_session.h"
#include "support/tiny_test.h"

namespace {

using gc::app::CommitFormSession;
using Field = gc::app::CommitFormSession::Field;

}  // namespace

GC_TEST(commit_form_session_starts_empty) {
  CommitFormSession session;
  GC_CHECK_MESSAGE(!session.HasUserContent(), "新会话没有任何用户内容");
  GC_CHECK_MESSAGE(session.AcceptsAuthorDefault(), "作者栏等着默认值来填");
  GC_CHECK_MESSAGE(!session.AuthorDefaultApplied(), "还没填过就不算已应用默认值");
  GC_CHECK_MESSAGE(!session.NeedsSwitchDecision(L"P:\\any\\repo"),
                   "还没有绑定过仓库时不需要问（没有旧内容会被弄丢）");
}

GC_TEST(commit_form_session_user_edit_blocks_default) {
  CommitFormSession session;
  session.BindRepository(L"P:\\repo-a");
  session.NoteAuthorDefaultApplied(L"张三 <z@e.com>");
  GC_CHECK_MESSAGE(session.AuthorDefaultApplied(), "记住这串是默认值填的");
  GC_CHECK_MESSAGE(!session.HasUserContent(), "默认值填的内容不该被当成用户输入");
  GC_CHECK_MESSAGE(!session.NeedsSwitchDecision(L"P:\\repo-b"),
                   "只有默认作者时换仓库不必追问，直接改用新仓库的默认值");

  session.NoteUserEdit(Field::author);
  GC_CHECK_MESSAGE(!session.AcceptsAuthorDefault(), "用户动过作者栏后，默认值不得再覆盖它");
  GC_CHECK_MESSAGE(!session.AuthorDefaultApplied(), "覆盖记号同时解除：现在的内容属于用户");
  GC_CHECK_MESSAGE(session.HasUserContent(), "作者栏现在是用户内容");
  GC_CHECK_MESSAGE(session.NeedsSwitchDecision(L"P:\\repo-b"), "换到别的仓库要先问");
  GC_CHECK_MESSAGE(!session.NeedsSwitchDecision(L"P:\\repo-a"),
                   "还绑在同一个仓库上（例如点刷新）就不该追问");
}

GC_TEST(commit_form_session_other_fields_count_as_user_content) {
  CommitFormSession session;
  session.BindRepository(L"P:\\repo-a");
  session.NoteUserEdit(Field::subject);
  GC_CHECK_MESSAGE(session.HasUserContent(), "标题是用户内容");
  GC_CHECK_MESSAGE(session.IsUserContent(Field::subject), "按栏位查询要能对上");
  GC_CHECK_MESSAGE(!session.IsUserContent(Field::description), "别的栏位没动就仍是未编辑");
  GC_CHECK_MESSAGE(session.AcceptsAuthorDefault(), "编辑标题不影响作者栏的默认值资格");

  session.NoteUserEdit(Field::coauthors);
  GC_CHECK_MESSAGE(session.IsUserContent(Field::coauthors), "合作者列表的增删也算用户内容");
}

GC_TEST(commit_form_session_keep_promotes_old_default) {
  CommitFormSession session;
  session.BindRepository(L"P:\\repo-a");
  session.NoteAuthorDefaultApplied(L"张三 <z@e.com>");  // 上个仓库的默认作者
  session.NoteUserEdit(Field::description);            // 用户只写了描述
  GC_CHECK_MESSAGE(session.NeedsSwitchDecision(L"P:\\repo-b"), "有描述在手，换仓库要问");

  session.ChooseKeep(L"P:\\repo-b");
  GC_CHECK_MESSAGE(session.BoundRepository() == L"P:\\repo-b", "保留之后要认下新仓库，避免反复追问");
  GC_CHECK_MESSAGE(session.IsUserContent(Field::description), "保留：用户写的描述仍然是用户内容");
  // 这一条是「避免旧作者被误带进新仓库」的关键：用户选保留，就意味着这段文字归他负责，
  // 新仓库的默认值不能再悄悄盖掉它，否则「保留」成了空话。
  GC_CHECK_MESSAGE(!session.AcceptsAuthorDefault(), "保留下来的旧默认值转为作者为用户内容");
  GC_CHECK_MESSAGE(!session.AuthorDefaultApplied(), "它不再被记成「本仓库的默认值」");
}

GC_TEST(commit_form_session_discard_clears_everything) {
  CommitFormSession session;
  session.BindRepository(L"P:\\repo-a");
  session.NoteAuthorDefaultApplied(L"张三 <z@e.com>");
  session.NoteUserEdit(Field::subject);
  session.NoteUserEdit(Field::coauthors);

  session.ChooseDiscard(L"P:\\repo-b");
  GC_CHECK_MESSAGE(!session.HasUserContent(), "放弃之后没有任何用户内容");
  GC_CHECK_MESSAGE(session.AcceptsAuthorDefault(), "作者栏重新等着新仓库的默认值");
  GC_CHECK_MESSAGE(session.AppliedAuthorDefault().empty(), "旧默认值的记录要一并清掉");
  GC_CHECK_MESSAGE(session.BoundRepository() == L"P:\\repo-b", "放弃之后同样认下新仓库");
  GC_CHECK_MESSAGE(!session.NeedsSwitchDecision(L"P:\\repo-c"),
                   "放弃之后再换仓库也不该追问：此时没有任何用户内容");
}

GC_TEST(commit_form_session_clearing_author_asks_default_again) {
  CommitFormSession session;
  session.BindRepository(L"P:\\repo-a");
  session.NoteUserEdit(Field::author);
  GC_CHECK_MESSAGE(!session.AcceptsAuthorDefault(), "动过之后不再接受默认值");
  session.NoteUserEdit(Field::subject);

  session.NoteAuthorCleared();
  GC_CHECK_MESSAGE(session.AcceptsAuthorDefault(),
                   "用户把作者栏清空＝主动要配置里的默认身份，这是唯一的退路");
  GC_CHECK_MESSAGE(session.AppliedAuthorDefault().empty(), "清空后旧默认值记录解除");
  GC_CHECK_MESSAGE(session.HasUserContent(), "标题里的用户内容不会因为清空作者而被抹掉");
}

GC_TEST(commit_form_session_committed_clears_body_keeps_author) {
  CommitFormSession session;
  session.BindRepository(L"P:\\repo-a");
  session.NoteUserEdit(Field::subject);
  session.NoteUserEdit(Field::description);
  session.NoteUserEdit(Field::coauthors);
  session.NoteUserEdit(Field::author);
  session.NoteAuthorDefaultApplied(L"张三 <z@e.com>");
  GC_CHECK_MESSAGE(session.HasUserContent(), "四个欄位都动过");

  session.NoteCommitted(true, true, true);
  GC_CHECK_MESSAGE(!session.IsUserContent(Field::subject), "已经提交出去的标题不再是用户内容");
  GC_CHECK_MESSAGE(!session.IsUserContent(Field::description), "描述同上");
  GC_CHECK_MESSAGE(!session.IsUserContent(Field::coauthors), "合作者同上");
  GC_CHECK_MESSAGE(session.IsUserContent(Field::author),
                   "作者留着继续用，它属于用户这件事不能因为提交成功而改变");
  GC_CHECK_MESSAGE(!session.AcceptsAuthorDefault(), "用户写过的作者不会被仓库默认值悄悄盖掉");
  GC_CHECK_MESSAGE(session.BoundRepository() == L"P:\\repo-a", "绑定的仓库没有变");
  GC_CHECK_MESSAGE(!session.NeedsSwitchDecision(L"P:\\repo-a"),
                   "同一仓库刷新不追问");
  GC_CHECK_MESSAGE(session.NeedsSwitchDecision(L"P:\\repo-b"),
                   "作者还是用户内容，换仓库仍要先问");

  // 作者也只是默认值填的话，提交后一张空表不该再被当成「有用户内容」。
  CommitFormSession defaultsOnly;
  defaultsOnly.BindRepository(L"P:\\repo-a");
  defaultsOnly.NoteAuthorDefaultApplied(L"张三 <z@e.com>");
  defaultsOnly.NoteUserEdit(Field::subject);
  defaultsOnly.NoteCommitted(true, true, true);
  GC_CHECK_MESSAGE(!defaultsOnly.HasUserContent(),
                   "只剩默认作者时，提交后的空表单不该在换仓库时白问一句");
}

GC_TEST(commit_form_session_committed_keeps_marks_for_fields_not_cleared) {
  // 提交跑完之前用户又写了新草稿：那一栏并没有被这次提交清空，「是你写的」记号必须留着，
  // 否则它会被当成可以悄悄覆盖的默认值，或在换仓库时被误判成「没有用户内容」。
  CommitFormSession session;
  session.BindRepository(L"P:\\repo-a");
  session.NoteUserEdit(Field::subject);
  session.NoteUserEdit(Field::description);
  session.NoteUserEdit(Field::coauthors);

  session.NoteCommitted(true, false, false);

  GC_CHECK_MESSAGE(!session.IsUserContent(Field::subject), "确实被清空的标题不再是用户内容");
  GC_CHECK_MESSAGE(session.IsUserContent(Field::description), "没被清空的描述仍然是用户内容");
  GC_CHECK_MESSAGE(session.IsUserContent(Field::coauthors), "没被清空的合作者仍然是用户内容");
  GC_CHECK_MESSAGE(session.NeedsSwitchDecision(L"P:\\repo-b"), "留着的新草稿换仓库时仍要先问");
}

GC_TEST(commit_form_session_default_remembered_value_is_comparable) {
  CommitFormSession session;
  session.NoteAuthorDefaultApplied(L"张三 <z@e.com>");
  GC_CHECK_MESSAGE(session.AppliedAuthorDefault() == L"张三 <z@e.com>",
                   "记住的就是实际填进去的那串，界面据此判断值有没有变");
  session.NoteAuthorDefaultApplied(L"张三 <z@e.com>");
  GC_CHECK_MESSAGE(session.AppliedAuthorDefault() == L"张三 <z@e.com>", "重复填同一串不产生变化");
}

// ---- 提交成功后的表单收尾判定（PlanCommittedFormCleanup）----

namespace {

gc::git::CommitFormData FormData(std::wstring subject, std::wstring description,
                                 std::vector<std::wstring> coauthors) {
  gc::git::CommitFormData data;
  data.subject = std::move(subject);
  data.description = std::move(description);
  data.coauthors = std::move(coauthors);
  return data;
}

}  // namespace

GC_TEST(form_cleanup_clears_only_unchanged_columns) {
  const auto committed = FormData(L"标题甲", L"描述乙", {L"张三 <z@e.com>"});

  // 屏幕上还是提交的那一份：三栏都清，时间回到此刻。
  const auto same = FormData(L"标题甲", L"描述乙", {L"张三 <z@e.com>"});
  const auto untouched = gc::app::PlanCommittedFormCleanup(committed, same, false);
  GC_CHECK(untouched.clearSubject);
  GC_CHECK(untouched.clearDescription);
  GC_CHECK(untouched.clearCoauthors);
  GC_CHECK(untouched.resetTimes);
  GC_CHECK_MESSAGE(untouched.note.find(L"已清空") != std::wstring::npos,
                   "全清时的说明要写明清了三样");

  // 提交跑的那段时间里用户又写了新标题：新草稿一个字节都不能被抹掉。
  const auto retitled = FormData(L"新的下一段标题", L"描述乙", {L"张三 <z@e.com>"});
  const auto kept = gc::app::PlanCommittedFormCleanup(committed, retitled, false);
  GC_CHECK_MESSAGE(!kept.clearSubject, "换过内容的标题不许清");
  GC_CHECK(kept.clearDescription);
  GC_CHECK_MESSAGE(kept.note.find(L"标题") != std::wstring::npos &&
                   kept.note.find(L"原样留着") != std::wstring::npos,
                   "说明必须点名哪一栏留着了");

  // 合作者列表逐条比对：多一条也算换过内容。
  const auto more = FormData(L"标题甲", L"描述乙", {L"张三 <z@e.com>", L"李四 <l@e.com>"});
  const auto coauthorsChanged = gc::app::PlanCommittedFormCleanup(committed, more, false);
  GC_CHECK_MESSAGE(!coauthorsChanged.clearCoauthors, "合作者列表变了就不许清");
}

GC_TEST(form_cleanup_keeps_user_picked_times) {
  const auto committed = FormData(L"标题甲", L"", {});
  const auto same = FormData(L"标题甲", L"", {});
  // 用户在这期间亲手改过时间：那是新的意图，「提交成功」不是把它抹掉的理由。
  const auto edited = gc::app::PlanCommittedFormCleanup(committed, same, true);
  GC_CHECK_MESSAGE(!edited.resetTimes, "用户改过的时间必须原样留着");
  GC_CHECK_MESSAGE(edited.note.find(L"没有重置") != std::wstring::npos,
                   "说明要把「没重置时间」说清楚");
}
