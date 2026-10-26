#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
VERSION=$(head -1 VERSION | tr -d ' \r\n')
MODE=${1:-deb}
OUT="$ROOT/dist"
mkdir -p "$OUT"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
[ -x ./csyc ] || { echo "error: build csyc first (make csyc)" >&2; exit 1; }
./csyc --version | grep -q "csyc $VERSION" || { echo "error: ./csyc is not version $VERSION; run: touch src/main.cpp && make csyc" >&2; exit 1; }

MAINTAINER=${MAINTAINER:-CSy <https://github.com/TonSHd/CSy>}

case "$MODE" in
deb)
    ARCH=$(dpkg --print-architecture)
    make -s install DESTDIR="$STAGE" PREFIX=/usr
    install -m 0644 packaging/copyright "$STAGE/usr/share/doc/csyc/copyright"
    printf 'csyc (%s) unstable; urgency=low\n\n  * Package built from the CSy sources.\n\n -- %s  %s\n' \
        "$VERSION" "$MAINTAINER" "$(date -R)" | gzip -9n > "$STAGE/usr/share/doc/csyc/changelog.Debian.gz"
    chmod 0644 "$STAGE/usr/share/doc/csyc/changelog.Debian.gz"
    mkdir -p "$STAGE/DEBIAN"
    (cd "$STAGE" && find usr -type f -exec md5sum {} + | sort -k2 > DEBIAN/md5sums)
    SIZE=$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)
    cat > "$STAGE/DEBIAN/control" <<CONTROL
Package: csyc
Version: $VERSION
Section: devel
Priority: optional
Architecture: $ARCH
Depends: libc6 (>= 2.38)
Recommends: gcc | c-compiler
Installed-Size: $SIZE
Maintainer: $MAINTAINER
Description: the CSy compiler and runner
 CSy is an AI focused language with a bytecode VM and an optional native
 fast path. csyc compiles and runs CSy programs and projects, creates new
 projects (csyc init) and can write standalone executables.
 The native fast path works without a C compiler on x86-64; a C compiler
 enables the second native backend.
CONTROL
    chmod 0755 "$STAGE" "$STAGE/DEBIAN"
    dpkg-deb --root-owner-group -Zxz -b "$STAGE" "$OUT/csyc_${VERSION}_${ARCH}.deb" >/dev/null
    echo "$OUT/csyc_${VERSION}_${ARCH}.deb"
    ;;
tar)
    ARCH=$(uname -m)
    NAME="csyc-$VERSION-linux-$ARCH"
    make -s install DESTDIR="$STAGE/$NAME" PREFIX=
    cp packaging/copyright "$STAGE/$NAME/share/doc/csyc/copyright"
    (cd "$STAGE/$NAME" && find . -type f | sed 's|^\./||' | sort > share/doc/csyc/MANIFEST)
    cat > "$STAGE/$NAME/install.sh" <<'INSTALL'
#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX=${1:-/usr}
mkdir -p "$PREFIX"
cp -a "$HERE/bin" "$HERE/share" "$PREFIX/"
echo "csyc installed in $PREFIX/bin/csyc"
INSTALL
    cat > "$STAGE/$NAME/uninstall.sh" <<'UNINSTALL'
#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX=${1:-/usr}
while IFS= read -r f; do rm -f "$PREFIX/$f"; done < "$HERE/share/doc/csyc/MANIFEST"
rm -f "$PREFIX/share/doc/csyc/MANIFEST"
rmdir "$PREFIX/share/doc/csyc/ccm" "$PREFIX/share/doc/csyc" "$PREFIX/share/csyc" 2>/dev/null || true
echo "csyc removed from $PREFIX"
UNINSTALL
    chmod 0755 "$STAGE/$NAME/install.sh" "$STAGE/$NAME/uninstall.sh"
    tar -C "$STAGE" --owner=0 --group=0 -czf "$OUT/$NAME.tar.gz" "$NAME"
    echo "$OUT/$NAME.tar.gz"
    ;;
*) echo "usage: package.sh deb|tar" >&2; exit 1 ;;
esac