cmake_minimum_required(VERSION 3.18)
set(PROJECT_NAME minima)
Project(${PROJECT_NAME})


function(set_target_output_directory target dir)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "Target '${target}' does not exist.")
  endif()

  # Set output directories for different build configurations
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${dir}"
    ARCHIVE_OUTPUT_DIRECTORY "${dir}"
    LIBRARY_OUTPUT_DIRECTORY "${dir}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${dir}"
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${dir}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${dir}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${dir}"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${dir}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${dir}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${dir}"
    ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${dir}"
    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${dir}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${dir}"
    ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${dir}"
    LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${dir}"
  )
endfunction()


set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND SOURCES 
  src/minima_common.h
  src/minima_common.c
  src/minima.h
  src/minima.c
  src/minima_ast.h
  src/minima_ast.c
  src/minima_eval.c
  src/minima_parser.c
  src/minima_array.h
  src/minima_array.c
  src/main.c
  src/version.rc)

add_executable(${PROJECT_NAME} ${SOURCES})

set(VERSION_MAJOR 1)
set(VERSION_PATCH 0)
set(VERSION_MINOR 0)
set(VERSION_STRING ${VERSION_MAJOR}_${VERSION_MINOR}_${VERSION_PATCH})
set_target_properties(${PROJECT_NAME} PROPERTIES VERSION ${VERSION_MAJOR}.${VERSION_MINOR})
set_target_output_directory(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/bin/${CMAKE_SYSTEM_NAME}_${CMAKE_HOST_SYSTEM_PROCESSOR}_$<CONFIG>)

if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX)
  configure_file(src/version.rc.in src/version.rc)
else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -pedantic -Werror)
endif()

