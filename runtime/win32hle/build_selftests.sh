#!/bin/sh
# Build and run every win32hle selftest, each linked with the whole layer:
# under gcc -m32 (the 32-bit flat host; needs gcc-multilib and SDL2 +
# SDL2_ttf for i386, README), or with HLE_HOST64=1 (the default on macOS) as
# the host's own 64-bit program, the guest in a 4 GB reservation (guest_mem.c).
set -u
cd "$(dirname "$0")"
CORE=$(ls *.c | grep -v _selftest.c)
if [ "${HLE_HOST64:-$([ "$(uname)" = Darwin ] && echo 1 || echo 0)}" = 1 ]; then
  CC=${CC:-cc}; ARCH=""
  LIBS=$(pkg-config --cflags --libs sdl2 SDL2_ttf)
else
  CC=${CC:-gcc}; ARCH="-m32"
  LIBS=$(PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --cflags --libs sdl2 SDL2_ttf)
fi
TMP=${TMPDIR:-/tmp}
fail=0
for t in *_selftest.c; do
  exe=$TMP/${t%.c}
  # shellcheck disable=SC2086  # CORE, LIBS and ARCH are lists
  if $CC $ARCH -O1 -Wall -Wno-unused-function -Wno-unused-but-set-variable -I../recomp32 -I. $CORE "$t" $LIBS -lpthread -lm -o "$exe"; then
    if SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$exe" > "$exe.out" 2>&1; then echo "ok    $t"; else echo "FAIL  $t"; tail -5 "$exe.out"; fail=1; fi
  else
    echo "BUILD FAIL  $t"; fail=1
  fi
done
exit $fail
