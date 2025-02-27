#pragma once

// Windows 资源 ID 定义。
//
// 该头文件同时被 C++ 源码（#include "ui/resource_ids.h"）和资源脚本
// resources/app.rc.in（#include "resource_ids.h"，CMake 会把本文件复制到
// 生成目录 app.rc 旁边）引用，保证资源编号只有一处定义。

// 应用程序图标，对应 resources/EvernightCommit.ico，同时用于窗口图标与可执行文件图标。
#define IDI_APP_ICON 101
