cmake_minimum_required(VERSION 3.10)

# 1. Define the shared library target using your C source file
add_library(xthread SHARED xthread.c)

# 2. Provide access to the local include directories (so it finds dthread.h)
target_include_directories(xthread PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 3. Link against the main dthread library and its standard dependencies
target_link_libraries(xthread PRIVATE 
    dthread 
    Threads::Threads 
    MPI::MPI_C
)

# 4. Enforce RPATH properties so the companion library finds libdthread at runtime
set_target_properties(xthread PROPERTIES
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

# 5. Define installation rules for the binary library and the public header
include(GNUInstallDirs)

install(TARGETS xthread
    EXPORT xthread-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES xthread.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dthread
)
