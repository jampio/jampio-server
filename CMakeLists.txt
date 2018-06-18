cmake_minimum_required(VERSION 3.10)
project(jampioded)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
include(CheckTypeSize)
check_type_size("void*" CMAKE_SIZEOF_VOID_P)
message(STATUS "sizeof void*: " ${CMAKE_SIZEOF_VOID_P})
message(STATUS "CMAKE_CROSSCOMPILING: " ${CMAKE_CROSSCOMPILING})
message(STATUS "CMAKE_SYSTEM_PROCESSOR: " ${CMAKE_SYSTEM_PROCESSOR})
message(STATUS "CMAKE_LIBRARY_ARCHITECTURE: " ${CMAKE_LIBRARY_ARCHITECTURE})
find_package(jampiocommonded REQUIRED)
add_executable(jampioded
	src/bot.cpp
	src/ccmds.cpp
	src/client.cpp
	src/game.cpp
	src/main.cpp
	src/net_chan.cpp
	src/snapshot.cpp
	src/world.cpp
)
include(GNUInstallDirs)
target_link_libraries(jampioded jampiocommonded)
target_include_directories(jampioded PRIVATE ${jampiocommonded_INCLUDE})
install(TARGETS jampioded DESTINATION ${CMAKE_INSTALL_BINDIR})