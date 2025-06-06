#include "platform/shell_dialect.h"

namespace gc::platform {

git::ShellDialect DefaultShellDialect() { return git::ShellDialect::posixSh; }

}  // namespace gc::platform
