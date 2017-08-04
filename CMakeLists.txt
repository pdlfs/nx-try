#
# CMakeLists.txt  cmake for nx-try
# 14-Jun-2017  chuck@ece.cmu.edu
#

#
# specify the location of mercury, nexus, etc.  with CMAKE_INSTALL_PATH or
# CMAKE_PREFIX_PATH...
#
cmake_minimum_required(VERSION 3.1)

project (nx-try C CXX)

set (CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

set (FAKE_NEXUS "OFF" CACHE BOOL "Use fake nexus lib")

set (CMAKE_THREAD_PREFER_PTHREAD TRUE)
set (THREADS_PREFER_PTHREAD_FLAG TRUE)
# XXX: cmake 3.1 and newer define a Threads::Threads imported target
# that we should switch too when we are ready to require 3.1 or better.
# (3.1 was released late 2014)
find_package (Threads REQUIRED)

find_package (mercury CONFIG REQUIRED)
if (FAKE_NEXUS)
    find_package (MPI MODULE REQUIRED)
    find_package (mercury CONFIG REQUIRED)
    set (nxdep mercury)
    set (xtracc fake_nexus.cc)
else ()
    find_package (deltafs-nexus CONFIG REQUIRED)
    set (nxdep deltafs-nexus)
endif ()

add_executable (nx-try nx-try.cc ${xtracc})
target_include_directories (nx-try PUBLIC ${MERCURY_INCLUDE_DIR})
target_link_libraries (nx-try ${nxdep} Threads::Threads)

if (FAKE_NEXUS)
    target_compile_definitions (nx-try PUBLIC FAKE_NEXUS)
    target_include_directories (nx-try PUBLIC ${MPI_CXX_INCLUDE_PATH})
    target_link_libraries (nx-try ${MPI_CXX_LIBRARIES})
    set_property (TARGET nx-try APPEND PROPERTY LINK_FLAGS
              ${MPI_CXX_LINK_FLAGS})
endif ()

#
# "make install" rule
#
install (TARGETS nx-try RUNTIME DESTINATION bin)
