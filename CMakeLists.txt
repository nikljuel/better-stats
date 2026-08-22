cmake_minimum_required(VERSION 3.21)
project(betterstats CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(third_party/pocketbook-sdk-qt6)

pocketbook_add_app(BetterStats
  SOURCES
    qt/src/main.cpp
    qt/src/inkview_bridge.cpp
    qt/src/stats_bridge.cpp
    qt/src/epub_cover.cpp
    qt/src/installer.cpp
    qt/src/file_handler_config.cpp
    src/tracker.c
    src/stats_db.c
    src/daemon.c
    qt/third_party/sqlite3.c
    qt/third_party/miniz.c
  QRC qt/qml/betterstats.qrc
)

target_include_directories(BetterStats PRIVATE qt/third_party src qt/src)
target_compile_definitions(BetterStats PRIVATE
  SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=1)
target_link_libraries(BetterStats PRIVATE m pthread dl)
