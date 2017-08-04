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

set (CMAKE_THREAD_PREFER_PTHREAD TRUE)
set (THREADS_PREFER_PTHREAD_FLAG TRUE)
# XXX: cmake 3.1 and newer define a Threads::Threads imported target
# that we should switch too when we are ready to require 3.1 or better.
# (3.1 was released late 2014)
find_package (Threads REQUIRED)

find_package (mercury CONFIG REQUIRED)
find_package (deltafs-nexus CONFIG REQUIRED)

add_executable (nx-try nx-try.cc)
target_include_directories (nx-try PUBLIC ${MERCURY_INCLUDE_DIR})
target_link_libraries (nx-try deltafs-nexus Threads::Threads)

#
# "make install" rule
#
install (TARGETS nx-try RUNTIME DESTINATION bin)
