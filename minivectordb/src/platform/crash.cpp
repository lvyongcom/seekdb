#include "platform/crash.h"

#include <csignal>
#include <cstdlib>
#include <exception>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace mvdb {
namespace {

constexpr size_t kStageBytes = 192;
char g_stage[kStageBytes] = "startup";

void AppendText(char* buffer, size_t capacity, size_t* position, const char* text) {
  if (text == nullptr) return;
  for (size_t i = 0; text[i] != '\0' && *position + 1 < capacity; ++i) {
    buffer[(*position)++] = text[i];
  }
}

void AppendNumber(char* buffer, size_t capacity, size_t* position, long long value) {
  if (*position + 1 >= capacity) return;
  if (value < 0) {
    buffer[(*position)++] = '-';
    value = -value;
  }
  char digits[24];
  int count = 0;
  if (value == 0) {
    digits[count++] = '0';
  } else {
    while (value > 0 && count < static_cast<int>(sizeof(digits))) {
      digits[count++] = static_cast<char>('0' + (value % 10));
      value /= 10;
    }
  }
  while (count > 0 && *position + 1 < capacity) {
    buffer[(*position)++] = digits[--count];
  }
}

void WriteStderr(const char* data, size_t size) {
#ifdef _WIN32
  _write(2, data, static_cast<unsigned int>(size));
#else
  ssize_t ignored = ::write(STDERR_FILENO, data, size);
  (void)ignored;
#endif
}

// Async-signal-safe by construction: no allocation, no printf, no locks.
void Report(const char* reason, int signal_number) {
  char buffer[320];
  size_t position = 0;
  AppendText(buffer, sizeof(buffer), &position, "\n[mvdb] FATAL: ");
  AppendText(buffer, sizeof(buffer), &position, reason);
  AppendText(buffer, sizeof(buffer), &position, " signal=");
  AppendNumber(buffer, sizeof(buffer), &position, signal_number);
  AppendText(buffer, sizeof(buffer), &position, " stage=");
  AppendText(buffer, sizeof(buffer), &position, g_stage);
  AppendText(buffer, sizeof(buffer), &position, "\n");
  WriteStderr(buffer, position);
}

extern "C" void OnFatalSignal(int signal_number) {
  Report("fatal signal", signal_number);
  // Restore the default action and re-raise so the exit status reflects the
  // signal instead of a plain return code.
  std::signal(signal_number, SIG_DFL);
  std::raise(signal_number);
}

void OnTerminate() {
  Report("uncaught exception", 0);
  std::abort();
}

}  // namespace

void InstallCrashHandlers() {
  std::signal(SIGSEGV, OnFatalSignal);
  std::signal(SIGABRT, OnFatalSignal);
  std::signal(SIGFPE, OnFatalSignal);
  std::signal(SIGILL, OnFatalSignal);
#ifndef _WIN32
  // Writing to a pipe whose reader has gone away raises SIGPIPE, whose default
  // action kills the process with no diagnostic at all. Ignoring it turns that
  // silent death into an ordinary write error we can report and survive.
  std::signal(SIGPIPE, SIG_IGN);
#endif
  std::set_terminate(OnTerminate);
}

void SetStage(const char* text) {
  if (text == nullptr) return;
  size_t i = 0;
  for (; i + 1 < kStageBytes && text[i] != '\0'; ++i) g_stage[i] = text[i];
  g_stage[i] = '\0';
}

}  // namespace mvdb
