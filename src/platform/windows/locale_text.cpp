#include "platform/windows/locale_text.h"

namespace gc::platform {

SYSTEMTIME CurrentLocalTime() noexcept {
  SYSTEMTIME now{};
  ::GetLocalTime(&now);
  return now;
}

}  // namespace gc::platform
