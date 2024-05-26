# install dependencies for C++ analysis
set -e

# update pacman
pacman --noconfirm -Suy

# install dependencies
pacman --noconfirm -S \
  mingw-w64-ucrt-x86_64-binutils \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-toolchain

# build
mkdir -p build
cd build || exit 1
cmake -G Ninja ..
ninja

# skip autobuild
echo "skip_autobuild=true" >> "$GITHUB_OUTPUT"
