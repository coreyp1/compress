#!/bin/sh
#
# Prove that what `make install` leaves behind is usable by a consumer that
# knows nothing about this source tree.
#
# Everything else this project checks is checked in-tree, against headers in
# include/ and a library in build/, reached by paths the Makefile already
# knows. A consumer reaches none of those: it asks pkg-config for a module
# name, compiles against whatever -I it is handed, and links against whatever
# -L and -l it is handed. That is a different code path, and CONVENTIONS.md
# section 1 records three defects that lived in it while every in-tree build
# stayed green - a .pc that emitted "-I <path>", which pkg-config splits into
# two arguments; one that omitted a "/" and named a directory that does not
# exist; and six of seven that emitted an empty Version:, because VERSION was
# substituted into the template but defined in only one Makefile.
#
# So each check below is named for the defect it would have caught, and the
# last one is the only check that proves the whole path at once: compile a
# program that includes only the installed umbrella header, link it with only
# the flags pkg-config gives, run it, and round-trip a buffer through every
# method the library advertises.
#
# Usage:
#   tools/check-install.sh <prefix> [module-name]
#
# The module name defaults to ghoti.io-compress-0, which is what an ordinary
# build of 0.x installs; pass it explicitly for a build that overrode BRANCH.

set -eu

PREFIX="${1:?usage: check-install.sh <prefix> [module-name]}"
MODULE="${2:-ghoti.io-compress-0}"

# Where `make install PREFIX=...` writes the .pc file. Prepended rather than
# replacing the caller's, so a prefix holding only this library still resolves
# the Requires: on cutil from wherever that was installed.
PKG_CONFIG_PATH="$PREFIX/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_PATH

CC="${CC:-cc}"
CXX="${CXX:-g++}"

fail() {
  printf '\033[0;31mcheck-install: %s\033[0m\n' "$*" >&2
  exit 1
}

pass() {
  printf '  \033[0;32mok\033[0m  %s\n' "$*"
}

printf '\033[0;36m\n'
printf '###################################################\n'
printf '### Consuming the installed library, as a caller ###\n'
printf '###################################################\n'
printf '\033[0m\n'
printf 'module:  %s\n' "$MODULE"
printf 'prefix:  %s\n\n' "$PREFIX"

# ---------------------------------------------------------------------------
# 1. The module is findable at all.
# ---------------------------------------------------------------------------
if ! pkg-config --exists "$MODULE"; then
  fail "pkg-config cannot find $MODULE.
  Searched: $PKG_CONFIG_PATH
  An install writes \$PREFIX/share/pkgconfig/<module>.pc; if that file is
  there, pkg-config is failing on it - run: pkg-config --print-errors --exists $MODULE"
fi
pass "pkg-config finds $MODULE"

# ---------------------------------------------------------------------------
# 2. Version: is not empty.
#
# An empty Version: makes every version constraint a consumer writes fail -
# `Requires: ghoti.io-compress-0 >= 0.0.0` stops resolving - and pkg-config
# reports it as a missing module rather than as a malformed one.
# ---------------------------------------------------------------------------
version="$(pkg-config --modversion "$MODULE" 2>/dev/null || true)"
if [ -z "$version" ]; then
  fail "$MODULE declares an empty Version:.
  The .pc template's (VERSION) placeholder was not substituted at install."
fi
pass "Version: $version"

# ---------------------------------------------------------------------------
# 3. No placeholder survived substitution.
#
# The template carries (SUITE), (PROJECT), (BRANCH), (VERSION), (LIB),
# (INCLUDE) and (REQUIRES). A missed one is not a build failure anywhere: it
# becomes a literal directory name that simply does not exist.
# ---------------------------------------------------------------------------
flags="$(pkg-config --cflags --libs "$MODULE")"
case "$flags" in
  *'('*)
    fail "the installed .pc still contains an unsubstituted placeholder:
  $flags"
    ;;
esac
pass "no unsubstituted placeholders"

# ---------------------------------------------------------------------------
# 4. No flag was split from its argument.
#
# `-I <path>` in a .pc is two arguments to every consumer, and the compiler
# then reads the path as a source file. A bare -I or -L token is the signature.
# ---------------------------------------------------------------------------
for tok in $flags; do
  case "$tok" in
    -I|-L|-l)
      fail "the installed .pc emits a bare '$tok' with its argument separated:
  $flags"
      ;;
  esac
done
pass "every flag carries its argument"

# ---------------------------------------------------------------------------
# 5. Every directory named actually exists.
#
# This is the defect that a single missing "/" produces, and the one an
# in-tree build can never see.
# ---------------------------------------------------------------------------
for tok in $flags; do
  case "$tok" in
    -I*)
      dir="${tok#-I}"
      [ -d "$dir" ] || fail "include directory does not exist: $dir"
      ;;
    -L*)
      dir="${tok#-L}"
      [ -d "$dir" ] || fail "library directory does not exist: $dir"
      ;;
  esac
