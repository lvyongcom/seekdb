#include "storage/table_store.h"

#include <cstring>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#include "platform/fs.h"
#include "storage/row_codec.h"

namespace mvdb {
namespace {

constexpr size_t kHeaderBytes = 4;
constexpr size_t kCrcBytes = 4;
constexpr size_t kKeyBytes = 8;
constexpr size_t kReadChunk = 1u << 16;
// Guards against a corrupt length prefix turning into a huge allocation.
constexpr uint32_t kMaxBodyBytes = 64u * 1024u * 1024u;

// A table whose encoded records fit in this budget is kept in memory, which
// removes the per-query disk read without reintroducing the decoded-row blow-up.
// Data sets above it fall back to streaming, so the memory ceiling still holds.
constexpr uint64_t kRowCacheLimit = 64ull * 1024ull * 1024ull;

const uint32_t* CrcTable() {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value >> 1) ^ (0xEDB88320u & (0u - (value & 1u)));
      }
      table[i] = value;
    }
    ready = true;
  }
  return table;
}

uint32_t Crc32(const char* data, size_t size) {
  const uint32_t* table = CrcTable();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc = (crc >> 8) ^
          table[(crc ^ static_cast<unsigned char>(data[i])) & 0xFF];
  }
  return ~crc;
}

void AppendU32(std::string* out, uint32_t value) {
  out->push_back(static_cast<char>(value & 0xFF));
  out->push_back(static_cast<char>((value >> 8) & 0xFF));
  out->push_back(static_cast<char>((value >> 16) & 0xFF));
  out->push_back(static_cast<char>((value >> 24) & 0xFF));
}

