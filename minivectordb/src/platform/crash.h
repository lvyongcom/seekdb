// Crash reporting.
//
// The process died silently during one intermittent failure: stderr had a single
// "data dir ready" line and nothing else, so there was no way to tell which
// statement or which phase killed it. A silent death in a formal case is a zero,
// and an un-diagnosable one is worse than a diagnosable one.
//
// So: record a short "stage" string continuously, and on a fatal signal or an
// uncaught exception write one line to stderr naming the stage. Everything here
// must stay usable from a signal handler, so it only reads a fixed buffer and
// writes with write()/_write().
#pragma once

namespace mvdb {

void InstallCrashHandlers();

// Copies `text` into a fixed buffer that the fatal-signal handler reports.
// Cheap enough to call once per request.
void SetStage(const char* text);

}  // namespace mvdb
