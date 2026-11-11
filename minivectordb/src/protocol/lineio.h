// Line-oriented transport on stdin/stdout.
//
// Two Windows hazards are handled here and nowhere else:
//   1. stdout defaults to text mode, so "\n" becomes "\r\n". The judge splits
//      on a bare LF and compares bytes, so everything must be binary mode.
//   2. The judge reads one response per request, so every write must be flushed
//      immediately instead of waiting for the buffer to fill.
#pragma once

#include <cstddef>
#include <string>

namespace mvdb {

// Switches stdin/stdout to binary mode and disables buffering surprises.
// Must be called once, before any read or write.
void InitStdio();

// Reads one line without its terminating LF. Returns false at EOF.
// A trailing CR is stripped so a CRLF producer cannot break JSON parsing.
bool ReadLine(std::string* out);

// Writes one line followed by a single LF and flushes stdout.
void WriteLine(const std::string& line);

}  // namespace mvdb
