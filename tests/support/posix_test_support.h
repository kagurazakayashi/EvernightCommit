#pragma once

// Linux 平台测试共用的临时目录与环境变量守卫（只在 Linux 目标里编译）。
// 所有路径都在测试自己创建的临时根内；析构时递归清理，设置 GC_TEST_KEEP_TEMP=1 可保留现场。

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "platform/text_encoding.h"
#include "support/tiny_test.h"

namespace gc::test {

class ScopedEnvironmentVariable {
public:
  explicit ScopedEnvironmentVariable(std::string name, std::optional<std::string> value = std::nullopt)
      : name_(std::move(name)) {
    const char* existing = std::getenv(name_.c_str());
    if (existing != nullptr) {
      hadValue_ = true;
      oldValue_ = existing;
    }
    if (value.has_value()) {
      ::setenv(name_.c_str(), value->c_str(), 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }

  ~ScopedEnvironmentVariable() {
    if (hadValue_) {
      ::setenv(name_.c_str(), oldValue_.c_str(), 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }

  ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
  ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
  std::string name_;
  std::string oldValue_;
  bool hadValue_ = false;
};

// 临时切换进程当前目录；析构恢复。测试串行运行，因此不会与其他用例互相干扰。
class ScopedCurrentDirectory {
public:
  explicit ScopedCurrentDirectory(const std::string& target) {
    char buffer[4096];
    if (::getcwd(buffer, sizeof(buffer)) != nullptr) {
      previous_ = buffer;
      valid_ = ::chdir(target.c_str()) == 0;
    }
  }
  ~ScopedCurrentDirectory() {
    if (!previous_.empty()) {
      static_cast<void>(::chdir(previous_.c_str()));
    }
  }
  ScopedCurrentDirectory(const ScopedCurrentDirectory&) = delete;
  ScopedCurrentDirectory& operator=(const ScopedCurrentDirectory&) = delete;
  [[nodiscard]] bool Valid() const noexcept { return valid_; }

private:
  std::string previous_;
  bool valid_ = false;
};

class PosixTempDirectory {
public:
  PosixTempDirectory() {
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    if (error) {
      reason_ = "无法取得系统临时目录";
      return;
    }
    std::string pattern = (base / "gc-test-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
      reason_ = "mkdtemp 失败";
      return;
    }
    path_ = std::move(pattern);
    owned_ = true;
  }

  ~PosixTempDirectory() {
    if (!owned_) {
      return;
    }
    const char* keep = std::getenv("GC_TEST_KEEP_TEMP");
    if (keep != nullptr && std::string_view(keep) == "1") {
      return;
    }
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  PosixTempDirectory(const PosixTempDirectory&) = delete;
  PosixTempDirectory& operator=(const PosixTempDirectory&) = delete;

  [[nodiscard]] bool Valid() const noexcept { return owned_; }
  [[nodiscard]] const std::string& PathBytes() const noexcept { return path_; }
  [[nodiscard]] std::wstring Path() const {
    std::wstring text;
    static_cast<void>(gc::platform::TryWidePathFromBytes(path_, text));
    return text;
  }
  [[nodiscard]] std::string PathIn(std::string_view relative) const {
    return path_ + "/" + std::string(relative);
  }
  [[nodiscard]] std::wstring WidePathIn(std::string_view relative) const {
    return Path() + L"/" + gc::platform::Utf8ToWide(relative);
  }
  [[nodiscard]] const std::string& FailureReason() const noexcept { return reason_; }

private:
  std::string path_;
  std::string reason_;
  bool owned_ = false;
};

// 建目录；已存在视为成功。失败直接以测试前置条件失败中止。
inline void RequireMakeDirectory(const std::string& path, mode_t mode = 0755) {
  if (::mkdir(path.c_str(), mode) != 0 && errno != EEXIST) {
    GC_REQUIRE_MESSAGE(false, "建目录失败：" + path + "（errno " + std::to_string(errno) + "）");
  }
}

inline void RequireWriteFile(const std::string& path, std::string_view bytes, mode_t mode = 0600) {
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  GC_REQUIRE_MESSAGE(descriptor >= 0, "建文件失败：" + path + "（errno " + std::to_string(errno) + "）");
  size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t count = ::write(descriptor, bytes.data() + written, bytes.size() - written);
    if (count <= 0) {
      const int error = errno;
      ::close(descriptor);
      GC_REQUIRE_MESSAGE(false, "写文件失败：" + path + "（errno " + std::to_string(error) + "）");
    }
    written += static_cast<size_t>(count);
  }
  ::close(descriptor);
}

inline void RequireReadFile(const std::string& path, std::string& out) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  GC_REQUIRE_MESSAGE(descriptor >= 0, "读文件失败：" + path + "（errno " + std::to_string(errno) + "）");
  out.clear();
  char buffer[4096];
  for (;;) {
    const ssize_t count = ::read(descriptor, buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int error = errno;
      ::close(descriptor);
      GC_REQUIRE_MESSAGE(false, "读文件失败：" + path + "（errno " + std::to_string(error) + "）");
    }
    if (count == 0) {
      break;
    }
    out.append(buffer, static_cast<size_t>(count));
  }
  ::close(descriptor);
}

inline void RequireSymlink(const std::string& target, const std::string& link) {
  if (::symlink(target.c_str(), link.c_str()) != 0) {
    GC_REQUIRE_MESSAGE(false, "建符号链接失败：" + link + "（errno " + std::to_string(errno) + "）");
  }
}

}  // namespace gc::test
