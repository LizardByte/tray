# install dependencies for C++ analysis
set -e

# install dependencies
brew install \
  cmake \
  ninja

# build
mkdir -p build
cd build || exit 1
cmake -G Ninja ..
ninja

# skip autobuild
echo "skip_autobuild=true" >> "$GITHUB_OUTPUT"
