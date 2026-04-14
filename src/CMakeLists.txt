cmake_minimum_required(VERSION 3.14)

project(client LANGUAGES CXX)

# ---------- Sysroot / 交叉编译：宿主 Qt 工具 + 目标库 ----------
# 使用 toolchain-rpi-arm64.cmake 时 CMAKE_SYSROOT 已设置；moc/uic/rcc 必须在宿主机运行。
if(CMAKE_SYSROOT)
  if(NOT DEFINED CMAKE_LIBRARY_ARCHITECTURE OR CMAKE_LIBRARY_ARCHITECTURE STREQUAL "")
    set(QUARCS_DEB_MULTIARCH "aarch64-linux-gnu")
  else()
    set(QUARCS_DEB_MULTIARCH "${CMAKE_LIBRARY_ARCHITECTURE}")
  endif()

  list(PREPEND CMAKE_PREFIX_PATH
    "${CMAKE_SYSROOT}/usr/lib/${QUARCS_DEB_MULTIARCH}/cmake"
    "${CMAKE_SYSROOT}/usr/lib/cmake"
    "${CMAKE_SYSROOT}/usr/local/lib/cmake"
  )

  find_program(QUARCS_HOST_MOC
    NAMES moc moc-qt5
    PATHS
      "/usr/lib/${CMAKE_HOST_SYSTEM_PROCESSOR}-linux-gnu/qt5/bin"
      /usr/lib/qt5/bin
      /usr/bin
    REQUIRED
    DOC "宿主机 Qt MOC（请安装 qtbase5-dev）")
  find_program(QUARCS_HOST_UIC
    NAMES uic uic-qt5
    PATHS
      "/usr/lib/${CMAKE_HOST_SYSTEM_PROCESSOR}-linux-gnu/qt5/bin"
      /usr/lib/qt5/bin
      /usr/bin
    REQUIRED)
  find_program(QUARCS_HOST_RCC
    NAMES rcc rcc-qt5
    PATHS
      "/usr/lib/${CMAKE_HOST_SYSTEM_PROCESSOR}-linux-gnu/qt5/bin"
      /usr/lib/qt5/bin
      /usr/bin
    REQUIRED)

  set(CMAKE_AUTOMOC_EXECUTABLE "${QUARCS_HOST_MOC}" CACHE FILEPATH "" FORCE)
  set(CMAKE_AUTOUIC_EXECUTABLE "${QUARCS_HOST_UIC}" CACHE FILEPATH "" FORCE)
  set(CMAKE_AUTORCC_EXECUTABLE "${QUARCS_HOST_RCC}" CACHE FILEPATH "" FORCE)

  set(QUARCS_INDI_INCLUDE "${CMAKE_SYSROOT}/usr/include/libindi")
  set(QUARCS_STELLARSOLVER_INCLUDE "${CMAKE_SYSROOT}/usr/local/include/libstellarsolver")
  set(QUARCS_LOCAL_INCLUDE "${CMAKE_SYSROOT}/usr/local/include")
  set(QUARCS_TARGET_LIBDIR "${CMAKE_SYSROOT}/usr/lib/${QUARCS_DEB_MULTIARCH}")
  set(QUARCS_TARGET_LIBDIR_ALT "${CMAKE_SYSROOT}/usr/lib")
  set(QUARCS_TARGET_LOCAL_LIBDIR "${CMAKE_SYSROOT}/usr/local/lib")
  set(QUARCS_TARGET_GCC_LIBDIR "${CMAKE_SYSROOT}/usr/lib/gcc/${QUARCS_DEB_MULTIARCH}/${QUARCS_SYSROOT_GCC_VERSION}")
  set(QUARCS_TARGET_CXX_INCLUDE "${CMAKE_SYSROOT}/usr/include/c++/${QUARCS_SYSROOT_GCC_VERSION}")
  set(QUARCS_TARGET_CXX_INCLUDE_MULTIARCH "${CMAKE_SYSROOT}/usr/include/${QUARCS_DEB_MULTIARCH}/c++/${QUARCS_SYSROOT_GCC_VERSION}")
  set(QUARCS_TARGET_CXX_INCLUDE_BACKWARD "${CMAKE_SYSROOT}/usr/include/c++/${QUARCS_SYSROOT_GCC_VERSION}/backward")
else()
  set(QUARCS_INDI_INCLUDE "/usr/include/libindi")
  set(QUARCS_STELLARSOLVER_INCLUDE "/usr/local/include/libstellarsolver")
  set(QUARCS_LOCAL_INCLUDE "/usr/local/include")
endif()

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ---------------- Sim guider test mode (compile-time) ----------------
# When enabled, the built-in guider can run without real guider camera / mount.
option(QUARCS_SIM_GUIDER "Use simulated guider (test guiding with synthetic star frames)" OFF)

set(QT_COMPONENTS
  Core
  Gui
  Widgets
  Concurrent
  Network
  WebSockets
  LinguistTools
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED ENV{QUARCS_QT_CLIENT_VERSION} AND NOT "$ENV{QUARCS_QT_CLIENT_VERSION}" STREQUAL "")
  set(QUARCS_QT_CLIENT_VERSION_VALUE "$ENV{QUARCS_QT_CLIENT_VERSION}")
