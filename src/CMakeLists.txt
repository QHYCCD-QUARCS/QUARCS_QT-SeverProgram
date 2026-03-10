cmake_minimum_required(VERSION 3.14)

project(client LANGUAGES CXX)

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

# 仅使用 Qt5（去掉 Qt6）
find_package(Qt5 REQUIRED COMPONENTS ${QT_COMPONENTS} SerialPort)

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

target_include_directories(guiding_offline_test PUBLIC
  /usr/include/libindi
  /usr/local/include/libstellarsolver
  /usr/local/include
)

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
  /usr/include/libindi
  /usr/local/include/libstellarsolver
)

link_directories(${PROJECT_SOURCE_DIR}/usr/lib/x86_64-linux-gnu)

target_link_libraries(client PUBLIC
  -lnova
  -lindidriver
  -lindiclient
  -lcfitsio
)

target_include_directories(client PUBLIC
    /usr/local/include
)

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

# 复制配置文件到构建目录
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/../qhyccd.ini
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
