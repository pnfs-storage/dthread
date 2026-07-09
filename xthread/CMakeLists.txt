cmake_minimum_required(VERSION 3.10)

add_library(xthread SHARED xthread.c)

target_compile_options(xthread PRIVATE -g -O0 -Wall)

target_include_directories(xthread PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(xthread PRIVATE
    dthread
    Threads::Threads
    MPI::MPI_C
)

set_target_properties(xthread PROPERTIES
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

include(GNUInstallDirs)

install(TARGETS xthread
    EXPORT xthread-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES xthread.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dthread
)
