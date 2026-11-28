cmake_minimum_required(VERSION 3.16)
project(mb_cmochicode C)
# ENGINE_INC: directory holding mbjs.h + mbjs_runtime.h (no absolute host paths).
set(ENGINE_INC "${ENGINE_INC}" CACHE PATH "engine native headers dir")
if(NOT ENGINE_INC)
  set(ENGINE_INC "${CMAKE_CURRENT_SOURCE_DIR}/../../libmochiberry/native")
endif()
add_library(mb_cmochicode SHARED src/cmochicode.c)
target_include_directories(mb_cmochicode PRIVATE "${ENGINE_INC}" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../../ccompiler" "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime")
if(WIN32 AND NOT "${ENGINE_INC}" STREQUAL "")
  target_link_libraries(mb_cmochicode PRIVATE )
endif()
set_target_properties(mb_cmochicode PROPERTIES PREFIX "" OUTPUT_NAME "cmochicode_x86_64"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib")
