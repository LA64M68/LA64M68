add_library(la64m68_core STATIC
    cpu.c
    opts.c
    memory.c
    debug.c
    vmmu.c
    vhid.c
    vnic.c
    vrtg.c
    vfpu.c
    vaga.c
    vblk.c
    vblk_qcow2.c
    vfs.c
    vcia.c
    vsplash.c
)

target_include_directories(la64m68_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(la64m68_core PUBLIC m)
