#pragma once

#include <string>

namespace gc::app {

// 提交表單的「這份內容是誰寫的」記錄。純狀態機，不碰 Win32，因此可以逐條測試。
//
// 為什麼需要它：作者欄的初值來自這個倉庫的有效 Git 配置（背景查詢），
// 而刷新與換倉庫都會再拿到一次默認值。若只用「輸入框是不是空的」判斷，
// 會出現兩種壞行為：
//   * 使用者正在打的字被遲來的默認值蓋掉；
//   * 上個倉庫的默認作者被当成「還可以用默認值覆蓋」，換倉庫後悄悄留著舊身份。
// 所以這裡把「默認值填進去的」與「使用者親手改過的」分成兩種狀態。
//
// 欄位文字一律用 std::wstring（界面與 Windows 邊界的表示）；
// 轉成 UTF-8 交给 Git 是後續「写提交信息档案」那一步的事。
class CommitFormSession {
public:
  // 界面裡會構成本表單內容的四個欄位。
  enum class Field {
    subject,
    description,
    author,
    coauthors,
  };

  // 使用者實際編輯過某個欄位（鍵入、列表增刪）。作者欄一旦被標記，
  // 之後的默認值就不再覆蓋它，直到 ChooseDiscard() 或 Reset()。
  void NoteUserEdit(Field field);

  // 該欄位目前是不是「使用者寫的內容」。只被默認值填過不算（見 AuthorDefaultApplied）。
  [[nodiscard]] bool IsUserContent(Field field) const noexcept;
  // 整個表單有沒有任何使用者內容——決定換倉庫時要不要先問一句。
  [[nodiscard]] bool HasUserContent() const noexcept;

  // 默認作者值能不能落地：只有使用者從未動過作者欄時才允許。
  [[nodiscard]] bool AcceptsAuthorDefault() const noexcept;
  // 使用者把作者欄清空＝主動要「配置裡的默認身份」。沒有這條退路，
  // 動过一次作者欄就再也拿不到默認值，使用者只能反覆手打同一個身份。
  void NoteAuthorCleared();
  // 記下「這串文字是默認值填進去的」；之後的刷新若拿到同一個值就不必再動輸入框。
  void NoteAuthorDefaultApplied(const std::wstring& identity);
  [[nodiscard]] const std::wstring& AppliedAuthorDefault() const noexcept;
  [[nodiscard]] bool AuthorDefaultApplied() const noexcept;

  // 換到 candidateKey 這個工作區之前：表單裡有使用者內容、而且原本已綁定另一個工作區時，
  // 必須由使用者決定保留還是放棄，程序不替人猜。返回 false 表示直接綁過去即可。
  [[nodiscard]] bool NeedsSwitchDecision(const std::wstring& candidateKey) const;

  // 保留：內容一律不動，但原本只是「默認值填進去」的作者轉成使用者內容
  // （它是上個倉庫的配置讀出來的，留下來就等於使用者認可的文字，
  //  既不能在新倉庫拿到默認值時被悄悄蓋掉，也不該被当成新倉庫的默認值）。
  void ChooseKeep(const std::wstring& newKey);
  // 放棄：髒記號與默認值記錄一起清空，作者欄回到「等待新倉庫默認值」的狀態。
  void ChooseDiscard(const std::wstring& newKey);
  // 沒有使用者內容時的直接綁定（或首次綁定）：不動任何欄位狀態。
  void BindRepository(const std::wstring& key);
  [[nodiscard]] const std::wstring& BoundRepository() const noexcept;

  // 表單被整體清空／重設（例如「放棄」之後界面重新填預設值）時調用。
  void Reset();

private:
  static constexpr size_t kFieldCount = 4;
  [[nodiscard]] static size_t Index(Field field) noexcept;

  bool edited_[kFieldCount] = {false, false, false, false};
  bool authorDefaultApplied_ = false;
  std::wstring appliedAuthorDefault_;
  std::wstring boundKey_;
};

}  // namespace gc::app