done
pass "every -I and -L directory exists"

# ---------------------------------------------------------------------------
# 6. A consumer compiles, links, runs, and gets its bytes back.
#
# Every method the README advertises is round-tripped, because a method whose
# registration did not make it into the shared library is invisible until
# somebody asks for it by name. The in-tree tests cannot see this: they link
# the static archive with --whole-archive.
# ---------------------------------------------------------------------------
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

cat > "$work/consumer.c" <<'EOF'
// A consumer of the installed library: one header, and nothing that knows
// where the source tree is.
#include <ghoti.io/compress/compress.h>

#include <stdio.h>
#include <string.h>

int main(void) {
  static const char * const methods[] = {
      "deflate", "gzip", "zlib", "lz4", "lzw", "rle", "zstd"};

  // Long enough to exercise a match finder, and compressible enough that a
  // failure to compress at all would be visible; the round trip is what is
  // being checked, not the ratio.
  char input[4096];
  for (size_t i = 0; i < sizeof(input); i++) {
    input[i] = (char)('a' + (i % 23) + ((i / 97) % 3));
  }

  gcomp_registry_t * registry = gcomp_registry_default();
  if (!registry) {
    fprintf(stderr, "consumer: no default registry\n");
    return 1;
  }

  for (size_t m = 0; m < sizeof(methods) / sizeof(methods[0]); m++) {
    unsigned char encoded[16384];
    char decoded[8192];
    size_t encoded_size = 0;
    size_t decoded_size = 0;

    gcomp_status_t status = gcomp_encode_buffer(registry, methods[m], NULL,
        input, sizeof(input), encoded, sizeof(encoded), &encoded_size);
    if (status != GCOMP_OK) {
      fprintf(stderr, "consumer: %s encode failed (%d)\n", methods[m],
          (int)status);
      return 1;
    }

    status = gcomp_decode_buffer(registry, methods[m], NULL, encoded,
        encoded_size, decoded, sizeof(decoded), &decoded_size);
    if (status != GCOMP_OK) {
      fprintf(stderr, "consumer: %s decode failed (%d)\n", methods[m],
          (int)status);
      return 1;
    }

    if (decoded_size != sizeof(input)
        || memcmp(decoded, input, sizeof(input)) != 0) {
      fprintf(stderr, "consumer: %s did not round-trip\n", methods[m]);
      return 1;
    }

    printf("  %-8s %5zu -> %5zu -> %5zu bytes\n", methods[m], sizeof(input),
        encoded_size, decoded_size);
  }

  printf("%s\n", gcomp_version_string());
  return 0;
}
EOF

# The prefix is not on the default loader path, so the consumer is given an
# rpath to it - the same thing the Makefile does for everything it builds, and
# what a consumer of a non-system prefix has to do. A system install has its
# directory in ld.so.conf instead and needs neither.
rpath=""
for tok in $flags; do
  case "$tok" in
    -L*) rpath="$rpath -Wl,-rpath,${tok#-L}" ;;
  esac
done

# shellcheck disable=SC2086
$CC -std=c17 -Wall -Wextra -Werror -o "$work/consumer" "$work/consumer.c" \
    $flags $rpath || fail "the consumer did not compile or link against the installed library"
pass "a C consumer compiles and links"

"$work/consumer" || fail "the consumer did not run"
pass "every advertised method round-trips through the installed library"

# The public headers are compiled as C++ in-tree because that is how the tests
# are built; this proves the *installed* copies are C++-consumable too, which
# is a different set of files.
# shellcheck disable=SC2086
# -x c++ before the file, not after: it applies to the inputs that follow it,
# and the suffix is .c.
$CXX -std=c++20 -Wall -Wextra -Werror -o "$work/consumer++" \
    -x c++ "$work/consumer.c" $flags $rpath \
    || fail "the installed headers are not C++-consumable"
pass "a C++ consumer compiles and links"

# ---------------------------------------------------------------------------
# 7. The library and its .pc agree about what version this is.
#
# They come from the same Makefile variables, so a disagreement means one of
# the two was substituted from a stale value.
# ---------------------------------------------------------------------------
reported="$("$work/consumer" | tail -n 1)"
case "$reported" in
  "$version"*) ;;
  *)
    fail "the library reports version '$reported' but its .pc says '$version'"
    ;;
esac
pass "the library agrees with its .pc about the version"

printf '\n\033[0;32mThe installed library is consumable.\033[0m\n\n'
