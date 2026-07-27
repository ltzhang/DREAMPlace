/**
 * @file   row_grid.h
 * @brief  The set of placement rows a legalizer may put a cell in.
 *
 * UNIFORM mode -- one row height across the whole core, which is the overwhelming majority of
 * floorplans -- keeps the classic closed-form arithmetic. Every accessor below reproduces LITERALLY
 * the expression it replaced at each call site (`floorDiv`, plain truncation, `ceilDiv`, ...), so a
 * single-height design legalizes exactly as it did before this grid existed.
 *
 * MIXED mode carries an explicit per-row (bottom edge, height) table. That is what a core
 * interleaving two standard-cell row heights needs: a cell may only occupy a run of rows whose
 * heights sum EXACTLY to its own height, so a 9-track cell can never be handed a 7-track row. A
 * cell that matches no row at all is reported as such (`rows_spanned` < 0) and the caller must fail
 * loudly rather than drop it somewhere plausible.
 *
 * The table is NOT owned here: it is a pair of raw pointers into caller-owned storage (a torch
 * tensor), so the grid stays trivially copyable into the by-value legalization databases.
 */
#ifndef _DREAMPLACE_UTILITY_ROW_GRID_H
#define _DREAMPLACE_UTILITY_ROW_GRID_H

#include <algorithm>
#include <cmath>

// Only the small numeric helpers -- utils.h pulls in legalization_db.h, which includes this header.
#include "utility/src/defs.h"
#include "utility/src/math.h"
#include "utility/src/namespace.h"

DREAMPLACE_BEGIN_NAMESPACE

template <typename T>
struct RowGrid {
  const T* row_yl = nullptr;  ///< bottom edge of each row, ascending; null in uniform mode
  const T* row_h = nullptr;   ///< height of each row; null in uniform mode
  int num_table_rows = 0;     ///< table length; 0 in uniform mode

  T yl = 0;           ///< core bottom edge (uniform mode only)
  T yh = 0;           ///< core top edge (uniform mode only)
  T height = 1;       ///< uniform row height, or the MINIMUM row height of the table
  T max_height = 1;   ///< maximum row height of the table

  /// @brief True when one scalar row height describes the whole core.
  bool uniform() const { return num_table_rows == 0; }

  /// @brief Row count as `floorDiv(yh - yl, row_height)` in uniform mode -- the blank-bin count the
  /// greedy legalizer has always used.
  int num_rows() const {
    return uniform() ? (int)floorDiv(yh - yl, height) : num_table_rows;
  }
  /// @brief Row count as `ceilDiv(yh - yl, row_height)` in uniform mode -- the bin count the abacus
  /// legalizer and the legality check have always used (they include a trailing partial row).
  int num_rows_incl_partial() const {
    return uniform() ? (int)ceilDiv(yh - yl, height) : num_table_rows;
  }

  T row_bottom(int r) const {
    return uniform() ? yl + r * height : row_yl[r];
  }
  T row_height(int r) const {
    return uniform() ? height : row_h[r];
  }

  /// @brief Index of the row containing `y`. Uniform mode is exactly `floorDiv(y - yl, row_height)`
  /// with the same tolerance the call site used; NOT clamped (callers clamp as they did before).
  int row_index_floor(T y, T rtol = NumericTolerance<T>::rtol) const {
    if (uniform()) return (int)floorDiv(y - yl, height, rtol);
    return search_floor(y, rtol * height);
  }
  /// @brief Index one past the last row strictly below `y`: uniform mode is
  /// `ceilDiv(y - yl, row_height)`.
  int row_index_ceil(T y, T rtol = NumericTolerance<T>::rtol) const {
    if (uniform()) return (int)ceilDiv(y - yl, height, rtol);
    const T tol = rtol * height;
    int lo = 0, hi = num_table_rows;
    while (lo < hi) {  // first row whose bottom edge is >= y (within tolerance)
      int mid = (lo + hi) / 2;
      if (row_yl[mid] + tol < y) lo = mid + 1; else hi = mid;
    }
    return lo;
  }

