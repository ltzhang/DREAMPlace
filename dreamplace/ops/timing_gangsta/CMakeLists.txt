set(OP_NAME timing_gangsta)

set(TARGET_NAME ${OP_NAME})

# ---- GangSTA external STA engine (open-source; static lib + embedded-Tcl SDC parser) ----
# PROJECT_SOURCE_DIR is the DREAMPlace root, which lives at <gangsta>/placement/DREAMPlace; the gangsta
# repo root is two levels up. `placement` is a symlink that points OUTSIDE the gangsta tree, so resolving
# it would land on the wrong root — collapse ".." textually (ABSOLUTE, no symlink resolution) to stay at
# <gangsta>. Override -DGANGSTA_ROOT=... if the repo lives elsewhere.
get_filename_component(GANGSTA_ROOT_DEFAULT "${PROJECT_SOURCE_DIR}/../.." ABSOLUTE)
set(GANGSTA_ROOT "${GANGSTA_ROOT_DEFAULT}" CACHE PATH "Path to the gangsta repo root (contains include/ and build/src/libgangsta.a)")
find_library(GANGSTA_LIB NAMES gangsta HINTS "${GANGSTA_ROOT}/build/src" "${GANGSTA_ROOT}/build")
find_library(GANGSTA_TCL_LIBRARY NAMES tcl8.6 tcl)  # gangsta's embedded-Tcl SDC parser dependency
if(NOT (GANGSTA_LIB AND EXISTS "${GANGSTA_ROOT}/include/gangsta/gangsta.h" AND GANGSTA_TCL_LIBRARY))
  message(FATAL_ERROR "timing_gangsta: GangSTA not found (lib=${GANGSTA_LIB} tcl=${GANGSTA_TCL_LIBRARY} root=${GANGSTA_ROOT}); build gangsta (cmake --build build) or pass -DGANGSTA_ROOT")
endif()
message(STATUS "timing_gangsta: GangSTA ENABLED (${GANGSTA_LIB}, tcl=${GANGSTA_TCL_LIBRARY})")

set(INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${LIMBO_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
  ${ZLIB_INCLUDE_DIRS}
  ${CUDA_INCLUDE_DIRS}
  ${GANGSTA_ROOT}/include
  )

set(LINK_LIBS utility_cxx
  # begin targets in Limbo for PlaceDB dependencies
  lefparseradapt
  defparseradapt
  verilogparser
  bookshelfparser
  gdsparser
  gzstream
  # end targets in Limbo
  OpenMP::OpenMP_CXX
  programoptions
  ${ZLIB_LIBRARIES}
  ${CUDA_CUDA_LIBRARY}
  stdc++fs
  )

file(GLOB CPP_SOURCES
  src/timing_gs_cpp.cpp
  src/timing_gs_io_cpp.cpp
  src/timing_gs_pybind.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../place_io/src/PlaceDB.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../place_io/src/Node.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../place_io/src/Net.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../place_io/src/Region.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../place_io/src/Group.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../place_io/src/Params.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../place_io/src/Enums.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../place_io/src/BenchMetrics.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../place_io/src/BinMap.cpp
  )

# Add CUDA sources if CUDA is enabled
if(TORCH_ENABLE_CUDA)
    file(GLOB CUDA_SOURCES
        src/timing_cuda_kernel.cu
    )
    set_source_files_properties(${CUDA_SOURCES} PROPERTIES LANGUAGE CUDA)
endif()

if(TORCH_ENABLE_CUDA)
    add_pytorch_extension(${TARGET_NAME}_cpp
        ${CPP_SOURCES} ${CUDA_SOURCES}
        EXTRA_INCLUDE_DIRS ${INCLUDE_DIRS}
        EXTRA_LINK_LIBRARIES ${LINK_LIBS})
    target_compile_definitions(${TARGET_NAME}_cpp PRIVATE CUDA_FOUND)
else()
    add_pytorch_extension(${TARGET_NAME}_cpp
        ${CPP_SOURCES}
        EXTRA_INCLUDE_DIRS ${INCLUDE_DIRS}
        EXTRA_LINK_LIBRARIES ${LINK_LIBS})
endif()


# GangSTA is a PIC static archive (libgangsta.a) plus its embedded-Tcl SDC dependency. Static-link both
# so the extension has no runtime .so dependency on gangsta.
target_link_libraries(${TARGET_NAME}_cpp PRIVATE ${GANGSTA_LIB} ${GANGSTA_TCL_LIBRARY})

set_target_properties(${TARGET_NAME}_cpp PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
    BUILD_RPATH_USE_ORIGIN TRUE)

install(TARGETS
  ${TARGET_NAME}_cpp
  DESTINATION dreamplace/ops/${OP_NAME})

file(GLOB INSTALL_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.py)
install(FILES ${INSTALL_SRCS} DESTINATION dreamplace/ops/${OP_NAME})
