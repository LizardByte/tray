# install dependencies for C++ analysis
set -e

# install dependencies
brew install \
  cmake \
  ninja

# build
mkdir -p build
cmake \
  -DBUILD_DOCS=OFF \
  -B build \
  -G Ninja \
  -S .
ninja -C build

# skip autobuild
echo "skip_autobuild=true" >> "$GITHUB_OUTPUT"
