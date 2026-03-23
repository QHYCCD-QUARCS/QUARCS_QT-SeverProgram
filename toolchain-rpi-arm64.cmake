# Raspberry Pi (aarch64) 交叉编译工具链 + sysroot
#
# 用法（在仓库根目录）:
#   cmake -S src -B build-rpi \
#     -DCMAKE_TOOLCHAIN_FILE=../toolchain-rpi-arm64.cmake
#
# 可选：指定 sysroot（默认 /home/quarcs/rpi-sysroot）
#   cmake ... -DCMAKE_SYSROOT=/path/to/sysroot
#
# 宿主机需安装:
#   sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu pkg-config cmake
#   sudo apt install qtbase5-dev qttools5-dev-tools   # 提供与本机架构一致的 moc/uic/rcc
#
# 树莓派上需已安装与本项目链接的 dev 包（再 rsync 到 sysroot）:
#   Qt5、OpenCV、libindi、stellarsolver、libcfitsio、qhyccd 等

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT CMAKE_SYSROOT)
  set(CMAKE_SYSROOT "/home/quarcs/rpi-sysroot" CACHE PATH "树莓派根文件系统（rsync 同步目录）")
endif()

# Debian/Ubuntu multiarch 库目录名（与 Pi 上 dpkg-architecture -qDEB_HOST_MULTIARCH 一致）
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 目标 pkg-config（OpenCV 等）
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
  "${CMAKE_SYSROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/pkgconfig:${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig:${CMAKE_SYSROOT}/usr/local/lib/pkgconfig")

# 链接时使用 sysroot；rpath-link 让 ld 在链接阶段解析 .so 的传递依赖（如 opencv_core → lapack/blas）
set(_QUARCS_RL "-Wl,-rpath-link,${CMAKE_SYSROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE} -Wl,-rpath-link,${CMAKE_SYSROOT}/lib/${CMAKE_LIBRARY_ARCHITECTURE} -Wl,-rpath-link,${CMAKE_SYSROOT}/usr/local/lib")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--sysroot=${CMAKE_SYSROOT} ${_QUARCS_RL}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "--sysroot=${CMAKE_SYSROOT} ${_QUARCS_RL}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "--sysroot=${CMAKE_SYSROOT} ${_QUARCS_RL}")
unset(_QUARCS_RL)
