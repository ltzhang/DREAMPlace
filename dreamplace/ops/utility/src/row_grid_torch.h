/**
 * @file   row_grid_torch.h
 * @brief  Build a RowGrid from the optional per-row tensors an op receives.
 *
 * The row table travels from Python as a pair of 1-D tensors. EMPTY tensors mean "single-height
 * core" -- the overwhelming majority -- and produce the uniform grid, which reproduces the original
 * closed-form arithmetic exactly. A table that turns out uniform anyway also degrades to it, so a
 * single-height design can never take a different code path by accident.
 */
#ifndef _DREAMPLACE_UTILITY_ROW_GRID_TORCH_H
#define _DREAMPLACE_UTILITY_ROW_GRID_TORCH_H

#include "utility/src/row_grid.h"
#include "utility/src/torch.h"

DREAMPLACE_BEGIN_NAMESPACE

template <typename T>
RowGrid<T> make_row_grid_from_tensors(double yl, double yh, double row_height,
                                      at::Tensor row_yl, at::Tensor row_h) {
  const int n = (int)row_yl.numel();
  if (n <= 0 || row_h.numel() != row_yl.numel()) {
    return make_uniform_row_grid((T)yl, (T)yh, (T)row_height);
  }
  return make_row_grid((T)yl, (T)yh, (T)row_height,
                       DREAMPLACE_TENSOR_DATA_PTR(row_yl, T),
                       DREAMPLACE_TENSOR_DATA_PTR(row_h, T), n);
}

DREAMPLACE_END_NAMESPACE

#endif
