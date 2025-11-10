cmake_minimum_required(VERSION 3.15)
project(libmbtiles LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/EmbededTemplate.cmake)

add_library(mbtiles STATIC
    src/lib/mbtiles.cpp
    src/lib/mbtiles_viewer.cpp
    src/lib/sqlite3.c
)

target_include_directories(mbtiles
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/includes
        ${CMAKE_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src/lib
)

if(UNIX)
    target_link_libraries(mbtiles PRIVATE dl pthread)
endif()

add_executable(mbtiles-cli src/cli/main.cpp)

target_include_directories(mbtiles-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cli
)

target_link_libraries(mbtiles-cli PRIVATE mbtiles)

install(TARGETS mbtiles-cli DESTINATION bin)
install(TARGETS mbtiles DESTINATION lib)
install(DIRECTORY includes/ DESTINATION include)
