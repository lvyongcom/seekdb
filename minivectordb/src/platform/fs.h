// Filesystem helpers with the Windows/POSIX split confined to one file.
//
// The submission image runs on Linux, but development happens on Windows, so
// every OS-specific call lives behind these wrappers instead of leaking
// #ifdefs into the storage layer.
#pragma once

#include <cstdint>
#include <string>

namespace mvdb {

// Creates `path` and any missing parents. Returns true if the directory exists
// afterwards (including the already-existed case).
bool EnsureDirectory(const std::string& path);

bool DirectoryExists(const std::string& path);

std::string JoinPath(const std::string& base, const std::string& child);

// Absolute path of the directory holding the running executable, without a
// trailing separator. Empty on failure.
std::string ExecutableDirectory();

// Reads a whole file into `out`. Returns false if it cannot be opened.
bool ReadFileToString(const std::string& path, std::string* out);

// Replaces `path` with `content` atomically: write a sibling temp file, flush,
// then rename over the target. A crash leaves either the old or the new file,
// never a half-written one.
bool WriteFileAtomically(const std::string& path, const std::string& content);

// Removes `path`. Returns true when the file is gone afterwards.
bool RemoveFile(const std::string& path);

}  // namespace mvdb
