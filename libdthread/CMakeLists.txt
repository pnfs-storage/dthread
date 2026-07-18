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
# CMakeLists.txt  cmake for dthread
# 14-Apr-2026  chuck@ece.cmu.edu
#

#
# list of libdthread source files
#
set (libdthread-src dt_init.c dt_mgrthread.c dt_mpithread.c dt_ops.c
     dt_queuing.c dt_segops.c dt_shmops.c dt_syncops.c dt_threadops.c)
if(DTHREAD_SYNC_PSHARED)
    list(APPEND libdthread-src dt_syncops_pshared.c)
endif()

#
# create dthread_config.h header file based on current cmake config
#
configure_file ("../include/dthread/dthread_config.h.in"
                "../include/dthread/dthread_config.h" @ONLY)

if(DTHREAD_MLOG)
    #
    # add mlog to srcs and generate header with mkmloghdr.sh script
    #
    list(APPEND libdthread-src mlog.c dt_mlog.h)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/dt_mlog.h
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/mkmloghdr.sh >
                                         ${CMAKE_CURRENT_BINARY_DIR}/dt_mlog.h
        DEPENDS mkmloghdr.sh)
endif(DTHREAD_MLOG)

# helpers for creating packages (e.g. for our library)
include (CMakePackageConfigHelpers)

#
# library version (used to name shared libs).
#
set (LIBDTHREAD_MAJOR 0)
set (LIBDTHREAD_MINOR 1)
set (LIBDTHREAD_PATCH 0)
set (LIBDTHREAD_VERSION
     "${LIBDTHREAD_MAJOR}.${LIBDTHREAD_MINOR}.${LIBDTHREAD_PATCH}")

#
# create library target and configure it
#
add_library(dthread ${libdthread-src})
# includes when building (e.g. for dthread.h, dthread_config.h, dt_mlog.h)
target_include_directories(dthread BEFORE PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/../include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)
# includes when target is installed
target_include_directories(dthread PUBLIC $<INSTALL_INTERFACE:include>)
target_link_libraries (dthread MPI::MPI_C Threads::Threads)
set_target_properties(dthread
                      PROPERTIES VERSION ${LIBDTHREAD_VERSION}
                      SOVERSION ${LIBDTHREAD_VERSION})
if(NOT DTHREAD_MLOG)
    # to be tidy clean dt_mlog.h even if mlog is disabled
    set_property(TARGET dthread APPEND PROPERTY
        ADDITIONAL_CLEAN_FILES ${CMAKE_CURRENT_BINARY_DIR}/dt_mlog.h)
endif()

#
# package generation
#
write_basic_package_version_file(
    dthread-config-version.cmake
    VERSION ${LIBDTHREAD_VERSION}
    COMPATIBILITY AnyNewerVersion)
configure_file (dthread-config.cmake.in		# cfg file for find_package
                dthread-config.cmake @ONLY)

#
# installation
#
install (TARGETS dthread EXPORT dthread-targets		# install libs
         ARCHIVE DESTINATION lib
         LIBRARY DESTINATION lib)
install (EXPORT dthread-targets				# install packaging
         DESTINATION share/cmake/dthread
         FILE "dthread-targets.cmake")
install (FILES "${CMAKE_CURRENT_BINARY_DIR}/dthread-config.cmake"
               "${CMAKE_CURRENT_BINARY_DIR}/dthread-config-version.cmake"
         DESTINATION share/cmake/dthread )
install (FILES ../include/dthread/dthread.h
               ${CMAKE_CURRENT_BINARY_DIR}/../include/dthread/dthread_config.h
         DESTINATION include/dthread)
if (DTHREAD_MLOG_EXPORT)
    install (FILES mlog.h ${CMAKE_CURRENT_BINARY_DIR}/dt_mlog.h
           DESTINATION include/dthread)
endif()
