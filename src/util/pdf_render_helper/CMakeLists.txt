## xopp-render-helper — standalone build
##
## Small sandbox-friendly binary that renders one page of a PDF to PNG.
## Designed to be spawned by Xournal++ inside firejail.
##
## This CMakeLists is fully self-contained. It does not depend on the
## Xournal++ source tree. The render helper's link closure is intentionally
## narrow: poppler-glib, cairo, glib.

cmake_minimum_required(VERSION 3.18)

project(xopp-render-helper
        VERSION 0.1
        DESCRIPTION "Sandboxed PDF render helper for Xournal++"
        LANGUAGES CXX)

if(WIN32)
    message(FATAL_ERROR "xopp-render-helper is Linux-only (firejail dependency).")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(PkgConfig REQUIRED)

pkg_check_modules(HELPER_DEPS REQUIRED IMPORTED_TARGET
    poppler-glib
    cairo
    glib-2.0
    gobject-2.0
)

add_executable(xopp-render-helper main.cpp)

target_compile_features(xopp-render-helper PRIVATE cxx_std_20)

target_compile_options(xopp-render-helper PRIVATE
    -Wall -Wextra -Wpedantic
)

target_link_libraries(xopp-render-helper
    PRIVATE PkgConfig::HELPER_DEPS
)

include(GNUInstallDirs)

install(TARGETS xopp-render-helper
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES render_helper.profile
    DESTINATION ${CMAKE_INSTALL_DATADIR}/xopp-render-helper
)