else()
  string(TIMESTAMP QUARCS_QT_CLIENT_VERSION_VALUE "%Y%m%d%H%M")
endif()

message(STATUS "QUARCS Qt client build version: ${QUARCS_QT_CLIENT_VERSION_VALUE}")

if(CMAKE_SYSROOT)
  # Ubuntu 24.04 的 aarch64 交叉编译器会默认优先使用宿主机自带的 libstdc++ 头，
  # 这会把产物 ABI 抬高到树莓派 bookworm 运行时之上。
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>)
  include_directories(SYSTEM BEFORE
    "${CMAKE_SYSROOT}/usr/include/${QUARCS_DEB_MULTIARCH}"
    "${CMAKE_SYSROOT}/usr/include"
  )
  include_directories(SYSTEM BEFORE
    "${QUARCS_TARGET_CXX_INCLUDE}"
    "${QUARCS_TARGET_CXX_INCLUDE_MULTIARCH}"
    "${QUARCS_TARGET_CXX_INCLUDE_BACKWARD}"
  )
endif()

# 仅使用 Qt5（去掉 Qt6）
find_package(Qt5 REQUIRED COMPONENTS ${QT_COMPONENTS} SerialPort)

# find_package(Qt5) 会从 sysroot 导入 ARM 版 moc/uic/rcc，AUTOMOC 会误执行目标架构二进制。
# 必须再指回宿主机上的同名工具（与上方 QUARCS_HOST_* 一致）。
if(CMAKE_SYSROOT)
  if(TARGET Qt5::moc)
    set_property(TARGET Qt5::moc PROPERTY IMPORTED_LOCATION "${QUARCS_HOST_MOC}")
  endif()
  if(TARGET Qt5::uic)
    set_property(TARGET Qt5::uic PROPERTY IMPORTED_LOCATION "${QUARCS_HOST_UIC}")
  endif()
  if(TARGET Qt5::rcc)
    set_property(TARGET Qt5::rcc PROPERTY IMPORTED_LOCATION "${QUARCS_HOST_RCC}")
  endif()
endif()

find_package(OpenCV REQUIRED)
find_package(StellarSolver REQUIRED)

add_executable(client
  main.cpp
  websocketclient.h websocketclient.cpp
  mainwindow.h mainwindow.cpp
  guiding/GuiderTypes.h
  guiding/GuiderCore.h guiding/GuiderCore.cpp
  guiding/GuidingStarDetector.h guiding/GuidingStarDetector.cpp
  guiding/phd2/Phd2MountGuiding.h guiding/phd2/Phd2MountGuiding.cpp
  guiding/phd2/Phd2GuideAlgorithms.h
  guiding/phd2/Phd2MountCalibration.h guiding/phd2/Phd2MountCalibration.cpp
  guiding/CentroidUtils.h guiding/CentroidUtils.cpp
  guiding/MultiStarTracker.h guiding/MultiStarTracker.cpp
  guiding/DecBacklashEstimator.h guiding/DecBacklashEstimator.cpp
  guiding/SimGuiderFrameSource.h guiding/SimGuiderFrameSource.cpp
  tools.h tools.cpp
  sdks/SdkCommon.h
  sdks/SdkDriver.h
  sdks/SdkManager.h sdks/SdkManager.cpp
  sdks/SdkDriverRegistry.h sdks/SdkDriverRegistry.cpp
  sdks/SdkSerialExecutor.h sdks/SdkSerialExecutor.cpp
  sdks/SdkDevice.h sdks/SdkDevice.cpp
  sdks/LoggerAdapter.h
  sdks/QHYCCD/QHYCCD.h sdks/QHYCCD/QHYCCD.cpp
  sdks/QHYCCD/QHYFocuser.h sdks/QHYCCD/QHYFocuser.cpp
  focused_star_detection.cpp
  myclient.h myclient.cpp
  websocketthread.h websocketthread.cpp
  Logger.h Logger.cpp
  autopolaralignment.h autopolaralignment.cpp
  autofocus.h autofocus.cpp
  starsimulator.h starsimulator.cpp
  mountstate.h
  SerialDeviceDetector.h SerialDeviceDetector.cpp
)

if(QUARCS_SIM_GUIDER)
  target_compile_definitions(client PRIVATE QUARCS_SIM_GUIDER=1)
else()
  target_compile_definitions(client PRIVATE QUARCS_SIM_GUIDER=0)
endif()

target_compile_definitions(client PRIVATE QUARCS_QT_CLIENT_VERSION="${QUARCS_QT_CLIENT_VERSION_VALUE}")

