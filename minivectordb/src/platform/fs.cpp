#include "platform/fs.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace mvdb {
namespace {

#ifdef _WIN32
constexpr char kSeparator = '\\';
#else
constexpr char kSeparator = '/';
#endif

bool IsSeparator(char c) {
#ifdef _WIN32
  return c == '\\' || c == '/';
#else
  return c == '/';
#endif
}

}  // namespace

bool DirectoryExists(const std::string& path) {
  if (path.empty()) return false;
  struct stat info {};
  if (stat(path.c_str(), &info) != 0) return false;
  return (info.st_mode & S_IFDIR) != 0;
}

bool EnsureDirectory(const std::string& path) {
  if (path.empty()) return false;
  if (DirectoryExists(path)) return true;

  // Walk the path creating each missing component in turn.
  std::string current;
  current.reserve(path.size());
  size_t index = 0;

  // Preserve a leading root ("/" or "C:\") instead of trying to create it.
  if (path.size() >= 2 && path[1] == ':') {
    current.append(path, 0, 2);
    index = 2;
  } else if (IsSeparator(path[0])) {
    current.push_back(path[0]);
    index = 1;
  }

  while (index <= path.size()) {
    const size_t next = path.find_first_of("/\\", index);
    const size_t end = (next == std::string::npos) ? path.size() : next;
    if (end > index) {
      if (!current.empty() && !IsSeparator(current.back())) current.push_back(kSeparator);
      current.append(path, index, end - index);
      if (!DirectoryExists(current)) {
#ifdef _WIN32
        if (_mkdir(current.c_str()) != 0) return false;
#else
        if (mkdir(current.c_str(), 0755) != 0) return false;
#endif
      }
    }
    if (next == std::string::npos) break;
    index = next + 1;
  }
  return DirectoryExists(path);
}

std::string JoinPath(const std::string& base, const std::string& child) {
  if (base.empty()) return child;
  std::string result = base;
  if (!IsSeparator(result.back())) result.push_back(kSeparator);
  result.append(child);
  return result;
}

std::string ExecutableDirectory() {
#ifdef _WIN32
  std::vector<char> buffer(MAX_PATH);
  while (true) {
    const DWORD length = GetModuleFileNameA(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0) return {};
    if (length < buffer.size()) {
      std::string path(buffer.data(), length);
      const size_t cut = path.find_last_of("/\\");
      return cut == std::string::npos ? std::string() : path.substr(0, cut);
    }
    buffer.resize(buffer.size() * 2);
  }
#else
  std::vector<char> buffer(4096);
  const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length <= 0) return {};
  std::string path(buffer.data(), static_cast<size_t>(length));
  const size_t cut = path.find_last_of('/');
  return cut == std::string::npos ? std::string() : path.substr(0, cut);
#endif
}

bool ReadFileToString(const std::string& path, std::string* out) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return false;
  out->clear();
  char buffer[65536];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out->append(buffer, got);
  }
  std::fclose(file);
  return true;
}

bool WriteFileAtomically(const std::string& path, const std::string& content) {
  const std::string temporary = path + ".tmp";
  std::FILE* file = std::fopen(temporary.c_str(), "wb");
  if (file == nullptr) {
    std::fprintf(stderr, "[mvdb] fopen failed: %s\n", temporary.c_str());
    return false;
  }

  const size_t written = std::fwrite(content.data(), 1, content.size(), file);
  const bool flushed = std::fflush(file) == 0;
  std::fclose(file);

  if (written != content.size() || !flushed) {
    std::fprintf(stderr, "[mvdb] short write: %s (%zu of %zu)\n", temporary.c_str(),
                 written, content.size());
    std::remove(temporary.c_str());
    return false;
  }

#ifdef _WIN32
  if (MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
    std::fprintf(stderr, "[mvdb] rename failed: %s -> %s (error %lu)\n", temporary.c_str(),
                 path.c_str(), static_cast<unsigned long>(GetLastError()));
    return false;
  }
  return true;
#else
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::fprintf(stderr, "[mvdb] rename failed: %s -> %s\n", temporary.c_str(),
                 path.c_str());
    return false;
  }
  return true;
#endif
}

bool RemoveFile(const std::string& path) {
  if (std::remove(path.c_str()) == 0) return true;
  // Distinguish "already gone" from "cannot remove" by probing.
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return true;
  std::fclose(file);
  return false;
}

}  // namespace mvdb
