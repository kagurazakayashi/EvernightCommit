#pragma once

#include <string>
#include <utility>

#include "git/author_config.h"
#include "platform/windows/git_task_worker.h"

namespace gc::platform {

// 一次「讀取有效 Git 身份」的請求。工作區根取識別結果，不取輸入框原文
// （使用者可能選了子目錄），並在結果裡原樣回顯，讓界面執行緒得以判別
// 「這份結果還是這個倉庫的嗎」——舊結果的作廢由 GitTaskWorker 的序號負責，
// 而「同一序號但倉庫已被換掉」要靠這裡回顯的目錄兜住。
struct AuthorConfigRequest {
  std::wstring exePath;
  std::wstring repositoryDirectory;
  unsigned long timeoutMilliseconds = 0;
};

// 一次背景讀取的成品：判讀結果加上請求攜帶的目錄回顯。
struct AuthorConfigOutcome {
  git::AuthorIdentityConfig config;
  std::wstring repositoryDirectory;
};

struct AuthorConfigDeps {
  git::GitQueryRunner runner;
};

// 只讀地問 Git「這個倉庫現在用哪個身份」：user.name 與 user.email 各一次查詢，
// 優先序完全由 Git 決定（倉庫本體設定、使用者設定、系統設定、includeIf 都由 Git 自己處理）。
// 本函數不寫任何設定檔，也不提供寫回的路徑。
[[nodiscard]] git::AuthorIdentityConfig LoadAuthorIdentity(const AuthorConfigRequest& request,
                                                           const AuthorConfigDeps& deps);

// 用本工程的隱藏窗口子進程執行器裝配 AuthorConfigDeps。
[[nodiscard]] AuthorConfigDeps MakeAuthorConfigDeps(unsigned long timeoutMilliseconds);

// 工作線程任務體：裝配依賴並執行兩次查詢，請求攜帶的序號與目錄原樣帶進結果。
[[nodiscard]] AuthorConfigOutcome RunAuthorIdentityLoad(const AuthorConfigRequest& request);

using AuthorConfigWorker = GitTaskWorker<AuthorConfigRequest, AuthorConfigOutcome>;

}  // namespace gc::platform
