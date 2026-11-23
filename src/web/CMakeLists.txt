cmake_minimum_required(VERSION 3.24)
project(BreezeCadWeb VERSION 0.2.6 LANGUAGES C CXX)

# The preview links the same document, sketch, assembly and kernel libraries
# as the desktop executable, without depending on its windowing backend.
get_filename_component(PROJECT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(EMSCRIPTEN)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fexceptions>)
    set(THREADS_PREFER_PTHREAD_FLAG OFF)
    # Geometry runs inside a browser worker. No shared-memory threads are used.
    add_library(Threads::Threads INTERFACE IMPORTED)
else()
    find_package(Threads REQUIRED)
endif()

include(CompilerWarnings)
include(ThirdParty)
include(OpenCascade)
foreach(layer IN ITEMS core sketch sheetmetal document assembly kernel)
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/${layer}" "${layer}")
endforeach()

add_executable(breezecad-web Preview.cpp)
target_link_libraries(breezecad-web PRIVATE bz::kernel)
bz_set_warnings(breezecad-web)
if(EMSCRIPTEN)
    set_target_properties(breezecad-web PROPERTIES SUFFIX ".mjs")
    target_link_options(breezecad-web PRIVATE
        --no-entry -fexceptions
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=worker,node
        -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=1073741824 -sSTACK_SIZE=2097152
        -sFORCE_FILESYSTEM=1 -sEXPORTED_FUNCTIONS=_bz_web_request
        -sEXPORTED_RUNTIME_METHODS=FS,ccall)
endif()
