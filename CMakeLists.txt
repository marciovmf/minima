cmake_minimum_required(VERSION 3.18)
project(minima)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(VERSION_MAJOR 1)
set(VERSION_PATCH 0)
set(VERSION_MINOR 0)
set(VERSION_STRING ${VERSION_MAJOR}_${VERSION_MINOR}_${VERSION_PATCH})

if(MSVC)
  add_compile_options(/W4 /WX)
else()
  add_compile_options(-Wall -Wextra -pedantic -Werror)
endif()

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
set(STDX_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/src/include")
set(OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/bin/${CMAKE_SYSTEM_NAME}_${CMAKE_HOST_SYSTEM_PROCESSOR}_$<CONFIG>)

list(APPEND SOURCES 
  ${CMAKE_CURRENT_LIST_DIR}/src/main.c
  ${CMAKE_CURRENT_LIST_DIR}/src/minima.c
  ${CMAKE_CURRENT_LIST_DIR}/src/minima.h
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_version.h
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_compile.c
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_compile.h
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_typecheck.c
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_typecheck.h
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_fold.c
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_fold.h
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_heap.c
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_heap.h
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_mx.c
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_mx.h
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_parse.c
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_parse.h
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_runtime.c
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_runtime.h
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_vm.c
  ${CMAKE_CURRENT_LIST_DIR}/src/mi_vm.h
)

if(MSVC)
  configure_file(src/version.rc.in ${CMAKE_CURRENT_LIST_DIR}/src/version.rc)
  list(APPEND SOURCES src/version.rc)
endif()

# Minima VM
add_executable(minima ${CMAKE_CURRENT_LIST_DIR}/src/main.c ${SOURCES})
target_include_directories(minima PUBLIC ${STDX_INCLUDE_DIR})
set_target_properties(minima PROPERTIES
  VERSION ${VERSION_MAJOR}.${VERSION_MINOR})
set_target_output_directory(minima ${OUTPUT_DIR})


# Modules

list(APPEND MODULE_CORE_SRC
  ${CMAKE_CURRENT_LIST_DIR}/src/module/core/mi_core.c
  ${CMAKE_CURRENT_LIST_DIR}/src/module/core/mi_core_int.h
  ${CMAKE_CURRENT_LIST_DIR}/src/module/core/mi_core_int.c
  ${CMAKE_CURRENT_LIST_DIR}/src/module/core/mi_core_float.h
  ${CMAKE_CURRENT_LIST_DIR}/src/module/core/mi_core_float.c
)

add_library(module_core SHARED ${MODULE_CORE_SRC})
target_include_directories(module_core PUBLIC
  ${STDX_INCLUDE_DIR}
  "${CMAKE_CURRENT_LIST_DIR}/src")

set_target_output_directory(module_core ${OUTPUT_DIR}/module)
