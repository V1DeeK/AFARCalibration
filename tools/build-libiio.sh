#!/usr/bin/env bash
# Собирает libiio v0.25 (сетевой бэкенд) из исходников в локальный префикс.
# Нужен на macOS: в Homebrew формулы libiio нет. На Linux проще
# `apt install libiio-dev`, на Windows — пакет MSYS2 mingw-w64-ucrt-x86_64-libiio.
#
#   tools/build-libiio.sh [префикс]      # по умолчанию ~/.local/afar-deps
#   cmake ... -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$(brew --prefix);$HOME/.local/afar-deps"
set -euo pipefail

PREFIX="${1:-$HOME/.local/afar-deps}"
VERSION="v0.25"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ -f "$PREFIX/include/iio.h" ]; then
    echo "libiio уже установлен в $PREFIX"
    exit 0
fi

git clone --quiet --depth 1 --branch "$VERSION" \
    https://github.com/analogdevicesinc/libiio.git "$WORK/libiio"
cmake -S "$WORK/libiio" -B "$WORK/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_NAME_DIR="$PREFIX/lib" \
    -DOSX_FRAMEWORK=OFF -DOSX_PACKAGE=OFF \
    -DWITH_NETWORK_BACKEND=ON -DWITH_XML_BACKEND=ON \
    -DWITH_USB_BACKEND=OFF -DWITH_SERIAL_BACKEND=OFF \
    -DWITH_LOCAL_BACKEND=OFF -DWITH_IIOD=OFF \
    -DWITH_TESTS=OFF -DWITH_ZSTD=OFF -DHAVE_DNS_SD=OFF \
    -DCPP_BINDINGS=OFF -DPYTHON_BINDINGS=OFF -DWITH_DOC=OFF -DWITH_MAN=OFF
cmake --build "$WORK/build"
cmake --install "$WORK/build"
echo "libiio $VERSION установлен в $PREFIX"
