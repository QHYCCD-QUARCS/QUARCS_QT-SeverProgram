cmake_minimum_required(VERSION 3.14)

project(client LANGUAGES CXX)

add_custom_target(CmakeAdditionalFiles
	SOURCES
	cmake/compiler_options.cmake)
LIST(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")
LIST(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/")
include(cmake/compiler_options.cmake)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(QT_COMPONENTS
  Core
  Gui
  Widgets
  Concurrent
  Network
  WebSockets
  LinguistTools
)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS ${QT_COMPONENTS})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS ${QT_COMPONENTS})
find_package(OpenCV REQUIRED)
find_package(StellarSolver REQUIRED)
find_package(INDI 2.0 REQUIRED)
find_package(CFITSIO REQUIRED)
find_package(Nova REQUIRED)
find_package(ZLIB REQUIRED)

add_subdirectory(src)

include(GNUInstallDirs)
install(TARGETS client
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
