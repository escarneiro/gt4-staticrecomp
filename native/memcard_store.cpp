#include "memcard_store.hpp"

#include <cstdio>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#include <stdio.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gt4::memcard {
namespace {

// Forces the bytes of `file` to stable storage before the rename.
bool flush_to_disk(std::FILE *file) {
  if (std::fflush(file) != 0) return false;
#ifdef _WIN32
  return _commit(_fileno(file)) == 0;
#else
  return fsync(fileno(file)) == 0;
#endif
}

// Opens with a wide path on Windows so non-ASCII user names keep working.
std::FILE *open_file(const std::filesystem::path &path, bool writing) {
#ifdef _WIN32
  std::FILE *file = nullptr;
  if (_wfopen_s(&file, path.c_str(), writing ? L"wb" : L"rb") != 0) return nullptr;
  return file;
#else
  return std::fopen(path.c_str(), writing ? "wb" : "rb");
#endif
}

#ifndef _WIN32
// Best effort: a folder that cannot be opened or synced leaves the rename done, only not yet durable.
void sync_folder(const std::filesystem::path &path) {
  const std::filesystem::path folder = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
  const int descriptor = open(folder.c_str(), O_RDONLY | O_DIRECTORY);
  if (descriptor < 0) return;
  fsync(descriptor);
  close(descriptor);
}
#endif

bool write_bytes(std::FILE *file, const std::vector<std::uint8_t> &bytes, std::size_t count) {
  return std::fwrite(bytes.data(), 1, count, file) == count;
}

} // namespace

bool write_card_atomically(const std::filesystem::path &path,
                           const std::vector<std::uint8_t> &bytes,
                           CrashPoint crash) {
  std::filesystem::path temporary = path;
  temporary += ".tmp";

  std::FILE *file = open_file(temporary, true);
  if (!file) return false;

  bool ok = true;
  if (crash == CrashPoint::after_partial_write) {
    write_bytes(file, bytes, bytes.size() / 2);
    std::fclose(file);
    return false;  // simulated power loss: the temporary file is incomplete
  }
  ok = write_bytes(file, bytes, bytes.size()) && flush_to_disk(file);
  ok = (std::fclose(file) == 0) && ok;
  if (!ok) return false;

  if (crash == CrashPoint::before_rename) return false;  // simulated crash before the swap

  std::error_code error;
  std::filesystem::rename(temporary, path, error);  // MoveFileExW(REPLACE_EXISTING) on Windows
  if (error) return false;
#ifndef _WIN32
  // POSIX: the rename is in the folder's own data, which reaches the disk only when the folder is synced; without this a
  // power loss soon after a save can bring back the previous card (the new one stays whole, as the old one does).
  sync_folder(path);
#endif
  return true;
}

bool read_card_file(const std::filesystem::path &path, std::vector<std::uint8_t> &bytes) {
  std::FILE *file = open_file(path, false);
  if (!file) return false;
  bytes.clear();
  std::uint8_t chunk[1u << 16];
  std::size_t got = 0;
  while ((got = std::fread(chunk, 1, sizeof chunk, file)) > 0) bytes.insert(bytes.end(), chunk, chunk + got);
  const bool ok = std::ferror(file) == 0;
  std::fclose(file);
  return ok;
}

} // namespace gt4::memcard
