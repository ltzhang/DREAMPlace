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
# timing operators. Gated so the core (wirelength-driven) placement builds without the timing engines:
# timing_gangsta hard-requires a built GangSTA (see -DGANGSTA_ROOT) and timing_heterosta targets the
# HeteroSTA/GPU timer — neither is needed unless timing_opt_flag is set (the Timer ops are lazy-imported
# only then). Default ON preserves upstream behavior; pass -DDREAMPLACE_ENABLE_TIMING=OFF for a
# timing-free build (WiseSyn Milestone-D core-placement path, ADR-0033).
option(DREAMPLACE_ENABLE_TIMING "Build the timing-driven ops (timing / timing_heterosta / timing_gangsta)" ON)
add_subdirectory(pin_weight_sum)
if(DREAMPLACE_ENABLE_TIMING)
  add_subdirectory(timing)
  add_subdirectory(timing_heterosta)
  add_subdirectory(timing_gangsta)
endif()
# gift initial placement 
add_subdirectory(gift_init)

file(GLOB INSTALL_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/*.py")
install(
    FILES ${INSTALL_SRCS} DESTINATION dreamplace/ops
    )
