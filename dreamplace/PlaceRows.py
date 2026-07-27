##
# @file   PlaceRows.py
# @brief  Physical placement-row table for a core that interleaves row heights.
#
# The classic floorplan model is a single uniform grid: one site width, one row height, rows at
# yl + k * row_height. That model cannot describe a core whose standard-cell rows come in TWO
# heights -- a PDK mixing 7-track and 9-track cells writes the DEF as alternating ROW records of two
# different SITEs. Collapsing such a core onto one height silently misplaces every taller cell: it
# lands on a y that is not the lower edge of any real row, so its power rails do not line up and the
# legalizer's own bookkeeping disagrees with the physical rows.
#
# This module derives the real row table (one entry per physical row, each with its OWN height) from
# the raw DEF ROW rectangles, and validates that those rectangles describe a consistent tiling. It
# is deliberately dependency-free (no torch, no PlaceDB) so the derivation is unit-testable alone.
#

UNIFORM = "uniform"    # every row has the same height -- the classic uniform grid; nothing changes
MIXED = "mixed"        # two or more distinct row heights, and the table validated
REJECTED = "rejected"  # the ROW records do not describe a consistent tiling; the table is unusable


class PlaceRowTable(object):
    """One entry per physical row: `row_yl[i]` is the bottom edge, `row_h[i]` the height.

    `status` is UNIFORM / MIXED / REJECTED; `message` says why a table was rejected.
    """

    def __init__(self, row_yl=None, row_h=None, status=UNIFORM, message=""):
        self.row_yl = list(row_yl) if row_yl else []
        self.row_h = list(row_h) if row_h else []
        self.status = status
        self.message = message

    @property
    def mixed(self):
        return self.status == MIXED

    @property
    def rejected(self):
        return self.status == REJECTED

    def __len__(self):
        return len(self.row_yl)

    def __repr__(self):
        return "PlaceRowTable(%d rows, status=%s%s)" % (
            len(self.row_yl), self.status, (", " + self.message) if self.message else "")


def _shortest_per_lower_edge(records, tol):
    """A DEF may describe the same physical strip more than once.

    A "hybrid" SITE -- one whose LEF definition carries a ROWPATTERN naming two shorter sites -- is
    commonly written out BOTH as the tall pattern row and as the shorter rows it expands into, all
    sharing a lower edge. Keeping the SHORTEST record at each bottom edge therefore recovers the
    finest-grained (i.e. the real) placement rows; the coarser duplicates are re-checked by the
    caller as exact multi-row spans.
    """
    by_y = {}
    for (y, h) in records:
        key = round(float(y) / tol) if tol > 0 else float(y)
        prev = by_y.get(key)
        if prev is None or h < prev[1]:
            by_y[key] = (float(y), float(h))
    return sorted(by_y.values(), key=lambda r: r[0])


def _span_rows(row_yl, row_h, first, height, tol):
    """Number of table rows, starting at `first`, whose heights sum to exactly `height`.

    Returns 0 when no such contiguous run exists (the caller treats that as "does not fit").
    """
    total = 0.0
    for i in range(first, len(row_yl)):
        if i > first and abs(row_yl[i] - (row_yl[i - 1] + row_h[i - 1])) > tol:
            break  # gap: not contiguous
        total += row_h[i]
        if abs(total - height) <= tol:
            return i - first + 1
        if total > height + tol:
            break
    return 0


def derive_placement_rows(records, tol=None):
    """Derive the placement-row table from raw (bottom_edge, height) ROW records.

    The table is REJECTED whenever the records are not a consistent tiling: a non-positive height,
    rows that overlap in y, or an original record that is not exactly covered by a contiguous run of
    table rows. Rejecting is the point -- a half-understood row structure must never be turned into
    a placement.
    """
    records = [(float(y), float(h)) for (y, h) in records]
    table = PlaceRowTable()
    if not records:
        return table  # UNIFORM + empty: the caller keeps its existing model

    for (y, h) in records:
        if h <= 0:
            return PlaceRowTable(status=REJECTED,
                                 message="row at y=%g has non-positive height %g" % (y, h))

    if tol is None:
        # Relative to the shortest record, so the tolerance means the same thing whether the caller
        # works in DBU or in the placer's site-width-scaled units.
        tol = 1e-3 * min(h for (_, h) in records)

    rows = _shortest_per_lower_edge(records, tol)
    row_yl = [r[0] for r in rows]
    row_h = [r[1] for r in rows]

    # Rows may leave gaps (a carved-out macro region is legal) but must never overlap.
    for i in range(1, len(row_yl)):
        if row_yl[i - 1] + row_h[i - 1] > row_yl[i] + tol:
            return PlaceRowTable(
                status=REJECTED,
                message="row at y=%g height %g overlaps the row at y=%g"
                        % (row_yl[i - 1], row_h[i - 1], row_yl[i]))

    # Every original record must be exactly reproduced by a contiguous run of table rows. That is
    # what makes the "shortest per lower edge" collapse safe: a taller record survives only if it is
    # genuinely the concatenation of the shorter rows it shadows.
    index_of_y = {}
    for i, y in enumerate(row_yl):
        index_of_y[round(y / tol) if tol > 0 else y] = i
    for (y, h) in records:
        i = index_of_y.get(round(y / tol) if tol > 0 else y)
        if i is None or _span_rows(row_yl, row_h, i, h, tol) == 0:
            return PlaceRowTable(
                status=REJECTED,
                message="row record at y=%g height %g is not an exact run of placement rows" % (y, h))

    h0 = row_h[0]
    status = UNIFORM
    for h in row_h:
        if abs(h - h0) > tol:
            status = MIXED
            break
    return PlaceRowTable(row_yl, row_h, status)


def rows_spanned(row_yl, row_h, r, height, tol=None):
    """How many rows a cell of `height` occupies when its bottom sits on row `r`.

    Returns the length of the contiguous run starting at `r` whose heights sum to exactly `height`,
    or -1 when no such run exists -- a 9-track cell can never be handed a 7-track row.
    """
    if r < 0 or r >= len(row_yl):
        return -1
    if tol is None:
        tol = 1e-3 * min(row_h)
    n = _span_rows(row_yl, row_h, r, height, tol)
    return n if n > 0 else -1
