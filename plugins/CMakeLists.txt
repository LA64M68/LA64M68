add_subdirectory(amiga)
add_subdirectory(atari)

# Plugin modules live in ./plugins inside the program root, next to the
# executable -- same layout as an unpacked release .tar.xz.
set_target_properties(la64m68_amiga la64m68_atari PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/plugins)
