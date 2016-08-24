#!/bin/sh

libs() {
    # todo(jamydev): install opus
    brew install ffmpeg --with-fdk-aac --with-ffplay --with-freetype --with-libass --with-libquvi --with-libvorbis --with-libvpx --with-opus
}

dependencies() {
    echo "@@ Installing dependencies..."
    mkdir build_deps
    cd build_deps
    # TODO: Install deps
    ftl_sdk
    tachyon_utils
    ftl_express
#    export CMAKE_PREFIX_PATH=/usr/local/Cellar/qt5/5.6.0/lib/cmake/Qt5Widgets
    cd ..
}

build() {
    echo "@@ Building Tachyon..."
    mkdir build
    cd build
    cmake -DOBS_VERSION_OVERRIDE=1.2.11 ..
    make
    cp -r ../build_deps/tachyon-utils/install/osx/* ./
    cp -r ../build_deps/ftl-sdk/build/libftl* ./rundir/RelWithDebInfo
    cp ../build_deps/go/bin/ftl-express ./rundir/RelWithDebInfo/bin/ftl-express
    sudo python2.7 build_app.py
    sudo chown -R :staff Tachyon.app
}

archive() {
    echo "@@ Archiving build to DMG..."
    hdiutil create -fs HFS+ -megabytes 70 -volname 'Tachyon Install' Tachyon.dmg
    hdiutil mount Tachyon.dmg
    cp -r ./Tachyon.app "/Volumes/Tachyon Install/Tachyon.app"
    hdiutil unmount "/Volumes/Tachyon Install"
    mkdir dmg
    cp Tachyon.dmg dmg/Tachyon.dmg
}

ftl_sdk() {
    echo "@@ Cloning ftl-sdk..."
    git clone git@github.com:WatchBeam/ftl-sdk
    cd ftl-sdk
    mkdir build
    cd build
    echo "@@ Building ftl-sdk..."
    cmake .. && make
    export FTLSDK_INCLUDE_DIR=$(pwd)
    echo "@@ Using FTL-SDK from $FTLSDK_INCLUDE_DIR"
    cd ../..
}

tachyon_utils() {
    echo "@@ Cloning tachyon_utils..."
    git clone git@github.com:WatchBeam/tachyon-utils
    cd tachyon-utils
    git checkout tachyon-mac
    cd ..
}

ftl_express() {
    echo "@@ Getting ftl-express..."
    mkdir go
    cd go
    export GOPATH=$(pwd)
    go get -u github.com/WatchBeam/ftl-express
    cd ..
}

rm -rf build build_deps

dependencies
build
archive
