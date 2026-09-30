add_subdirectory(core)
add_subdirectory(host)

add_executable(la64m68 main.c)

# LA64M68 is a self-contained solution directory: the directory that holds the
# executable is the program root at runtime (src/host/host.c selfroot). Emit
# the binary there so a built tree and an unpacked release .tar.xz behave
# identically -- nothing is installed into POSIX/Linux/BSD hierarchies.
set_target_properties(la64m68 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

target_include_directories(la64m68 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/host
)
target_link_libraries(la64m68 PRIVATE la64m68_core la64m68_host m)
