#
# Copyright (c) 2026, Carnegie Mellon University.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 3. Neither the name of the University nor the names of its contributors
#    may be used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
# HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
# OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
# AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
# WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#

#
# CMakeLists.txt  cmake for xthread
# 21-Jul-2026  chuck@ece.cmu.edu
#

#
# list of libxthread source files
#
set (libxthread-src xthread.c)

# helpers for creating packages (e.g. for our library)
include (CMakePackageConfigHelpers)

#
# library version (used to name shared libs).
#
set (LIBXTHREAD_MAJOR 0)
set (LIBXTHREAD_MINOR 1)
set (LIBXTHREAD_PATCH 0)
set (LIBXTHREAD_VERSION
     "${LIBXTHREAD_MAJOR}.${LIBXTHREAD_MINOR}.${LIBXTHREAD_PATCH}")

#
# create library target and configure it
#
add_library(xthread ${libxthread-src})
# includes when building (e.g. for xthread.h)
target_include_directories(xthread BEFORE PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
# includes when target is installed
target_include_directories(xthread PUBLIC $<INSTALL_INTERFACE:include>)
target_link_libraries (xthread MPI::MPI_C Threads::Threads dthread)
set_target_properties(xthread
                      PROPERTIES VERSION ${LIBXTHREAD_VERSION}
                      SOVERSION ${LIBXTHREAD_VERSION})

#
# package generation
#
write_basic_package_version_file(
    xthread-config-version.cmake
    VERSION ${LIBXTHREAD_VERSION}
    COMPATIBILITY AnyNewerVersion)
configure_file (xthread-config.cmake.in		# cfg file for find_package
                xthread-config.cmake @ONLY)

#
# installation
#
install (TARGETS xthread EXPORT xthread-targets		# install libs
         ARCHIVE DESTINATION lib
         LIBRARY DESTINATION lib)
install (EXPORT xthread-targets				# install packaging
         DESTINATION share/cmake/xthread
         FILE "xthread-targets.cmake")
install (FILES "${CMAKE_CURRENT_BINARY_DIR}/xthread-config.cmake"
               "${CMAKE_CURRENT_BINARY_DIR}/xthread-config-version.cmake"
         DESTINATION share/cmake/xthread )
install (FILES ../include/dthread/xthread.h
         DESTINATION include/dthread)
