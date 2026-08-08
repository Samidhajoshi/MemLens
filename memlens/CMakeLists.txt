cmake_minimum_required(VERSION 3.10)
project(MemLens CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# The instrumenter CLI: `memlens path/to/file.cpp` instruments,
# compiles, and runs it, producing memory_report.json.
add_executable(memlens src/memlens.cpp)
target_link_libraries(memlens PRIVATE Threads::Threads)
target_compile_options(memlens PRIVATE -Wall -Wextra)

# src/memlens.h is a header-only library consumed by whatever program
# gets instrumented (it's copied alongside the instrumented .cpp at
# runtime, not linked into this target).

install(TARGETS memlens RUNTIME DESTINATION bin)
install(FILES src/memlens.h DESTINATION include)
install(FILES dashboard.html DESTINATION share/memlens)
