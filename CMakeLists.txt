cmake_minimum_required(VERSION 3.10)
project(microcal C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find the ncurses library
find_package(Curses REQUIRED)
include_directories(${CURSES_INCLUDE_DIR})

# Source files located in the src directory
set(SOURCES
    src/main.c
    src/calendar.c
    src/io.c
    src/ui.c
    src/zone.c
)

# Executable target
add_executable(microcal ${SOURCES})

# Link ncurses
target_link_libraries(microcal PRIVATE ${CURSES_LIBRARIES})

enable_testing()

add_executable(zone_test tests/zone_test.c src/zone.c)
target_include_directories(zone_test PRIVATE src)
add_test(NAME zone_test COMMAND zone_test)