  /// @brief How many rows a cell of height `h` occupies when its bottom sits on row `r`.
  /// Uniform mode: `ceilDiv(h, row_height)` -- unchanged, and never negative.
  /// Mixed mode: the length of the contiguous run starting at `r` whose heights sum to exactly `h`,
  /// or -1 when no such run exists (the cell does not fit that row).
  int rows_spanned(int r, T h) const {
    if (uniform()) return (int)ceilDiv(h, height);
    if (r < 0 || r >= num_table_rows) return -1;
    const T tol = (T)1e-3 * height;
    T sum = 0;
    for (int i = r; i < num_table_rows; ++i) {
      if (i > r && std::abs(row_yl[i] - (row_yl[i - 1] + row_h[i - 1])) > tol) break;  // gap
      sum += row_h[i];
      if (std::abs(sum - h) <= tol) return i - r + 1;
      if (sum > h + tol) break;
    }
    return -1;
  }

  /// @brief Upper bound on rows_spanned() over all rows, for sizing per-cell scratch before the
  /// candidate-row search. Uniform mode is `ceilDiv(h, row_height)`.
  int max_rows_spanned(T h) const {
    int n = (int)ceilDiv(h, height);
    return n < 1 ? 1 : n;
  }

  /// @brief True when a cell of height `h` whose bottom is at `y` sits exactly on a row, or on a
  /// contiguous run of rows that tiles it exactly.
  bool aligned(T y, T h) const {
    int r = row_index_floor(y, (T)1e-3);
    if (r < 0 || r >= num_rows_incl_partial()) return false;
    if (std::abs(row_bottom(r) - y) > (T)1e-3 * height) return false;
    return rows_spanned(r, h) > 0;
  }

  /// @brief Snap `y` down to the bottom edge of the row that contains it, clamped into the core.
  T align_to_row(T y, T h) const {
    if (uniform()) {
      T yy = std::max(std::min(y, yh - h), yl);
      return floorDiv(yy - yl, height) * height + yl;
    }
    int r = row_index_floor(y, (T)1e-3);
    r = std::max(0, std::min(r, num_table_rows - 1));
    return row_yl[r];
  }

 private:
  /// @brief Last row whose bottom edge is <= y (+tol); -1 when y is below the first row.
  int search_floor(T y, T tol) const {
    int lo = 0, hi = num_table_rows;
    while (lo < hi) {
      int mid = (lo + hi) / 2;
      if (row_yl[mid] <= y + tol) lo = mid + 1; else hi = mid;
    }
    return lo - 1;
  }
};

/// @brief Build a uniform grid: rows at yl + k * row_height. Identical to the pre-existing model.
template <typename T>
RowGrid<T> make_uniform_row_grid(T yl, T yh, T row_height) {
  RowGrid<T> g;
  g.yl = yl;
  g.yh = yh;
  g.height = row_height;
  g.max_height = row_height;
  return g;
}

/// @brief Build a grid from an explicit table. A table that turns out uniform DEGRADES to the
/// uniform grid, so a single-height core can never take a different code path by accident.
template <typename T>
RowGrid<T> make_row_grid(T yl, T yh, T row_height, const T* row_yl, const T* row_h, int num_rows) {
  if (row_yl == nullptr || row_h == nullptr || num_rows <= 0) {
    return make_uniform_row_grid(yl, yh, row_height);
  }
  T lo = row_h[0], hi = row_h[0];
  for (int i = 1; i < num_rows; ++i) {
    lo = std::min(lo, row_h[i]);
    hi = std::max(hi, row_h[i]);
  }
  if (!(hi - lo > (T)1e-3 * lo)) return make_uniform_row_grid(yl, yh, row_height);
  RowGrid<T> g;
  g.row_yl = row_yl;
  g.row_h = row_h;
  g.num_table_rows = num_rows;
  g.yl = yl;
  g.yh = yh;
  g.height = lo;
  g.max_height = hi;
  return g;
}

DREAMPLACE_END_NAMESPACE

#endif
