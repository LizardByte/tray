# install dependencies for C++ analysis
set -e

sudo apt-get update -y

# allow newer gcc
sudo add-apt-repository ppa:ubuntu-toolchain-r/test -y

sudo apt-get install -y \
  build-essential \
  cmake \
  libayatana-appindicator3-dev \
  libglib2.0-dev \
  libnotify-dev \
  ninja-build

# clean apt cache
sudo apt-get clean
sudo rm -rf /var/lib/apt/lists/*

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
