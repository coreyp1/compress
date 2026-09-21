/**
 * @file temp_file.h
 *
 * One temporary file for the whole test suite, built on cutil.
 *
 * Twelve test files each carried a private copy of this.  Nine of them had a
 * `#ifdef _WIN32` half that has never been compiled - `COMPRESS-TODO`
 * section 5: Windows is untested.  The POSIX halves came in three shapes, and
 * all three were worse than the one call that replaces them:
 *
 *   - `mkstemps()` into a hardcoded `"/tmp"`, ignoring `TMPDIR`.
 *   - `tempPath()`, which built a name out of the process id and a counter
 *     and handed it back *unopened*, so the file was created by whoever
 *     opened it next.  That is the "invent a name and then open it" pattern
 *     `cutil/file.h` warns about: between the naming and the opening,
 *     anything may put a symbolic link there.
 *   - `temp_path()`, whose unique part was `"%d_%p"` of a stack address -
 *     the same value on every call from the same frame.
 *
 * `gcu_file_temp_create()` chooses the name and creates the file in one step
 * that fails if the name is taken, opens it so that only its owner may read
 * it, and puts it wherever `gcu_path_temp_dir()` says.  One implementation
 * that both platforms test, rather than nine that neither does.
 *
 * Header-only, like `failing_allocator.h` beside it, so that the three build
 * trees (release, ASan, TSan) need no new object rule.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GCOMP_TESTS_TEMP_FILE_H
#define GCOMP_TESTS_TEMP_FILE_H

#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/path.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gcomp_test {

/**
 * A temporary file that deletes itself, and any name derived from it.
 *
 * The suffix is not decoration: the external oracles dispatch on the
 * extension, so `gzip` and `zstd` both misread a file that does not carry
 * one.  cutil appends six random characters to the prefix; the suffix is
 * applied by committing the created file to `<unique name><suffix>` in the
 * same directory, which inherits the uniqueness and keeps the rename inside
 * one filesystem, where it is atomic.
 */
class TempFile {
public:
  TempFile(const char * prefix, const std::string & suffix) {
    GCU_File_Temp temp;
    // dir = NULL means wherever gcu_path_temp_dir() says, which is TMPDIR
    // when it is set.  Half the copies this replaces hardcoded "/tmp".
    if (gcu_file_temp_create(&temp, nullptr, prefix, nullptr) != GCU_FILE_OK) {
      return;
    }
    std::string dest = std::string(gcu_file_temp_path(&temp)) + suffix;
    // Commit spends the handle whether it succeeds or fails, so there is no
    // abort path here.  SYNC_NONE because this is scratch that every run
    // regenerates; file.h reserves the durable default for bytes that cannot
    // be remade.
    if (gcu_file_temp_commit(&temp, dest.c_str(), GCU_FILE_SYNC_NONE)
        != GCU_FILE_OK) {
      return;
    }
    path_ = dest;
  }

  ~TempFile() {
    for (const auto & name : also_remove_) {
      std::remove(name.c_str());
    }
    if (!path_.empty()) {
      std::remove(path_.c_str());
    }
  }

  TempFile(const TempFile &) = delete;
  TempFile & operator=(const TempFile &) = delete;

  /// True if the file exists on disk and @ref path is usable.
  bool valid() const noexcept { return !path_.empty(); }

  /// The name on disk.  Empty if construction failed.
  const std::string & path() const noexcept { return path_; }

  /// Replace the contents.  False on any failure.
  bool write(const void * data, size_t len) {
    if (path_.empty()) {
      return false;
    }
    return gcu_file_write_atomic(
               path_.c_str(), data, len, GCU_FILE_SYNC_NONE, nullptr)
        == GCU_FILE_OK;
  }

  bool write(const std::vector<uint8_t> & data) {
    return write(data.data(), data.size());
  }

  /**
   * A second name beside this one - `derived(".out")` for a file the oracle
   * is about to write, say.  Registered for removal, which the open-coded
   * `path + ".out"` never was.
   */
  std::string derived(const std::string & extra) {
    if (path_.empty()) {
      return {};
    }
    std::string name = path_ + extra;
    also_remove_.push_back(name);
    return name;
  }

private:
  std::string path_;
  std::vector<std::string> also_remove_;
};

