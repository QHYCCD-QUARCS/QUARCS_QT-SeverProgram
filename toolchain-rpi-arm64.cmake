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

# 交叉编译器的 GCC 主版本必须与 sysroot 里的 libstdc++/glibc 时代尽量一致；
# 否则 g++ 会优先使用宿主机交叉工具链自带的 C++ 头/库，生成高于树莓派运行时的 ABI 需求。
file(GLOB _QUARCS_SYSROOT_GCC_DIRS LIST_DIRECTORIES true
  "${CMAKE_SYSROOT}/usr/lib/gcc/${CMAKE_LIBRARY_ARCHITECTURE}/*")
list(SORT _QUARCS_SYSROOT_GCC_DIRS COMPARE NATURAL ORDER DESCENDING)
set(_QUARCS_SYSROOT_GCC_VERSION "")
foreach(_QUARCS_GCC_DIR IN LISTS _QUARCS_SYSROOT_GCC_DIRS)
  if(IS_DIRECTORY "${_QUARCS_GCC_DIR}")
    get_filename_component(_QUARCS_SYSROOT_GCC_VERSION "${_QUARCS_GCC_DIR}" NAME)
    break()
  endif()
endforeach()

if(_QUARCS_SYSROOT_GCC_VERSION STREQUAL "")
  message(FATAL_ERROR
    "Unable to detect GCC version from sysroot: "
    "${CMAKE_SYSROOT}/usr/lib/gcc/${CMAKE_LIBRARY_ARCHITECTURE}")
endif()

set(QUARCS_SYSROOT_GCC_VERSION "${_QUARCS_SYSROOT_GCC_VERSION}" CACHE INTERNAL "GCC version detected from target sysroot")

string(REGEX MATCH "^[0-9]+" _QUARCS_SYSROOT_GCC_MAJOR "${_QUARCS_SYSROOT_GCC_VERSION}")
if(_QUARCS_SYSROOT_GCC_MAJOR STREQUAL "")
  message(FATAL_ERROR "Unable to parse sysroot GCC major version: ${_QUARCS_SYSROOT_GCC_VERSION}")
endif()
set(QUARCS_SYSROOT_GCC_MAJOR "${_QUARCS_SYSROOT_GCC_MAJOR}" CACHE INTERNAL "GCC major version detected from target sysroot")

# CMake 会缓存 find_program 结果；如果之前配置过不同主版本的交叉工具链，
# 这里需要先清掉旧缓存，避免重新配置时继续沿用旧的 -11/-12 路径。
unset(_QUARCS_CC CACHE)
unset(_QUARCS_CXX CACHE)
unset(_QUARCS_AR CACHE)
unset(_QUARCS_RANLIB CACHE)
unset(_QUARCS_NM CACHE)

find_program(_QUARCS_CC
  NAMES
    "aarch64-linux-gnu-gcc-${_QUARCS_SYSROOT_GCC_MAJOR}"
    aarch64-linux-gnu-gcc
  NO_CMAKE_FIND_ROOT_PATH
  REQUIRED)
find_program(_QUARCS_CXX
  NAMES
    "aarch64-linux-gnu-g++-${_QUARCS_SYSROOT_GCC_MAJOR}"
    aarch64-linux-gnu-g++
  NO_CMAKE_FIND_ROOT_PATH
  REQUIRED)
find_program(_QUARCS_AR
  NAMES
    "aarch64-linux-gnu-gcc-ar-${_QUARCS_SYSROOT_GCC_MAJOR}"
    aarch64-linux-gnu-gcc-ar
  NO_CMAKE_FIND_ROOT_PATH
  REQUIRED)
find_program(_QUARCS_RANLIB
  NAMES
    "aarch64-linux-gnu-gcc-ranlib-${_QUARCS_SYSROOT_GCC_MAJOR}"
    aarch64-linux-gnu-gcc-ranlib
  NO_CMAKE_FIND_ROOT_PATH
  REQUIRED)
find_program(_QUARCS_NM
  NAMES
    "aarch64-linux-gnu-gcc-nm-${_QUARCS_SYSROOT_GCC_MAJOR}"
    aarch64-linux-gnu-gcc-nm
  NO_CMAKE_FIND_ROOT_PATH
  REQUIRED)

execute_process(
  COMMAND "${_QUARCS_CXX}" -dumpversion
  OUTPUT_VARIABLE _QUARCS_CXX_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX MATCH "^[0-9]+" _QUARCS_CXX_MAJOR "${_QUARCS_CXX_VERSION}")

if(NOT _QUARCS_CXX_MAJOR STREQUAL _QUARCS_SYSROOT_GCC_MAJOR)
  message(FATAL_ERROR
    "Cross compiler major version (${_QUARCS_CXX_VERSION}) does not match sysroot GCC "
    "major version (${_QUARCS_SYSROOT_GCC_VERSION}). "
    "Install matching cross compilers, for example:\n"
    "  sudo apt install gcc-${_QUARCS_SYSROOT_GCC_MAJOR}-aarch64-linux-gnu "
    "g++-${_QUARCS_SYSROOT_GCC_MAJOR}-aarch64-linux-gnu")
endif()

set(CMAKE_C_COMPILER   "${_QUARCS_CC}" CACHE FILEPATH "Cross C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${_QUARCS_CXX}" CACHE FILEPATH "Cross CXX compiler" FORCE)
set(CMAKE_C_COMPILER_AR "${_QUARCS_AR}" CACHE FILEPATH "Cross C compiler archiver" FORCE)
set(CMAKE_CXX_COMPILER_AR "${_QUARCS_AR}" CACHE FILEPATH "Cross CXX compiler archiver" FORCE)
set(CMAKE_C_COMPILER_RANLIB "${_QUARCS_RANLIB}" CACHE FILEPATH "Cross C compiler ranlib" FORCE)
set(CMAKE_CXX_COMPILER_RANLIB "${_QUARCS_RANLIB}" CACHE FILEPATH "Cross CXX compiler ranlib" FORCE)
set(CMAKE_NM "${_QUARCS_NM}" CACHE FILEPATH "Cross nm" FORCE)

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 让 g++ 在自动追加 -lstdc++ / -lgcc_s 时优先命中 sysroot 中与树莓派一致的运行库。
set(_QUARCS_LIBRARY_PATH_PREFIX
  "${CMAKE_SYSROOT}/usr/lib/gcc/${CMAKE_LIBRARY_ARCHITECTURE}/${QUARCS_SYSROOT_GCC_VERSION}:"
  "${CMAKE_SYSROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}:"
  "${CMAKE_SYSROOT}/lib/${CMAKE_LIBRARY_ARCHITECTURE}")
set(ENV{LIBRARY_PATH} "${_QUARCS_LIBRARY_PATH_PREFIX}:$ENV{LIBRARY_PATH}")
unset(_QUARCS_LIBRARY_PATH_PREFIX)

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
unset(_QUARCS_GCC_DIR)
unset(_QUARCS_SYSROOT_GCC_DIRS)
unset(_QUARCS_AR)
unset(_QUARCS_RANLIB)
unset(_QUARCS_NM)
