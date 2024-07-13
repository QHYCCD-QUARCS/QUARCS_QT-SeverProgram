# QUARCS_QT-SeverProgram

## Build(Ubuntu)

### Install Dependencies

```shell
sudo apt-get update
sudo apt-get upgrade
sudo apt-get install subversion
sudo apt install build-essential cmake zlib1g-dev libgl1-mesa-dev libdrm-dev gcc g++
sudo apt install graphviz doxygen gettext git 

sudo snap install cmake --classic //此命令可以按照较新版本的cmake，Ubuntu18.04安装版本为3.25.2

sudo apt-get install libxcb-xinerama0
sudo apt-get install gnome-keyring

sudo apt-get install libusb-1.0.0-dev
sudo apt-get install libcfitsio-dev

sudo apt-get install astrometry.net
sudo apt-get install astrometry-data-tycho2
sudo apt install libastrometry-dev
```

### Install OpenCV

#### Install via apt-get

```shell
sudo apt-get install libopencv-dev
```

#### Install OpenCV from source

Compile opencv from source and install it. (Recommended opencv version: 3.4.14 or 3.4.16)
Download link for version 3.4.14: <https://codeload.github.com/opencv/opencv/zip/refs/tags/3.4.14>

##### Environment configuration

```shell
sudo apt-get install build-essential 
sudo apt-get install cmake git libgtk2.0-dev pkg-config libavcodec-dev libavformat-dev libswscale-dev
sudo apt-get install python-dev python-numpy libtbb2 libtbb-dev libjpeg-dev libpng-dev libtiff-dev libjasper-dev libdc1394-dev
```
  
##### Compile and install

```shell
cd opencv-3.4.14
mkdir build
cd build
sudo cmake -D CMAKE_BUILD_TYPE=Release -D CMAKE_INSTALL_PREFIX=/usr/local ..
sudo make // sudo make -j4
sudo make install
```

##### Add path

```shell
# If your system does not have a graphical interface, you can use Nano instead
sudo gedit /etc/ld.so.conf # Add the following line to the file: /usr/local/lib
sudo ldconfig
sudo gedit /etc/bash.bashrc

# Add the following two lines at the end:
# PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/usr/local/lib/pkgconfig
# export PKG_CONFIG_PATH

source /etc/bash.bashrc

# Enter the following command to check the installed opencv version
pkg-config opencv --modversion
```

### Install QHYCCD SDK

Download link for the compressed package: <https://www.qhyccd.com/file/repository/publish/SDK/240109/sdk_linux64_24.01.09.tgz>

```shell
tar xvf sdk_linux64_24.01.09.tgz
cd sdk_linux64_24.01.09

sudo cp -r usr/local /usr
sudo cp -r usr/share /usr
```

### Install INDI

#### Install via apt

```shell
sudo apt-add-repository ppa:mutlaqja/ppa
sudo apt-get update

sudo apt-get install indi-full gsc libindi-dev
```

#### Install from source

##### INDI

```shell
sudo apt-get install -y \
    git \
    cdbs \
    dkms \
    cmake \
    fxload \
    libev-dev \
    libgps-dev \
    libgsl-dev \
    libraw-dev \
    libusb-dev \
    zlib1g-dev \
    libftdi-dev \
    libjpeg-dev \
    libkrb5-dev \
    libnova-dev \
    libtiff-dev \
    libfftw3-dev \
    librtlsdr-dev \
    libcfitsio-dev \
    libgphoto2-dev \
    build-essential \
    libusb-1.0-0-dev \
    libdc1394-dev \
    libboost-regex-dev \
    libcurl4-gnutls-dev \
    libtheora-dev

git clone https://github.com/indilib/indi.git
cd indi
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Debug ..
make -j4
sudo make install
```

##### indi-3rdparty

```shell
sudo apt-get -y install libnova-dev libcfitsio-dev libusb-1.0-0-dev zlib1g-dev libgsl-dev build-essential cmake git libjpeg-dev libcurl4-gnutls-dev libtiff-dev libfftw3-dev libftdi-dev libgps-dev libraw-dev libdc1394-dev libgphoto2-dev libboost-dev libboost-regex-dev librtlsdr-dev liblimesuite-dev libftdi1-dev libavcodec-dev libavdevice-dev libindi-dev

git clone https://github.com/indilib/indi-3rdparty.git
cd indi-3rdparty
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Debug ..
make -j4
sudo make install
```

### Install QT components

```shell
sudo apt install qtcreator qtbase5-dev qtscript5-dev libqt5svg5-dev qttools5-dev-tools qttools5-dev libqt5opengl5-dev qtmultimedia5-dev libqt5multimedia5-plugins libqt5serialport5 libqt5serialport5-dev qtpositioning5-dev libgps-dev libqt5positioning5 libqt5positioning5-plugins qtwebengine5-dev libqt5charts5-dev libqt5websockets5-dev
```

### Install StellarSolver

```shell
sudo apt install libstellarsolver-dev
```

### Build and Run

You can fork the this project into your own repositories and download it into workspace folder(lets assume it is in ~/)

```shell
cd QUARCS_QT-SeverProgram
mkdir build && cd build
cmake ..
make

./client # run the server
```

## Contact

Welcome to join the QUARUS Discod discussion group for online discussions (<https://discord.gg/uHTPfJ5uuV>)
