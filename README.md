QUARCS_QT-SeverProgram
=====================

Ubuntu 22.04

1、安装必要的库：
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

2、安装OPENCV
-
从源码编译opencv并安装。（opencv版本建议使用3.4.14 或者3.4.16）
3.4.14版本下载链接：

	https://codeload.github.com/opencv/opencv/zip/refs/tags/3.4.14

环境配置:

		sudo apt-get install build-essential 
		sudo apt-get install cmake git libgtk2.0-dev pkg-config libavcodec-dev libavformat-dev libswscale-dev
		sudo apt-get install python-dev python-numpy libtbb2 libtbb-dev libjpeg-dev libpng-dev libtiff-dev libjasper-dev libdc1394-dev
	
 编译安装：
  
		cd opencv-3.4.14
		mkdir build
		cd build
		sudo cmake -D CMAKE_BUILD_TYPE=Release -D CMAKE_INSTALL_PREFIX=/usr/local ..
		sudo make	// sudo make -j4 
		sudo make install
		
添加路径:
  
  	sudo gedit /etc/ld.so.conf
  
在文件中添加如下代码：

  	/usr/loacal/lib
		
保存关闭，运行下面代码：

	sudo ldconfig

配置环境:（打开.bashrc文件）
 
 	sudo gedit /etc/bash.bashrc 
   
添加下面两行代码，放到最后面即可：

	PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/usr/local/lib/pkgconfig
	export PKG_CONFIG_PATH
 
保存退出，终端输入：

	source /etc/bash.bashrc

输入以下命令，可以查看所安装opencv的版本:

	pkg-config opencv --modversion

3、安装QHYCCD SDK
-
压缩包下载链接：https://www.qhyccd.com/file/repository/publish/SDK/240109/sdk_linux64_24.01.09.tgz

	tar xvf sdk_linux64_24.01.09.tgz
	cd sdk_linux64_24.01.09
	sudo bash install.sh

4、安装INDI和INDI三方驱动库
-
具体安装可访问以下GitHub库：

	https://github.com/indilib/indi.git
	https://github.com/indilib/indi-3rdparty.git

5、安装QT组件：
-
	sudo apt install qtcreator qtbase5-dev qtscript5-dev libqt5svg5-dev qttools5-dev-tools qttools5-dev libqt5opengl5-dev qtmultimedia5-dev libqt5multimedia5-plugins libqt5serialport5 libqt5serialport5-dev qtpositioning5-dev libgps-dev libqt5positioning5 libqt5positioning5-plugins qtwebengine5-dev libqt5charts5-dev libqt5websockets5-dev
	
5、安装Visual Studio Code
-
用VSCODE可作为本工程的编辑器.
在VScode的Extensions中安装C/C++ Extension Pack.	
打开QUARCS_QT-SeverProgram文件夹，用Cmake对项目进行配置，选择/src/CmakeLists.txt，然后选择CXX 11.4.0
之后可以直接在VScode里运行QT-SeverProgram.




		



