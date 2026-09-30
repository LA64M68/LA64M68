add_library(la64m68_atari MODULE atari.c)
target_link_libraries(la64m68_atari PRIVATE la64m68_core)
