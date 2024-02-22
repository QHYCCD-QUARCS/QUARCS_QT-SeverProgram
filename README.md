QUARCS_QT-SeverProgram
=====================

Ubuntu 22.04

1、Install Pre-requisites:
-

	sudo apt-get update
	sudo apt-get upgrade
	sudo apt-get install subversion
	sudo apt install build-essential cmake zlib1g-dev libgl1-mesa-dev libdrm-dev gcc g++ 
	sudo apt install graphviz doxygen gettext git 
	sudo apt-get install libxcb-xinerama0
	sudo snap install cmake --classic //此命令可以按照较新版本的cmake，Ubuntu18.04安装版本为3.25.2
	sudo apt-get install gnome-keyring
	sudo apt-get install libusb-1.0.0-dev
	sudo apt-get install libcfitsio-dev
	sudo apt-get install astrometry.net
	sudo apt-get install astrometry-data-tycho2

2、Install OPENCV:
-
Compile opencv from source and install it. (Recommended opencv version: 3.4.14 or 3.4.16)
Download link for version 3.4.14: https://codeload.github.com/opencv/opencv/zip/refs/tags/3.4.14

1.Environment configuration:

		sudo apt-get install build-essential 
		sudo apt-get install cmake git libgtk2.0-dev pkg-config libavcodec-dev libavformat-dev libswscale-dev
		sudo apt-get install python-dev python-numpy libtbb2 libtbb-dev libjpeg-dev libpng-dev libtiff-dev libjasper-dev libdc1394-dev
	
 2.Compile and install:
  
		cd opencv-3.4.14
		mkdir build
		cd build
		sudo cmake -D CMAKE_BUILD_TYPE=Release -D CMAKE_INSTALL_PREFIX=/usr/local ..
		sudo make	// sudo make -j4 
		sudo make install
		
3.Add path:
  
  	sudo gedit /etc/ld.so.conf
  
4.Add the following line to the file:

  	/usr/loacal/lib
		
5.Save and close, then run:

	sudo ldconfig

6.Configure environment: (Open .bashrc file)
 
 	sudo gedit /etc/bash.bashrc 
   
7.Add the following two lines at the end:

	PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/usr/local/lib/pkgconfig
	export PKG_CONFIG_PATH
 
8.Save and exit, then in the terminal:

	source /etc/bash.bashrc

9.Enter the following command to check the installed opencv version:

	pkg-config opencv --modversion

3、Install QHYCCD SDK
-
Download link for the compressed package: https://www.qhyccd.com/file/repository/publish/SDK/240109/sdk_linux64_24.01.09.tgz

Installation steps:

	tar xvf sdk_linux64_24.01.09.tgz
	cd sdk_linux64_24.01.09
	sudo bash install.sh

4、Install indi and indi-3rdparty driver library
-

Installation steps:
1. indi:
   
   Install Pre-requisites
   
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
   
   Create Project Directory
   
		mkdir -p ~/Projects
		cd ~/Projects

   Get the code
   
		git clone https://github.com/indilib/indi.git

   Build indi-core (cmake)

		mkdir -p ~/Projects/build/indi-core
		cd ~/Projects/build/indi-core
		cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Debug ~/Projects/indi
		make -j4
		sudo make install

2. indi-3rdparty:
   
   Install Pre-requisites
   
		  sudo apt-get -y install libnova-dev libcfitsio-dev libusb-1.0-0-dev zlib1g-dev libgsl-dev build-essential cmake git libjpeg-dev libcurl4-gnutls-dev libtiff-dev libfftw3-dev libftdi-dev libgps-dev libraw-dev libdc1394-dev libgphoto2-dev libboost-dev libboost-regex-dev librtlsdr-dev liblimesuite-dev libftdi1-dev libavcodec-dev libavdevice-dev libindi-dev
   
   Create Project Directory(It can be in the same folder as the first step of installing indi)
   
		mkdir -p ~/Projects
		cd ~/Projects

   Get the code
   
		git clone https://github.com/indilib/indi-3rdparty

   Building all the 3rd Party Libraries

		mkdir -p ~/Projects/build/indi-3rdparty-libs
		cd ~/Projects/build/indi-3rdparty-libs
		cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Debug -DBUILD_LIBS=1 ~/Projects/indi-3rdparty
		make -j4
		sudo make install

   Building all the 3rd Party Drivers

		mkdir -p ~/Projects/build/indi-3rdparty
		cd ~/Projects/build/indi-3rdparty
		cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Debug ~/Projects/indi-3rdparty
		make -j4
		sudo make install
   

5、Install QT components:
-
	sudo apt install qtcreator qtbase5-dev qtscript5-dev libqt5svg5-dev qttools5-dev-tools qttools5-dev libqt5opengl5-dev qtmultimedia5-dev libqt5multimedia5-plugins libqt5serialport5 libqt5serialport5-dev qtpositioning5-dev libgps-dev libqt5positioning5 libqt5positioning5-plugins qtwebengine5-dev libqt5charts5-dev libqt5websockets5-dev

6、Download QUARCS_QT-ServerProgram

   You can fork the this project into your own repositories and download it into workspace folder(lets assume it is in ~/workspace)

7、Compile and Run in VSCODE

- VSCODE can be used as the editor for this project.
- Install C/C++ Extension Pack in VScode's Extensions.	
- Open the QUARCS_QT-SeverProgram folder, configure the project with Cmake, select /src/CmakeLists.txt, and then select CXX 11.4.0.
- You can then run QT-SeverProgram directly in VScode.

8、Compile and Run in Terminal (assume the project in ~/workspace/QUARCS_QT-SeverProgram)

	cd ~/workspace/QUARCS_QT-SeverProgram/src
	make build
	cd build
	cmake ..
	make 
	make install
   
   the compiled filename is "client"  by default it is be installed in /usr/local/bin  you can run it

	client




   
   