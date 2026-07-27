/**
 * @file   greedy_legalize.cpp
 * @author Yibo Lin
 * @date   Jun 2018
 */
#include "greedy_legalize/src/function_cpu.h"

DREAMPLACE_BEGIN_NAMESPACE

/// @brief legalize layout with greedy legalization.
/// Only movable nodes will be moved. Fixed nodes and filler nodes are fixed.
///
/// @param init_x initial x location of nodes, including movable nodes, fixed
/// nodes, and filler nodes, [0, num_movable_nodes) are movable nodes,
/// [num_movable_nodes, num_nodes-num_filler_nodes) are fixed nodes,
/// [num_nodes-num_filler_nodes, num_nodes) are filler nodes
/// @param init_y initial y location of nodes, including movable nodes, fixed
/// nodes, and filler nodes, same as init_x
/// @param node_size_x width of nodes, including movable nodes, fixed nodes, and
/// filler nodes, [0, num_movable_nodes) are movable nodes, [num_movable_nodes,
/// num_nodes-num_filler_nodes) are fixed nodes, [num_nodes-num_filler_nodes,
/// num_nodes) are filler nodes
/// @param node_size_y height of nodes, including movable nodes, fixed nodes,
/// and filler nodes, same as node_size_x
/// @param xl left edge of bounding box of layout area
/// @param yl bottom edge of bounding box of layout area
/// @param xh right edge of bounding box of layout area
/// @param yh top edge of bounding box of layout area
/// @param site_width width of a placement site
/// @param row_height height of a placement row
/// @param num_bins_x number of bins in horizontal direction
/// @param num_bins_y number of bins in vertical direction
/// @param num_nodes total number of nodes, including movable nodes, fixed
/// nodes, and filler nodes; fixed nodes are in the range of [num_movable_nodes,
/// num_nodes-num_filler_nodes)
/// @param num_movable_nodes number of movable nodes, movable nodes are in the
/// range of [0, num_movable_nodes)
/// @param number of filler nodes, filler nodes are in the range of
/// [num_nodes-num_filler_nodes, num_nodes)
template <typename T>
int greedyLegalizationLauncher(LegalizationDB<T> db) {
  return greedyLegalizationCPU(db, db.init_x, db.init_y, db.node_size_x,
                               db.node_size_y, db.x, db.y, db.xl, db.yl, db.xh,
                               db.yh, db.site_width, db.row_height,
                               db.num_bins_x, db.num_bins_y, db.num_nodes,
                               db.num_movable_nodes);
}

/// @brief legalize layout with greedy legalization.
/// Only movable nodes will be moved. Fixed nodes and filler nodes are fixed.
///
/// @param init_pos initial locations of nodes, including movable nodes, fixed
/// nodes, and filler nodes, [0, num_movable_nodes) are movable nodes,
/// [num_movable_nodes, num_nodes-num_filler_nodes) are fixed nodes,
/// [num_nodes-num_filler_nodes, num_nodes) are filler nodes
/// @param node_size_x width of nodes, including movable nodes, fixed nodes, and
/// filler nodes, [0, num_movable_nodes) are movable nodes, [num_movable_nodes,
/// num_nodes-num_filler_nodes) are fixed nodes, [num_nodes-num_filler_nodes,
/// num_nodes) are filler nodes
/// @param node_size_y height of nodes, including movable nodes, fixed nodes,
/// and filler nodes, same as node_size_x
/// @param node_weights weight of nodes in computing displacement
/// @param xl left edge of bounding box of layout area
/// @param yl bottom edge of bounding box of layout area
/// @param xh right edge of bounding box of layout area
/// @param yh top edge of bounding box of layout area
/// @param site_width width of a placement site
/// @param row_height height of a placement row
/// @param num_bins_x number of bins in horizontal direction
/// @param num_bins_y number of bins in vertical direction
/// @param num_nodes total number of nodes, including movable nodes, fixed
/// nodes, and filler nodes; fixed nodes are in the range of [num_movable_nodes,
/// num_nodes-num_filler_nodes)
/// @param num_movable_nodes number of movable nodes, movable nodes are in the
/// range of [0, num_movable_nodes)
/// @param number of filler nodes, filler nodes are in the range of
/// [num_nodes-num_filler_nodes, num_nodes)
at::Tensor greedy_legalization_forward(
    at::Tensor init_pos, at::Tensor pos, at::Tensor node_size_x,
    at::Tensor node_size_y, at::Tensor node_weights,
    at::Tensor flat_region_boxes, at::Tensor flat_region_boxes_start,
    at::Tensor node2fence_region_map, double xl, double yl, double xh,
    double yh, double site_width, double row_height, int num_bins_x,
    int num_bins_y, int num_movable_nodes, int num_terminal_NIs,
    int num_filler_nodes, at::Tensor row_yl, at::Tensor row_h) {
  CHECK_FLAT_CPU(init_pos);
  CHECK_EVEN(init_pos);
  CHECK_CONTIGUOUS(init_pos);

  auto pos_copy = pos.clone();
  int unfittable = 0;

  CPUTimer::hr_clock_rep timer_start, timer_stop;
  timer_start = CPUTimer::getGlobaltime();
  // Call the cuda kernel launcher
  DREAMPLACE_DISPATCH_FLOATING_TYPES(
      pos, "greedyLegalizationLauncher", [&] {
        auto db = make_placedb<scalar_t>(
            init_pos, pos_copy, node_size_x, node_size_y, node_weights,
            flat_region_boxes, flat_region_boxes_start, node2fence_region_map,
            xl, yl, xh, yh, site_width, row_height, num_bins_x, num_bins_y,
            num_movable_nodes, num_terminal_NIs, num_filler_nodes, row_yl,
            row_h);
        unfittable = greedyLegalizationLauncher<scalar_t>(db);
        // db.check_legality();
      });
  timer_stop = CPUTimer::getGlobaltime();
  dreamplacePrint(kINFO, "Greedy legalization takes %g ms\n",
                  (timer_stop - timer_start) * CPUTimer::getTimerPeriod());

  // A cell that fits no row of this core cannot be legalized. Reject loudly rather than hand back a
  // placement that merely looks plausible: the caller must see this as a failure, not as a result.
  // A raised error (not an abort) lets the embedding flow report it and fall back deliberately.
  TORCH_CHECK(unfittable == 0, "greedy legalization: ", unfittable,
              " movable cell(s) have a height that fits no placement row of this core");

  return pos_copy;
}

DREAMPLACE_END_NAMESPACE

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("forward", &DREAMPLACE_NAMESPACE::greedy_legalization_forward,
        "Greedy legalization forward");
}
