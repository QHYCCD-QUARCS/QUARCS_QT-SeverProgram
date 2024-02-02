cmake_minimum_required(VERSION 3.14)

project(client LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
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

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS ${QT_COMPONENTS})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS ${QT_COMPONENTS})
find_package(OpenCV REQUIRED)

add_executable(client
  main.cpp
  websocketclient.h websocketclient.cpp
  mainwindow.h mainwindow.cpp
  tools.hpp tools.cpp
  myclient.h myclient.cpp
  websocketthread.h websocketthread.cpp
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

target_include_directories(client PUBLIC /usr/include/libindi)
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
)

target_link_libraries(client PRIVATE
    ${OpenCV_LIBS}
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Concurrent
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::WebSockets
)

