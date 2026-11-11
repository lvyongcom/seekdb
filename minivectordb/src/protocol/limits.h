// Wire limits.
//
// The judge reads one response line per request and caps it at
// `max_response_bytes` (16 MiB in the published config). Exceeding it is a
// protocol violation, not a failed assertion, so it is worth refusing a result
// we cannot legally deliver instead of emitting a line the judge must reject.
#pragma once

#include <cstddef>

namespace mvdb {

inline constexpr size_t kMaxResponseBytes = 16u * 1024u * 1024u;

// The engine stops materialising rows a little before the hard limit so the
// final serialization still has room for the wrapper fields and escaping.
inline constexpr size_t kMaxResultBytes = kMaxResponseBytes - (64u * 1024u);

}  // namespace mvdb
