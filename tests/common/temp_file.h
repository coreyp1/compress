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
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <vector>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

/**
 * The null device, spelled for the shell that system() and popen() run.
 *
 * That shell is cmd.exe on Windows, which refuses a redirection to
 * "/dev/null" with "The system cannot find the path specified" and then does
 * not run the command at all - so an oracle probe written the POSIX way
 * reports the tool missing on a machine that has it.
 */
#ifdef _WIN32
#define GCOMP_TEST_NULL_DEVICE "NUL"
#else
#define GCOMP_TEST_NULL_DEVICE "/dev/null"
#endif

namespace gcomp_test {

namespace detail {

/// Create @p path, failing if anything is already there.  On failure,
/// @p err receives errno.
inline bool createExclusive(const std::string & path, int * err) {
#ifdef _WIN32
  int fd = _open(path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
      _S_IREAD | _S_IWRITE);
#else
  int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
#endif
  if (fd < 0) {
    *err = errno;
    return false;
  }
#ifdef _WIN32
  _close(fd);
#else
  close(fd);
#endif
  return true;
}

/**
 * Create `<unique name><suffix>` and return its name, or empty on failure.
 *
 * cutil makes `<unique name>` unique only among files that exist while it
 * does.  The suffixed name is a different file, so the base name's
 * uniqueness does not carry over to it once the base is gone - and this
 * used to rename the base onto the suffixed name, which is exactly what
 * frees it.  On POSIX the next call's six random characters made a repeat
 * improbable; on Windows `_mktemp` names are a letter and the process id,
 * so the next call chose the same base again and two live TempFiles shared
 * one path.
 *
 * So the suffixed name is itself created exclusively.  If it is taken, the
 * base that produced it is held open - which makes cutil choose another -
 * until one is found, and every held base is released afterwards.  This is
 * correct however cutil chooses its names.
 */
inline std::string createUnique(const char * prefix, const std::string & suffix) {
  if (suffix.empty()) {
    // The suffixed name would be the base itself, which always exists.
    return {};
  }
  // A counter in the prefix, because on Windows cutil can only ever have 26
  // files of one prefix in existence per process (the _mktemp letter), and
  // a sweep holds thousands.  It changes nothing on POSIX but the name.
  static std::atomic<unsigned long> counter{0};
  const std::string stem = std::string(prefix ? prefix : "tmp")
      + std::to_string(counter.fetch_add(1)) + "_";
  std::vector<GCU_File_Temp> held;
  std::string result;
  // Bounded: _mktemp offers 26 names per prefix per process.
  for (int attempt = 0; attempt < 26; ++attempt) {
    GCU_File_Temp temp;
    // dir = NULL means wherever gcu_path_temp_dir() says, which is TMPDIR
    // when it is set on POSIX.  Half the copies this replaces hardcoded
    // "/tmp".
    if (gcu_file_temp_create(&temp, nullptr, stem.c_str(), nullptr)
        != GCU_FILE_OK) {
      break;
    }
    std::string dest = std::string(gcu_file_temp_path(&temp)) + suffix;
    int err = 0;
    if (createExclusive(dest, &err)) {
      result = dest;
      gcu_file_temp_abort(&temp);
      break;
    }
    held.push_back(temp);
    if (err != EEXIST) {
      break;
    }
  }
  for (GCU_File_Temp & temp : held) {
    gcu_file_temp_abort(&temp);
  }
  return result;
}

} // namespace detail

/**
 * A temporary file that deletes itself, and any name derived from it.
 *
 * The suffix is not decoration: the external oracles dispatch on the
 * extension, so `gzip` and `zstd` both misread a file that does not carry
 * one.  cutil chooses a unique name; the file actually used is that name
 * plus the suffix, created exclusively beside it (see
 * detail::createUnique()).
 */
class TempFile {
public:
  TempFile(const char * prefix, const std::string & suffix)
      : path_(detail::createUnique(prefix, suffix)) {}

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
    return gcu_file_write_atomic(path_.c_str(), data, len,
               GCU_FILE_SYNC_NONE, GCU_FILE_PERMS_PRIVATE, nullptr)
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
  return detail::createUnique(prefix, suffix);
}

/// Replace a file's contents.  False on any failure.
inline bool writeWholeFile(
    const std::string & path, const std::vector<uint8_t> & data) {
  return gcu_file_write_atomic(path.c_str(), data.data(), data.size(),
             GCU_FILE_SYNC_NONE, GCU_FILE_PERMS_PRIVATE, nullptr)
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
