add_subdirectory(utility)
add_subdirectory(place_io)
add_subdirectory(dct)
add_subdirectory(pin_pos)
add_subdirectory(move_boundary)
add_subdirectory(draw_place)
add_subdirectory(density_map)
add_subdirectory(density_overflow)
add_subdirectory(density_potential)
add_subdirectory(electric_potential)
add_subdirectory(hpwl)
# add_subdirectory(rmst_wl)
add_subdirectory(weighted_average_wirelength)
add_subdirectory(logsumexp_wirelength)
add_subdirectory(fence_region)
# legalization operators
add_subdirectory(legality_check)
add_subdirectory(macro_legalize)
add_subdirectory(greedy_legalize)
add_subdirectory(abacus_legalize)
# detailed placement operators
add_subdirectory(global_swap)
add_subdirectory(independent_set_matching)
add_subdirectory(k_reorder)
# routing congestion operator
add_subdirectory(pin_utilization)
add_subdirectory(rudy)
add_subdirectory(pinrudy)
add_subdirectory(nctugr_binary)
add_subdirectory(adjust_node_area)
# timing operators. Gated so the core (wirelength-driven) placement builds without the timing engines,
# and so each engine builds independently (the Timer ops are lazy-imported only when timing_opt_flag is
# set). DREAMPLACE_ENABLE_TIMING drives the upstream pair (OpenTimer `timing` + GPU `timing_heterosta`);
# DREAMPLACE_ENABLE_GANGSTA drives the CPU GangSTA op alone, which hard-requires a built GangSTA (see
# -DGANGSTA_ROOT) but needs NEITHER a GPU nor HeteroSTA's network fetch — so a CPU-only, GPU-free build
# can carry the gangsta inner-loop timer (WiseSyn Milestone-D, ADR-0033). Default: upstream pair ON,
# gangsta OFF (opt-in, since it needs the external GangSTA).
option(DREAMPLACE_ENABLE_TIMING "Build the OpenTimer + HeteroSTA timing ops (timing / timing_heterosta)" ON)
option(DREAMPLACE_ENABLE_GANGSTA "Build the GangSTA CPU timing op (timing_gangsta; needs -DGANGSTA_ROOT)" OFF)
add_subdirectory(pin_weight_sum)
if(DREAMPLACE_ENABLE_TIMING)
  add_subdirectory(timing)
  add_subdirectory(timing_heterosta)
endif()
if(DREAMPLACE_ENABLE_GANGSTA)
  add_subdirectory(timing_gangsta)
endif()
# gift initial placement 
add_subdirectory(gift_init)

file(GLOB INSTALL_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/*.py")
install(
    FILES ${INSTALL_SRCS} DESTINATION dreamplace/ops
    )
