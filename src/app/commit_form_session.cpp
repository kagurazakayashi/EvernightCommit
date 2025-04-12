#include "app/commit_form_session.h"

namespace gc::app {

size_t CommitFormSession::Index(Field field) noexcept {
  switch (field) {
    case Field::subject:
      return 0;
    case Field::description:
      return 1;
    case Field::author:
      return 2;
    case Field::coauthors:
      return 3;
  }
  return 0;  // 列舉已窮盡；這裡只為讓編譯器不抱怨「控制路徑到達函數結尾」。
}

void CommitFormSession::NoteUserEdit(Field field) {
  edited_[Index(field)] = true;
  if (field == Field::author) {
    // 使用者的文字蓋在默認值之上：這份內容不再屬於「默認值」，之後的刷新不得再覆蓋它。
    authorDefaultApplied_ = false;
    appliedAuthorDefault_.clear();
  }
}

bool CommitFormSession::IsUserContent(Field field) const noexcept {
  // 只被默認值填過的欄位不算使用者內容：edited_ 只在 NoteUserEdit / ChooseKeep 裡置位。
  return edited_[Index(field)];
}

bool CommitFormSession::HasUserContent() const noexcept {
  for (size_t index = 0; index < kFieldCount; ++index) {
    if (edited_[index]) {
      return true;
    }
  }
  return false;
}

bool CommitFormSession::AcceptsAuthorDefault() const noexcept {
  return !edited_[Index(Field::author)];
}

void CommitFormSession::NoteAuthorCleared() {
  // 清空是主動回退：作者欄重新回到「等默認值來填」，之前的編輯記號一併解除。
  edited_[Index(Field::author)] = false;
  authorDefaultApplied_ = false;
  appliedAuthorDefault_.clear();
}

void CommitFormSession::NoteAuthorDefaultApplied(const std::wstring& identity) {
  appliedAuthorDefault_ = identity;
  authorDefaultApplied_ = true;
}

const std::wstring& CommitFormSession::AppliedAuthorDefault() const noexcept {
  return appliedAuthorDefault_;
}

bool CommitFormSession::AuthorDefaultApplied() const noexcept {
  return authorDefaultApplied_;
}

bool CommitFormSession::NeedsSwitchDecision(const std::wstring& candidateKey) const {
  if (boundKey_ == candidateKey) {
    return false;  // 同一個工作區（例如刷新時 Git 換了寫法但根沒變）。
  }
  if (boundKey_.empty()) {
    return false;  // 還沒有任何綁定（首次識別成功）：沒有「舊內容」需要保護。
  }
  return HasUserContent();
}

void CommitFormSession::ChooseKeep(const std::wstring& newKey) {
  boundKey_ = newKey;
  if (authorDefaultApplied_) {
    // 上個倉庫的默認作者被留下來：從現在起它是使用者內容，
    // 新倉庫的默認值不得悄悄蓋掉它，否則「保留」就成了空話。
    authorDefaultApplied_ = false;
    appliedAuthorDefault_.clear();
    edited_[Index(Field::author)] = true;
  }
}

void CommitFormSession::ChooseDiscard(const std::wstring& newKey) {
  Reset();
  boundKey_ = newKey;
}

void CommitFormSession::BindRepository(const std::wstring& key) {
  boundKey_ = key;
}

const std::wstring& CommitFormSession::BoundRepository() const noexcept {
  return boundKey_;
}

void CommitFormSession::Reset() {
  for (bool& flag : edited_) {
    flag = false;
  }
  authorDefaultApplied_ = false;
  appliedAuthorDefault_.clear();
}

}  // namespace gc::app
