cmake_minimum_required(VERSION 3.16)
project(smartcross_storage_initializer)

set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${CMAKE_SYSROOT})

FIND_PACKAGE(PkgConfig REQUIRED)
pkg_check_modules(LIBPARTED REQUIRED libparted)

set(CMAKE_CXX_STANDARD 14)

add_executable(smartcross_storage_initializer main.c)
TARGET_LINK_LIBRARIES(smartcross_storage_initializer ${LIBPARTED_LIBRARIES})
