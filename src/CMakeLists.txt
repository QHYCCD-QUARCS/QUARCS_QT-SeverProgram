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

if (UNIX AND NOT APPLE)
    target_link_libraries(client PRIVATE pthread)
endif()

link_directories(${PROJECT_SOURCE_DIR}/usr/lib/x86_64-linux-gnu)

target_link_libraries(client PUBLIC
  nova
  indidriver
  indiclient
  cfitsio
)

target_link_libraries(client PRIVATE
    qhyccd
    stellarsolver
    ${OpenCV_LIBS}
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Concurrent
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::WebSockets
    ${StellarSolver_LIBS}
)
