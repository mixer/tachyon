#!/bin/sh
mkdir build
cd build
# TODO: Install deps
export CMAKE_PREFIX_PATH=/usr/local/Cellar/qt5/5.6.0/lib/cmake/Qt5Widgets
export FTLSDK_INCLUDE_DIR="/Users/jamy/Programming/beam/ftl-sdk/build"
cmake ..
make
cp -r /Users/jamy/Programming/beam/obs-studio-utils/install/osx/* ./
cp -r /Users/jamy/Programming/beam/ftl-sdk/build/libftl* ./rundir/RelWithDebInfo
sudo python2.7 build_app.py
sudo chown -R jamy:staff Tachyon.app
hdiutil create -fs HFS+ -megabytes 70 -volname 'Tachyon Install' Tachyon.dmg
hdiutil mount Tachyon.dmg
cp -r ./Tachyon.app "/Volumes/Tachyon Install/Tachyon.app"
hdiutil unmount "/Volumes/Tachyon Install"
mkdir dmg
cp Tachyon.dmg dmg/Tachyon.dmg
