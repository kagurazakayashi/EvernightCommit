#pragma once

// Windows 资源 ID 定义。
//
// 该头文件同时被 C++ 源码（#include "ui/resource_ids.h"）和资源脚本
// resources/app.rc.in（#include "resource_ids.h"，CMake 会把本文件复制到
// 生成目录 app.rc 旁边）引用，保证资源编号只有一处定义。

// 应用程序图标，对应 resources/EvernightCommit.ico，同时用于窗口图标与可执行文件图标。
#define IDI_APP_ICON 101

// 身份输入框（添加/修改合作者用）。模板在 resources/app.rc.in 里，
// 只放控件与占位几何；所有文字由 C++ 在 WM_INITDIALOG 里按 UTF-16 设置，
// 因此资源脚本本身不需要任何非 ASCII 字符。
#define IDD_COMMIT_IDENTITY_DIALOG 201
#define IDC_IDENTITY_LABEL 1001
#define IDC_IDENTITY_EDIT 1002
#define IDC_IDENTITY_NOTE 1003

// fetch 的远端选择框。同上：模板只放控件与占位几何，文字全部由 C++ 设置。
#define IDD_FETCH_REMOTE_DIALOG 202
#define IDC_REMOTE_LABEL 1004
#define IDC_REMOTE_LIST 1005
