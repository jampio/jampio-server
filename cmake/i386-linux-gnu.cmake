set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i386)
set(CMAKE_SYSTEM_VERSION gnu)
set(CMAKE_LIBRARY_ARCHITECTURE i386-linux-gnu)
set(CMAKE_C_LIBRARY_ARCHITECTURE i386-linux-gnu)
set(CMAKE_CXX_LIBRARY_ARCHITECTURE i386-linux-gnu)
set(CMAKE_C_COMPILER_TARGET i386-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET i386-linux-gnu)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32 -march=i386")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m32 -march=i386")
# jampio - I'm not sure why, but this must be set
# otherwise pkg config continues to only read from host arch
# so far this is the best I can do
set(ENV{PKG_CONFIG_PATH} /lib/i386-linux-gnu/pkgconfig:/usr/lib/i386-linux-gnu/pkgconfig:/usr/local/lib/i386-linux-gnu/pkgconfig)