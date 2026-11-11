// MiniVectorDB entry point.
//
// Speaks the judge's JSON Lines protocol on stdin/stdout (see STDIO_PROTOCOL.md):
//   1. open the data directory and finish recovery
//   2. emit {"type":"ready"} -- exactly that object, before reading any request
//   3. serve one response per request until {"op":"shutdown"} arrives
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "db/database.h"
#include "platform/crash.h"
#include "protocol/dispatch.h"
#include "protocol/lineio.h"

namespace {

void PrintUsage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [--data-dir <path>]\n"
               "data dir precedence: --data-dir, $VECTORDB_DATA_DIR, /data\n",
               argv0);
}

std::string ResolveDataDir(int argc, char** argv) {
  std::string data_dir;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--data-dir" && i + 1 < argc) {
      data_dir = argv[++i];
    } else if (arg.rfind("--data-dir=", 0) == 0) {
      data_dir = arg.substr(std::string("--data-dir=").size());
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    } else {
      std::fprintf(stderr, "[mvdb] ignoring unknown argument: %s\n", arg.c_str());
    }
  }
  if (data_dir.empty()) {
    const char* from_env = std::getenv("VECTORDB_DATA_DIR");
    if (from_env != nullptr && *from_env != '\0') data_dir = from_env;
  }
  if (data_dir.empty()) data_dir = "/data";
  return data_dir;
}

}  // namespace

int main(int argc, char** argv) {
  mvdb::InstallCrashHandlers();
  mvdb::InitStdio();
  const std::string data_dir = ResolveDataDir(argc, argv);

  mvdb::SetStage("open database");
  mvdb::Database db;
  const mvdb::Status status = db.Open(data_dir);
  if (!status.ok()) {
    std::fprintf(stderr, "[mvdb] failed to open database: %s (%s)\n",
                 status.message().c_str(), status.code().c_str());
    return 1;
  }

  // Recovery (and index rebuild) is complete; announce readiness exactly once.
  mvdb::WriteLine("{\"type\":\"ready\"}");
  mvdb::SetStage("idle");

  mvdb::Dispatcher dispatcher(&db);
  std::string line;
  while (mvdb::ReadLine(&line)) {
    if (line.empty()) continue;
    // Record what we are about to run: if the process dies silently, the fatal
    // handler prints this string and the failing statement is identified.
    char stage[176];
    std::snprintf(stage, sizeof(stage), "execute: %.140s", line.c_str());
    mvdb::SetStage(stage);

    // A request must never take the process down. If anything throws (for
    // example a bad_alloc while materialising a large result) the dispatcher
    // has already tried to answer; here we only make sure later requests in the
    // same session still get served.
    try {
      if (dispatcher.HandleLine(line)) return 0;
    } catch (const std::exception& error) {
      std::fprintf(stderr, "[mvdb] request aborted: %s\n", error.what());
    } catch (...) {
      std::fprintf(stderr, "[mvdb] request aborted: unknown exception\n");
    }
  }

  // stdin closed without a shutdown request: roll back and leave cleanly.
  db.Shutdown();
  return 0;
}
