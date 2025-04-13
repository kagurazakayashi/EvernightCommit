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

  session.NoteCommitted();
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
  defaultsOnly.NoteCommitted();
  GC_CHECK_MESSAGE(!defaultsOnly.HasUserContent(),
                   "只剩默认作者时，提交后的空表单不该在换仓库时白问一句");
}

GC_TEST(commit_form_session_default_remembered_value_is_comparable) {
  CommitFormSession session;
  session.NoteAuthorDefaultApplied(L"张三 <z@e.com>");
  GC_CHECK_MESSAGE(session.AppliedAuthorDefault() == L"张三 <z@e.com>",
                   "记住的就是实际填进去的那串，界面据此判断值有没有变");
  session.NoteAuthorDefaultApplied(L"张三 <z@e.com>");
  GC_CHECK_MESSAGE(session.AppliedAuthorDefault() == L"张三 <z@e.com>", "重复填同一串不产生变化");
}