uint32_t ReadU32(const char* data) {
  return static_cast<uint32_t>(static_cast<unsigned char>(data[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(data[3])) << 24);
}

int64_t DecodeI64(const char* data) {
  uint64_t bits = 0;
  for (int k = 0; k < 8; ++k) {
    bits |= static_cast<uint64_t>(static_cast<unsigned char>(data[k])) << (8 * k);
  }
  return static_cast<int64_t>(bits);
}

// long is 32-bit on Windows, so 64-bit seek/tell must be used explicitly for
// data sets above 2 GiB.
bool SeekFile(std::FILE* file, uint64_t offset) {
#ifdef _WIN32
  return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
  return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

bool SeekEnd(std::FILE* file) {
#ifdef _WIN32
  return _fseeki64(file, 0, SEEK_END) == 0;
#else
  return fseeko(file, 0, SEEK_END) == 0;
#endif
}

uint64_t TellFile(std::FILE* file) {
#ifdef _WIN32
  const __int64 position = _ftelli64(file);
#else
  const off_t position = ftello(file);
#endif
  return position < 0 ? 0 : static_cast<uint64_t>(position);
}

}  // namespace

TableStore::~TableStore() {
  if (writer_ != nullptr) {
    std::fflush(writer_);
    std::fclose(writer_);
    writer_ = nullptr;
  }
}

bool TableStore::Open(const std::string& data_dir, uint64_t table_id) {
  if (writer_ != nullptr) {
    std::fclose(writer_);
    writer_ = nullptr;
  }
  path_ = JoinPath(data_dir, std::to_string(table_id) + ".log");
  append_offset_ = 0;
  last_valid_offset_ = 0;
  live_.clear();
  cache_.clear();
  cache_active_ = false;
  return true;
}

bool TableStore::Append(LogOp op, int64_t key, const std::string& row_image,
                        uint64_t* offset) {
  if (writer_ == nullptr) {
    // Deliberately not "ab": in append mode the initial file position is not
    // guaranteed to be the end, so the running offset would start at 0 and
    // every liveness check would then compare against the wrong value.
    writer_ = std::fopen(path_.c_str(), "r+b");
    if (writer_ == nullptr) writer_ = std::fopen(path_.c_str(), "w+b");
    if (writer_ == nullptr) return false;
    if (!SeekEnd(writer_)) {
      std::fclose(writer_);
      writer_ = nullptr;
      return false;
    }
    append_offset_ = TellFile(writer_);

    // A table created in this session never goes through Recover(), so this is
    // the only place that can turn the row cache on. Without it every query on
    // such a table re-verifies the checksum of every record.
    if (!cache_active_ && append_offset_ <= kRowCacheLimit) {
      cache_.clear();
      if (append_offset_ == 0 || ReadFileToString(path_, &cache_)) {
        cache_active_ = true;
      }
    }
  }

  // One reused buffer for the whole run: two fresh strings per row dominated the
  // write path at 15k rows/s.
  std::string& frame = frame_scratch_;
  frame.clear();
  frame.reserve(kHeaderBytes + 1 + kKeyBytes + row_image.size() + kCrcBytes);

  const size_t body_start = kHeaderBytes;
  frame.append(kHeaderBytes, '\0');  // length filled in below
  frame.push_back(static_cast<char>(op));
  frame.append(EncodeKey(key));
  frame.append(row_image);

  const size_t body_length = frame.size() - body_start;
  frame[0] = static_cast<char>(body_length & 0xFF);
  frame[1] = static_cast<char>((body_length >> 8) & 0xFF);
  frame[2] = static_cast<char>((body_length >> 16) & 0xFF);
  frame[3] = static_cast<char>((body_length >> 24) & 0xFF);
  const uint32_t crc = Crc32(frame.data() + body_start, body_length);
  AppendU32(&frame, crc);

  if (std::fwrite(frame.data(), 1, frame.size(), writer_) != frame.size()) return false;

  if (offset != nullptr) *offset = append_offset_;
  if (cache_active_) {
    cache_.append(frame);
    if (cache_.size() > kRowCacheLimit) {
      // Keep the memory ceiling: a table that outgrows the budget goes back to
      // streaming and its scan-side checksum verification.
      cache_.clear();
      cache_.shrink_to_fit();
      cache_active_ = false;
    }
  }
  append_offset_ += frame.size();

  if (op == LogOp::Delete) {
    if (live_.erase(key) > 0) ++stale_records_;
  } else {
    const auto found = live_.find(key);
    if (found != live_.end()) ++stale_records_;  // the older version is masked
    live_[key] = append_offset_ - frame.size();
  }
  return true;
}

bool TableStore::Flush() {
  if (writer_ == nullptr) return true;
  return std::fflush(writer_) == 0;
}

bool TableStore::Truncate(uint64_t offset) {
  if (writer_ != nullptr) {
    std::fflush(writer_);
    std::fclose(writer_);
    writer_ = nullptr;
  }

  bool ok = true;
  std::FILE* file = std::fopen(path_.c_str(), "r+b");
  if (file != nullptr) {
#ifdef _WIN32
    ok = _chsize_s(_fileno(file), static_cast<__int64>(offset)) == 0;
#else
    ok = ftruncate(fileno(file), static_cast<off_t>(offset)) == 0;
#endif
    std::fclose(file);
  }

  if (ok) {
    append_offset_ = offset;
    if (cache_active_ && cache_.size() > offset) cache_.resize(offset);
  }
  return ok;
}

uint64_t TableStore::FileSize() const {
  std::FILE* file = std::fopen(path_.c_str(), "rb");
  if (file == nullptr) return 0;
  uint64_t size = 0;
  if (SeekEnd(file)) size = TellFile(file);
  std::fclose(file);
  return size;
}

namespace {

// Emits every complete, checksummed record in one contiguous buffer.
// `base_offset` is the file offset of buffer[0], so record offsets stay absolute.
//
// `verify` is false for scans: checksums guard against a torn tail on recovery,
// and re-verifying every record on every query dominated scan time (a 55 MiB
// table cost about a second per query before this flag existed).
bool ParseRecords(const char* base, size_t available, uint64_t base_offset, bool verify,
                  const std::function<bool(LogOp, int64_t, uint64_t, const char*, size_t)>& visit,
                  size_t* consumed, uint64_t* last_valid) {
  *consumed = 0;
  while (true) {
    const size_t position = *consumed;
    if (available - position < kHeaderBytes) return true;

    const uint32_t body_length = ReadU32(base + position);
    if (body_length < 1 + kKeyBytes || body_length > kMaxBodyBytes) return true;
    if (available - position < kHeaderBytes + body_length + kCrcBytes) return true;

    const char* body = base + position + kHeaderBytes;
    if (verify && ReadU32(body + body_length) != Crc32(body, body_length)) return true;

    const auto op = static_cast<LogOp>(static_cast<unsigned char>(body[0]));
    const int64_t key = DecodeI64(body + 1);
    const char* image = body + 1 + kKeyBytes;
    const size_t image_size = body_length - 1 - kKeyBytes;

    if (!visit(op, key, base_offset + position, image, image_size)) {
      *consumed = position + kHeaderBytes + body_length + kCrcBytes;
      *last_valid = base_offset + *consumed;
      return true;
    }
    *consumed = position + kHeaderBytes + body_length + kCrcBytes;
    *last_valid = base_offset + *consumed;
  }
}

}  // namespace

bool TableStore::ScanBuffer(const char* base, size_t size, const Visit& visit) {
  size_t consumed = 0;
  uint64_t last_valid = 0;
  ParseRecords(base, size, 0, /*verify=*/false, visit, &consumed, &last_valid);
  last_valid_offset_ = last_valid;
  return true;
}

bool TableStore::ForEachRecord(const Visit& visit) {
  last_valid_offset_ = 0;

  if (cache_active_) {
    size_t consumed = 0;
    uint64_t last_valid = 0;
    ParseRecords(cache_.data(), cache_.size(), 0, /*verify=*/false, visit, &consumed,
                 &last_valid);
    last_valid_offset_ = last_valid;
    return true;
  }

  std::FILE* file = std::fopen(path_.c_str(), "rb");
  if (file == nullptr) return true;  // a table that never mutated has no log

  std::string buffer;
  buffer.reserve(kReadChunk * 2);
  size_t consumed = 0;
  uint64_t file_position = 0;  // offset of buffer[0] in the file

  std::vector<char> chunk(kReadChunk);
  bool keep_going = true;

  while (keep_going) {
    const size_t got = std::fread(chunk.data(), 1, chunk.size(), file);
    if (got > 0) buffer.append(chunk.data(), got);

    size_t used = 0;
    uint64_t last_valid = 0;
    ParseRecords(buffer.data() + consumed, buffer.size() - consumed,
                 file_position + consumed, /*verify=*/true, visit, &used, &last_valid);
    consumed += used;
    if (last_valid != 0) last_valid_offset_ = last_valid;

    if (got == 0) break;

    // Slide the window so the buffer cannot grow with the file.
    if (consumed > 0) {
      buffer.erase(0, consumed);
      file_position += consumed;
      consumed = 0;
    }
  }

  std::fclose(file);
  return true;
}

bool TableStore::Recover() {
  live_.clear();
  stale_records_ = 0;

  const uint64_t size = FileSize();
  cache_.clear();
  cache_active_ = false;
  if (size > 0 && size <= kRowCacheLimit) {
    if (ReadFileToString(path_, &cache_)) cache_active_ = true;
  }

  if (!ForEachRecord([this](LogOp op, int64_t key, uint64_t offset, const char* /*data*/,
                            size_t /*size*/) {
        if (op == LogOp::Delete) {
          if (live_.erase(key) > 0) ++stale_records_;
        } else {
          if (live_.find(key) != live_.end()) ++stale_records_;
          live_[key] = offset;
        }
        return true;
      })) {
    return false;
  }

  // Drop a torn tail (a record half written by a kill) and line the writer up
  // with the end of the intact data, so a later transaction marker records the
  // right offset.
  if (size > last_valid_offset_) {
    Truncate(last_valid_offset_);
  } else {
    append_offset_ = last_valid_offset_;
  }
  return true;
}

bool TableStore::Scan(const RowCallback& callback) {
  // Append-only tables (the common case, and the whole write benchmark) have no
  // stale versions, so the per-record map lookup can be skipped outright.
  const bool all_live = (stale_records_ == 0);
  return ForEachRecord([this, &callback, all_live](LogOp op, int64_t key, uint64_t offset,
                                                   const char* data, size_t size) {
    if (op == LogOp::Delete) return true;
    if (!all_live) {
      const auto found = live_.find(key);
      // A stale version (superseded or deleted) is not visible.
      if (found == live_.end() || found->second != offset) return true;
    }
    return callback(key, offset, data, size);
  });
}

bool TableStore::Read(int64_t key, std::string* row_image) {
  const auto found = live_.find(key);
  if (found == live_.end()) return false;

  std::FILE* file = std::fopen(path_.c_str(), "rb");
  if (file == nullptr) return false;

  bool ok = false;
  do {
    if (!SeekFile(file, found->second)) break;

    char header[kHeaderBytes];
    if (std::fread(header, 1, kHeaderBytes, file) != kHeaderBytes) break;
    const uint32_t body_length = ReadU32(header);
    if (body_length < 1 + kKeyBytes || body_length > kMaxBodyBytes) break;

    std::string body(body_length, '\0');
    if (std::fread(&body[0], 1, body_length, file) != body_length) break;

    char trailer[kCrcBytes];
    if (std::fread(trailer, 1, kCrcBytes, file) != kCrcBytes) break;
    if (ReadU32(trailer) != Crc32(body.data(), body.size())) break;

    const auto op = static_cast<LogOp>(static_cast<unsigned char>(body[0]));
    if (op == LogOp::Delete) break;
    if (DecodeI64(body.data() + 1) != key) break;

    row_image->assign(body, 1 + kKeyBytes, body_length - 1 - kKeyBytes);
    ok = true;
  } while (false);

  std::fclose(file);
  return ok;
}

}  // namespace mvdb
