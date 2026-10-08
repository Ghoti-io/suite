#!/bin/bash
# Source INSIDE ghoti-cross-mingw64:deb13.  Makes the container look enough like
# MSYS2 MINGW64 (it IMITATES it; this is not Windows, see README.md) that the libraries' real Makefiles (which pick their Windows
# arm from `uname -s`) cross-compile: a PATH shim directory holding a uname
# that answers MINGW64_NT, an identity cygpath, and cc/g++/ar/... pointing at
# the mingw toolchain.  Used by m1-win.sh.  /w is a writable scratch tree.
SHIM=/w/shim
mkdir -p $SHIM
T=x86_64-w64-mingw32
for p in gcc g++ ar ranlib objdump nm strip windres dlltool; do
  ln -sf /usr/bin/$T-$p $SHIM/$p
done
ln -sf /usr/bin/$T-gcc $SHIM/cc
ln -sf /usr/bin/$T-g++ $SHIM/c++
cat > $SHIM/uname <<'U'
#!/bin/sh
case "$1" in -s|"") echo MINGW64_NT-10.0-26100;; -m) echo x86_64;; *) exec /usr/bin/uname "$@";; esac
U
cat > $SHIM/cygpath <<'U'
#!/bin/sh
for a; do :; done; echo "$a"
U
chmod +x $SHIM/uname $SHIM/cygpath
export PATH=$SHIM:$PATH
export WPREFIX=/w/prefix
export PREFIX=$WPREFIX
export PKG_CONFIG_PATH=$WPREFIX/share/pkgconfig

# Host wine (same Debian 13 libc as the image), mounted at /hostusr, and a
# binfmt_misc entry so that a PE .exe a recipe runs (cutil's float_identifier
# generator, the test binaries) is run by wine.  Needs --cap-add SYS_ADMIN and
# a kernel with per-user-namespace binfmt_misc (6.7+).
export WINEPREFIX=/w/wineprefix WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLPATH= XDG_RUNTIME_DIR=/tmp
cat > $SHIM/wine-exec <<'U'
#!/bin/sh
export LD_LIBRARY_PATH=/hostusr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export WINEPREFIX=${WINEPREFIX:-/w/wineprefix} WINEDEBUG=${WINEDEBUG:--all}
export WINESERVER=/usr/lib/wine/wineserver64
exec /usr/lib/wine/wine64 "$@"
U
chmod +x $SHIM/wine-exec; ln -sf wine-exec $SHIM/wine
mount -t binfmt_misc none /proc/sys/fs/binfmt_misc 2>/dev/null
[ -e /proc/sys/fs/binfmt_misc/wine ] || echo ":wine:M::MZ::$SHIM/wine-exec:" > /proc/sys/fs/binfmt_misc/register

# Host python3 (the checks need one; the image has none), run from /hostusr.
cat > $SHIM/python3 <<'U'
#!/bin/sh
export LD_LIBRARY_PATH=/hostusr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export PYTHONHOME=/hostusr
exec /hostusr/bin/python3 "$@"
U
chmod +x $SHIM/python3; ln -sf python3 $SHIM/python

# In MSYS2 gtest lives in /mingw64/{include,lib}, a default search path; here it
# is in the prefix, so say so to the compiler.
export CPATH=$WPREFIX/include LIBRARY_PATH=$WPREFIX/lib
# pkg-config drops -I/-L it believes are system paths (CPATH/LIBRARY_PATH are
# read as such), so name them to the Makefiles directly as well.
export EXTRA_CXXFLAGS="-I$WPREFIX/include" EXTRA_LDFLAGS="-L$WPREFIX/lib"
unset CPATH LIBRARY_PATH

# The runtime DLLs MSYS2 has in /mingw64/bin, next to the libraries' own DLLs in
# the prefix, and the wine-side PATH that finds them.
mkdir -p $WPREFIX/bin
for d in libwinpthread-1.dll libgcc_s_seh-1.dll libstdc++-6.dll; do
  f=$(find /usr/lib/gcc/x86_64-w64-mingw32 /usr/x86_64-w64-mingw32 -name $d 2>/dev/null | head -1)
  [ -n "$f" ] && cp -u "$f" $WPREFIX/bin/
done
export WINEPATH='Z:\w\prefix\bin'

# The image has no bison or flex either.  The host's are run from /hostusr the
# same way (they are platform independent tools, and what they write is C), with
# the host m4 and bison's data directory named explicitly because both are
# compiled in as /usr/... paths that the image does not have.
for g in bison flex; do
  cat > $SHIM/$g <<U
#!/bin/sh
export LD_LIBRARY_PATH=/hostusr/lib/x86_64-linux-gnu\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}
export M4=/hostusr/bin/m4 BISON_PKGDATADIR=/hostusr/share/bison
exec /hostusr/bin/$g "\$@"
U
  chmod +x $SHIM/$g
done
