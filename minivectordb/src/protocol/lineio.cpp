#include "protocol/lineio.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace mvdb {
namespace {

// Upper bound on a single protocol line. The judge caps responses at 16 MiB;
// requests are tiny. Keeping a ceiling stops a malformed stream from eating RAM.
constexpr size_t kMaxLineBytes = 32u * 1024u * 1024u;

std::string g_buffer;
size_t g_offset = 0;
bool g_eof = false;

// Reads whatever is available on stdin, *without* waiting to fill the buffer.
//
// fread() must not be used here: on a pipe it loops until the requested count
// is satisfied, so a 32 KiB request would block forever after a 40-byte
// "shutdown" line. The judge writes one line and then waits for the answer,
// which would deadlock both sides.
long ReadAvailable(char* buffer, size_t capacity, int* saved_errno) {
  if (saved_errno != nullptr) *saved_errno = 0;
#ifdef _WIN32
  const int fd = _fileno(stdin);
  const int got = _read(fd, buffer, static_cast<unsigned int>(capacity));
  if (got < 0 && saved_errno != nullptr) *saved_errno = errno;
#else
  const int fd = fileno(stdin);
  ssize_t got;
  do {
    got = ::read(fd, buffer, capacity);
  } while (got < 0 && errno == EINTR);
  if (got < 0 && saved_errno != nullptr) *saved_errno = errno;
#endif
  return static_cast<long>(got);
}

}  // namespace

void InitStdio() {
#ifdef _WIN32
  // Without this, "\n" becomes "\r\n" and the judge would see stray bytes.
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  // Output may buffer freely because WriteLine always flushes.
  std::setvbuf(stdout, nullptr, _IOFBF, 1u << 16);
}

bool ReadLine(std::string* out) {
  out->clear();
  char chunk[65536];
  int read_errno = 0;
  while (true) {
    const size_t newline = g_buffer.find('\n', g_offset);
    if (newline != std::string::npos) {
      out->assign(g_buffer, g_offset, newline - g_offset);
      g_offset = newline + 1;
      if (!out->empty() && out->back() == '\r') out->pop_back();
      return true;
    }
    if (g_eof) {
      if (g_offset < g_buffer.size()) {
        out->assign(g_buffer, g_offset, std::string::npos);
        g_offset = g_buffer.size();
        if (!out->empty() && out->back() == '\r') out->pop_back();
        return !out->empty();
      }
      return false;
    }
    // Drop the already-consumed prefix before growing the buffer.
    if (g_offset > 0) {
      g_buffer.erase(0, g_offset);
      g_offset = 0;
    }
    const long got = ReadAvailable(chunk, sizeof(chunk), &read_errno);
    if (got <= 0) {
      // One line, once: a silent exit here used to be indistinguishable from a
      // clean shutdown, and an intermittent case died exactly this way.
      if (!g_eof) {
        std::fprintf(stderr, "[mvdb] stdin read returned %ld errno=%d\n", got,
                     read_errno);
      }
      g_eof = true;
      continue;
    }
    g_buffer.append(chunk, static_cast<size_t>(got));
    if (g_buffer.size() > kMaxLineBytes) {
      g_eof = true;
      g_buffer.clear();
      g_offset = 0;
      return false;
    }
  }
}

void WriteLine(const std::string& line) {
  size_t written = 0;
  if (!line.empty()) written = std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  // A failed flush used to be ignored, which loses the response while the
  // process still exits "successfully" - the judge then sees a truncated stream
  // and reports that the database went away.
  const bool flushed = std::fflush(stdout) == 0;
  if ((!line.empty() && written != line.size()) || !flushed) {
    std::fprintf(stderr, "[mvdb] stdout write failed: wrote %zu of %zu\n", written,
                 line.size());
  }
}

}  // namespace mvdb
