#!/bin/bash

set -e

# 默认使用apt
USE_APT=false
USE_YUM=false
USE_GIT=false

# 检查是否使用apt或yum
if command -v apt-get &> /dev/null; then
    USE_APT=true
elif command -v yum &> /dev/null; then
    USE_YUM=true
else
    echo "Unsupported package manager. Only apt and yum are supported."
    exit 1
fi

# 解析命令行参数
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --use-git) USE_GIT=true ;;
        *) echo "Unknown parameter passed: $1"; exit 1 ;;
    esac
    shift
done

install_dependencies_apt() {
    sudo apt-get update
    sudo apt-get upgrade -y
    sudo apt-get install -y subversion build-essential cmake zlib1g-dev libgl1-mesa-dev libdrm-dev gcc g++ graphviz doxygen gettext git cmake \
                            libxcb-xinerama0 gnome-keyring libusb-1.0.0-dev libcfitsio-dev astrometry.net astrometry-data-tycho2 libastrometry-dev
}

install_dependencies_yum() {
    sudo yum groupinstall -y "Development Tools"
    sudo yum install -y epel-release
    sudo yum install -y subversion cmake zlib-devel mesa-libGL-devel mesa-libGLU-devel libdrm-devel gcc gcc-c++ graphviz doxygen gettext git \
                         libxcb libusbx-devel cfitsio-devel astrometry.net astrometry-data-tycho2 astrometry-devel
}

install_opencv_apt() {
    sudo apt-get install -y libopencv-dev
}

install_opencv_yum() {
    sudo yum install -y opencv-devel
}

install_opencv_from_source() {
    OPENCV_VERSION="3.4.14"
    wget https://codeload.github.com/opencv/opencv/zip/refs/tags/${OPENCV_VERSION} -O opencv-${OPENCV_VERSION}.zip
    unzip opencv-${OPENCV_VERSION}.zip
    cd opencv-${OPENCV_VERSION}
    mkdir build && cd build
    cmake -D CMAKE_BUILD_TYPE=Release -D CMAKE_INSTALL_PREFIX=/usr/local ..
    make -j$(nproc)
    sudo make install
    cd ../..
    rm -rf opencv-${OPENCV_VERSION} opencv-${OPENCV_VERSION}.zip
    sudo sh -c 'echo "/usr/local/lib" > /etc/ld.so.conf.d/opencv.conf'
    sudo ldconfig
    echo 'PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/usr/local/lib/pkgconfig' | sudo tee -a /etc/bash.bashrc
    source /etc/bash.bashrc
}

install_qhyccd_sdk() {
    wget https://www.qhyccd.com/file/repository/publish/SDK/240109/sdk_linux64_24.01.09.tgz
    tar xvf sdk_linux64_24.01.09.tgz
    cd sdk_linux64_24.01.09
    sudo cp -r usr/local /usr
    sudo cp -r usr/share /usr
    cd ..
    rm -rf sdk_linux64_24.01.09 sdk_linux64_24.01.09.tgz
}

install_indi_apt() {
    sudo apt-add-repository ppa:mutlaqja/ppa -y
    sudo apt-get update
    sudo apt-get install -y indi-full gsc libindi-dev
}

install_indi_yum() {
    sudo yum install -y libindi
}

install_indi_from_source() {
    git clone https://github.com/indilib/indi.git
    cd indi
    mkdir build && cd build
    cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Debug ..
    make -j$(nproc)
    sudo make install
    cd ../..
    rm -rf indi

    git clone https://github.com/indilib/indi-3rdparty.git
    cd indi-3rdparty
    mkdir build && cd build
    cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Debug ..
    make -j$(nproc)
    sudo make install
    cd ../..
    rm -rf indi-3rdparty
}

install_qt_components_apt() {
    sudo apt install -y qtcreator qtbase5-dev qtscript5-dev libqt5svg5-dev qttools5-dev-tools qttools5-dev libqt5opengl5-dev \
                        qtmultimedia5-dev libqt5multimedia5-plugins libqt5serialport5 libqt5serialport5-dev qtpositioning5-dev \
                        libgps-dev libqt5positioning5 libqt5positioning5-plugins qtwebengine5-dev libqt5charts5-dev libqt5websockets5-dev
}

install_qt_components_yum() {
    sudo yum install -y qt5-qtbase-devel qt5-qtscript-devel qt5-qtsvg-devel qt5-qttools-devel qt5-qttools qt5-qtbase qt5-qtbase-gui \
                         qt5-qtmultimedia-devel qt5-qtmultimedia qt5-qtmultimedia-plugins qt5-qtserialport qt5-qtserialport-devel \
                         qt5-qtlocation-devel qt5-qtlocation qt5-qtwebengine-devel qt5-qtcharts-devel qt5-qtnetworkauth-devel qt5-qtnetworkauth \
                         qt5-qtwebsockets-devel qt5-qtwebsockets
}

install_stellarsolver_apt() {
    sudo apt install -y libstellarsolver-dev
}

install_stellarsolver_yum() {
    sudo yum install -y stellarsolver-devel
}

build_and_run() {
    mkdir -p build && cd build
    cmake ..
    make
    ./client # 运行服务器
}

if $USE_APT; then
    install_dependencies_apt
    install_opencv_apt
    install_indi_apt
    install_qt_components_apt
    install_stellarsolver_apt
elif $USE_YUM; then
    install_dependencies_yum
    install_opencv_yum
    install_indi_yum
    install_qt_components_yum
    install_stellarsolver_yum
fi

if $USE_GIT; then
    install_opencv_from_source
    install_indi_from_source
fi

install_qhyccd_sdk

build_and_run

echo "Build and installation complete. Welcome to join the QUARCS Discord discussion group for online discussions (https://discord.gg/uHTPfJ5uuV)"
