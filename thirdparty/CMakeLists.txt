#project(thirdparty)

add_subdirectory(flute)

set(OPENBLAS OFF CACHE BOOL "Turn off OPENBLAS for Limbo")
set(INSTALL_LIMBO OFF CACHE BOOL "Turn off installation for Limbo")
set(GENERATE_DOCS OFF CACHE BOOL "Turn off documentation for Limbo")
set(ENABLE_TEST OFF CACHE BOOL "Turn off test for Limbo")
add_subdirectory(Limbo)
add_subdirectory(munkres-cpp)
# Timing engines (OpenTimer = CPU timer; HeteroSTA = GPU timer, downloaded at configure time). Gated so
# a timing-free build (DREAMPLACE_ENABLE_TIMING=OFF) needs neither — and skips HeteroSTA's network
# fetch. See the option in the top-level CMakeLists.txt (WiseSyn Milestone-D; ADR-0033).
if(DREAMPLACE_ENABLE_TIMING)
  add_subdirectory(OpenTimer)
  add_subdirectory(HeteroSTA)
endif()

install(
    DIRECTORY NCTUgr.ICCAD2012 DESTINATION thirdparty
    )

