cmake_minimum_required(VERSION 3.18)
project(minima)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(VERSION_MAJOR 1)
set(VERSION_PATCH 0)
set(VERSION_MINOR 0)
set(VERSION_STRING ${VERSION_MAJOR}_${VERSION_MINOR}_${VERSION_PATCH})


function(set_target_output_directory  target dir)
  foreach(conf "" "_DEBUG" "_RELEASE" "_RELWITHDEBINFO" "_MINSIZEREL")
    set_target_properties(${target} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY${conf} "${dir}"
      ARCHIVE_OUTPUT_DIRECTORY${conf} "${dir}"
      LIBRARY_OUTPUT_DIRECTORY${conf} "${dir}"
    )
  endforeach()
endfunction()

# Build dependencies
set(STDX_INCLUDE_DIR                "${CMAKE_CURRENT_LIST_DIR}/src/stdx/src")

list(APPEND SOURCES 
  src/main.c
  src/minima.h
  src/minima.c
  src/minima_runtime.h
  src/minima_runtime.c
  src/main.c)

if(MSVC)
  configure_file(src/version.rc.in src/version.rc)
  list(APPEND SOURCES src/version.rc)
endif()

add_executable(minima ${SOURCES})
target_include_directories(minima
  PUBLIC ${STDX_INCLUDE_DIR})
set_target_properties(minima PROPERTIES
  VERSION ${VERSION_MAJOR}.${VERSION_MINOR})
set_target_output_directory(minima ${CMAKE_CURRENT_LIST_DIR}/bin/${CMAKE_SYSTEM_NAME}_${CMAKE_HOST_SYSTEM_PROCESSOR}_$<CONFIG>)

if(MSVC)
  target_compile_options(minima PRIVATE /W4 /WX)
else()
  target_compile_options(minima PRIVATE -Wall -Wextra -pedantic -Werror)
endif()

