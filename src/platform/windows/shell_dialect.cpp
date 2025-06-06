#include "platform/shell_dialect.h"

namespace gc::platform {

git::ShellDialect DefaultShellDialect() { return git::ShellDialect::cmdInteractive; }

}  // namespace gc::platform
