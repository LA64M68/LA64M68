add_library(la64m68_host STATIC
    host.c
    vm.c
    fb.c
    fb_fbdev.c
    fb_blit.c
    input.c
    net.c
    pis.c
    plugin_dl.c
)

find_package(Threads REQUIRED)
target_link_libraries(la64m68_host PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# VideoCore direct backend (DRM/KMS dumb buffer on RPi4B+)
find_package(PkgConfig)
if (PkgConfig_FOUND)
    pkg_check_modules(DRM libdrm)
endif()
if (DRM_FOUND)
    target_sources(la64m68_host PRIVATE fb_vc.c)
    target_compile_definitions(la64m68_host PRIVATE LA64M68_HAVE_VC)
    target_include_directories(la64m68_host PRIVATE ${DRM_INCLUDE_DIRS})
    target_link_libraries(la64m68_host PRIVATE ${DRM_LIBRARIES})
endif()

# X11 presenter (XOrg window fallback; no Wayland per display policy)
find_package(X11)
if (X11_FOUND)
    target_sources(la64m68_host PRIVATE fb_x11.c)
    target_compile_definitions(la64m68_host PRIVATE LA64M68_HAVE_X11)
    target_include_directories(la64m68_host PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(la64m68_host PRIVATE ${X11_LIBRARIES})
endif()

# vm backend lives in vm.c: userfaultfd on Linux (runtime-detected),
# POSIX/SIGSEGV elsewhere and as fallback (BSD tier-2 path).

target_include_directories(la64m68_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(la64m68_host PRIVATE la64m68_core)