# 离线导星算法测试（不依赖相机/赤道仪）
add_executable(guiding_offline_test
  tests/guiding_offline_test.cpp
  guiding/GuiderTypes.h
  guiding/GuiderCore.h guiding/GuiderCore.cpp
  guiding/GuidingStarDetector.h guiding/GuidingStarDetector.cpp
  guiding/phd2/Phd2MountGuiding.h guiding/phd2/Phd2MountGuiding.cpp
  guiding/phd2/Phd2GuideAlgorithms.h
  guiding/phd2/Phd2MountCalibration.h guiding/phd2/Phd2MountCalibration.cpp
  guiding/CentroidUtils.h guiding/CentroidUtils.cpp
  guiding/MultiStarTracker.h guiding/MultiStarTracker.cpp
  guiding/DecBacklashEstimator.h guiding/DecBacklashEstimator.cpp
  focused_star_detection.cpp
  tools.h tools.cpp
  Logger.h Logger.cpp
  websocketthread.h websocketthread.cpp
  websocketclient.h websocketclient.cpp
  mountstate.h
)

# sysroot 中 /usr/local/include 常含旧版 opencv2 树；-I 会优先于 -isystem(opencv4)，导致头文件与 apt 的 libopencv 不一致。
# 不要用 -I/usr/local/include，改为 -idirafter，使 QHY 等仍能找到，且 <opencv2/...> 先命中 /usr/include/opencv4。
if(CMAKE_SYSROOT)
  target_compile_options(client PRIVATE "-idirafter" "${CMAKE_SYSROOT}/usr/local/include")
  target_compile_options(guiding_offline_test PRIVATE "-idirafter" "${CMAKE_SYSROOT}/usr/local/include")
endif()

target_include_directories(guiding_offline_test PUBLIC
  "${QUARCS_INDI_INCLUDE}"
  "${QUARCS_STELLARSOLVER_INCLUDE}"
)
if(NOT CMAKE_SYSROOT)
  target_include_directories(guiding_offline_test PUBLIC "${QUARCS_LOCAL_INCLUDE}")
endif()

target_link_libraries(guiding_offline_test PRIVATE
  ${OpenCV_LIBS}
  -lcfitsio
  Qt5::Core
  Qt5::Gui
  Qt5::Widgets
  Qt5::Network
  Qt5::WebSockets
  indiclient
  ${ZLIB_LIBRARY}
  ${NOVA_LIBRARIES}
  qhyccd
  stellarsolver
  ${StellarSolver_LIBS}
)

target_link_libraries(client PRIVATE
    indiclient ${ZLIB_LIBRARY} ${NOVA_LIBRARIES}
)

IF (UNIX AND NOT APPLE)
    target_link_libraries(client PRIVATE -lpthread)
ENDIF ()

include(GNUInstallDirs)
install(TARGETS client
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

target_include_directories(client PUBLIC
  "${QUARCS_INDI_INCLUDE}"
  "${QUARCS_STELLARSOLVER_INCLUDE}"
)

# 原生 x86_64：保留项目内自带的 INDI 相关库路径（交叉编译时改用 sysroot）
if(CMAKE_SYSROOT)
  link_directories(
    "${QUARCS_TARGET_LIBDIR}"
    "${QUARCS_TARGET_LIBDIR_ALT}"
    "${QUARCS_TARGET_LOCAL_LIBDIR}"
  )
else()
  link_directories(${PROJECT_SOURCE_DIR}/usr/lib/x86_64-linux-gnu)
endif()

target_link_libraries(client PUBLIC
  -lnova
  -lindidriver
  -lindiclient
  -lcfitsio
)

if(NOT CMAKE_SYSROOT)
  target_include_directories(client PUBLIC "${QUARCS_LOCAL_INCLUDE}")
endif()

target_link_libraries(client PRIVATE
    qhyccd
    stellarsolver
)

# 这里全部固定为 Qt5::
target_link_libraries(client PRIVATE
    ${OpenCV_LIBS}
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Concurrent
    Qt5::Network
    Qt5::WebSockets
    Qt5::SerialPort
    ${StellarSolver_LIBS}
)

# Debian sysroot：liblapack.so.3 / libblas.so.3 常经 alternatives 指到绝对路径，交叉 ld 用 rpath-link 仍可能解析失败。
# 直接链接 multiarch/atlas 下真实 .so，满足 libopencv_core 对 LAPACK/BLAS 的传递依赖。
if(CMAKE_SYSROOT)
  set(_QUARCS_ATLAS_BLAS "${CMAKE_SYSROOT}/usr/lib/${QUARCS_DEB_MULTIARCH}/atlas/libblas.so.3")
  set(_QUARCS_ATLAS_LAPACK "${CMAKE_SYSROOT}/usr/lib/${QUARCS_DEB_MULTIARCH}/atlas/liblapack.so.3")
  if(EXISTS "${_QUARCS_ATLAS_BLAS}" AND EXISTS "${_QUARCS_ATLAS_LAPACK}")
    target_link_libraries(client PRIVATE "${_QUARCS_ATLAS_BLAS}" "${_QUARCS_ATLAS_LAPACK}")
    target_link_libraries(guiding_offline_test PRIVATE "${_QUARCS_ATLAS_BLAS}" "${_QUARCS_ATLAS_LAPACK}")
  endif()
  unset(_QUARCS_ATLAS_BLAS)
  unset(_QUARCS_ATLAS_LAPACK)
endif()

# 复制配置文件到构建目录
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/../qhyccd.ini
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