/// Read a whole file.  Empty on any failure, and on an empty file.
inline std::vector<uint8_t> readWholeFile(const std::string & path) {
  void * data = nullptr;
  size_t len = 0;
  // Chunked rather than sized first, so this reads what fseek/ftell quietly
  // return nothing for: a pipe, a character device, anything under /proc.
  if (gcu_file_read(path.c_str(), GCU_FILE_UNLIMITED, nullptr, &data, &len)
      != GCU_FILE_OK) {
    return {};
  }
  const uint8_t * bytes = static_cast<const uint8_t *>(data);
  std::vector<uint8_t> out(bytes, bytes + len);
  gcu_file_free(nullptr, data);
  return out;
}

/**
 * Read a whole file, distinguishing a missing file from an empty one.
 *
 * The single-argument form flattens both to an empty vector, which is what
 * most callers want; the suites that report whether the oracle produced a
 * file at all need the difference.
 *
 * @return False if the file could not be read.  @p out is left untouched
 *   then.
 */
inline bool readWholeFile(const std::string & path, std::vector<uint8_t> * out) {
  void * data = nullptr;
  size_t len = 0;
  if (gcu_file_read(path.c_str(), GCU_FILE_UNLIMITED, nullptr, &data, &len)
      != GCU_FILE_OK) {
    return false;
  }
  const uint8_t * bytes = static_cast<const uint8_t *>(data);
  out->assign(bytes, bytes + len);
  gcu_file_free(nullptr, data);
  return true;
}

/**
 * A unique name, with the file already created and left in place.
 *
 * For the suites that hand a name to an external tool and unlink it
 * themselves, where the lifetime is not a scope and @ref TempFile does not
 * fit.  The file is created rather than merely named, which is the whole
 * point: the `tempPath()` copies this replaces built a name out of the
 * process id and a counter and returned it unopened.
 *
 * Empty on failure.  The caller removes it.
 */
inline std::string uniqueTempPath(
    const char * prefix, const std::string & suffix) {
  GCU_File_Temp temp;
  if (gcu_file_temp_create(&temp, nullptr, prefix, nullptr) != GCU_FILE_OK) {
    return {};
  }
  std::string dest = std::string(gcu_file_temp_path(&temp)) + suffix;
  if (gcu_file_temp_commit(&temp, dest.c_str(), GCU_FILE_SYNC_NONE)
      != GCU_FILE_OK) {
    return {};
  }
  return dest;
}

/// Replace a file's contents.  False on any failure.
inline bool writeWholeFile(
    const std::string & path, const std::vector<uint8_t> & data) {
  return gcu_file_write_atomic(path.c_str(), data.data(), data.size(),
             GCU_FILE_SYNC_NONE, nullptr)
      == GCU_FILE_OK;
}

/**
 * Rewrite a path's separators to `/`.
 *
 * The oracles are driven by Python one-liners that embed a path in a string
 * literal, and a Windows path put there verbatim arrives full of escape
 * sequences.  Each test file used to do this with its own `#ifdef _WIN32`
 * loop over the characters; cutil copies unchanged on POSIX, so the branch
 * disappears rather than moving.
 */
inline std::string toPosixPath(const std::string & path) {
  size_t len = 0;
  if (gcu_path_to_posix(GCU_PATH_NATIVE, path.c_str(), nullptr, 0, &len)
      != GCU_PATH_OK) {
    return path;
  }
  std::string out(len, '\0');
  // out.size() + 1 counts the terminator std::string keeps past the end.
  if (gcu_path_to_posix(GCU_PATH_NATIVE, path.c_str(), out.data(),
          out.size() + 1, &len)
      != GCU_PATH_OK) {
    return path;
  }
  out.resize(len);
  return out;
}

} // namespace gcomp_test

#endif // GCOMP_TESTS_TEMP_FILE_H
