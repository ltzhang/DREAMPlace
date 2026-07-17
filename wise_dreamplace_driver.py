"""WiseSyn-owned seam over DREAMPlace's placement flow (ADR-0034, Milestone D).

Isolates the wisesyn C++ embedding from DREAMPlace's ``Params``/``PlaceDB``/``NonLinearPlace``
internals: two clean entry points, one file to update if DREAMPlace's option set drifts.

* ``place_arrays(arrays, ...)`` — the MANDATORY in-memory path. Builds a DREAMPlace ``PlaceDB``
  directly from plain per-node / per-pin / per-net arrays (no LEF/DEF on disk, no C++ ``rawdb``),
  runs the SAME non-linear global placement + legalization the file path runs, and returns the
  placed lower-left coordinates in the caller's INPUT node order.
* ``place(lef_paths, in_def, out_def, ...)`` — the DEF-interchange fallback / sanity path: drives
  DREAMPlace's own LEF/DEF reader + full flow and copies the placed DEF to ``out_def``.

Unlike xplace, DREAMPlace runs GPU-free: ``gpu=0`` selects the CPU (OpenMP) path — the Milestone-D
headline. ``gpu=1`` requires CUDA and raises loudly with no silent CPU downgrade (rule #7).

The caller (the embedded interpreter, or a test) puts torch's site-packages on ``sys.path``; this
module makes the DREAMPlace package (its compiled ops) and its bare-name inner modules importable.
"""
import os
import sys
import glob
import shutil
import logging
import tempfile

# The DREAMPlace repo root (this file's directory) and its inner package dir. The compiled ops are
# installed in-place under <root>/dreamplace/ops/*/*.so (WISE_ENABLE_DREAMPLACE build). DREAMPlace's
# modules import each other by bare name (`import BasicPlace`), so the inner dir must be on sys.path.
_HERE = os.path.dirname(os.path.abspath(__file__))
_PKG = os.path.join(_HERE, "dreamplace")
for _p in (_PKG, _HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)


def available():
    """True iff torch and the compiled DREAMPlace package import. Cheap probe for the loud fallback."""
    try:
        import torch  # noqa: F401
        import dreamplace.ops.place_io.place_io as _pio  # compiled op — proves the ops are built
        import Params, PlaceDB, NonLinearPlace  # noqa: F401  (bare-name inner modules resolve)
        return True
    except Exception:
        return False


def _make_params(util, seed, deterministic, gpu, timer, result_dir):
    """A DREAMPlace Params with defaults, tuned for a wirelength-driven (or timed) single run."""
    import Params
    params = Params.Params()
    d = params.__dict__
    d["gpu"] = 1 if gpu else 0
    d["target_density"] = float(util)
    d["random_seed"] = int(seed)
    d["deterministic_flag"] = 1 if deterministic else 0
    d["global_place_flag"] = 1
    d["legalize_flag"] = 1
    d["detailed_place_flag"] = 1  # E1: detailed placement, fair+clean vs XPlace
    d["plot_flag"] = 0
    d["dtype"] = "float32"
    d["result_dir"] = result_dir
    # A design name for DREAMPlace's dump/log paths; no file is read on the array path.
    d["def_input"] = "wise_top.def"
    # Inner-loop timer selection (ADR-0034). "" / "none" -> wirelength-driven (timing off).
    t = (timer or "").strip().lower()
    if t in ("opentimer", "gangsta"):
        d["timing_opt_flag"] = 1
        d["timer_engine"] = t
    else:
        d["timing_opt_flag"] = 0
    return params


def _build_placedb_from_arrays(arrays, params):
    """Construct a DREAMPlace PlaceDB from plain arrays (no rawdb). Returns (db, perm, num_movable).

    ``arrays`` keys (python lists / numpy; converted here):
      * ``node_size``      (N,2) float — cell width,height in DB units.
      * ``node_lpos``      (N,2) float — lower-left initial position in DB units.
      * ``node_type``      len-N — "Mov" (movable std cell) | "Fix" (fixed macro) | "IOPin" (IO anchor).
      * ``pin_id2node_id`` (P,) int — parent node (INPUT order) of each pin.
      * ``pin_rel_lpos``   (P,2) float — pin offset relative to its node's lower-left corner.
      * ``net2pin_list`` + ``net2pin_end`` — CSR: pin ids grouped by net; cumulative end index.
      * ``die_info``       (xl,xh,yl,yh) — die/core box in DB units.
      * ``site_info``      (site_width, row_height) in DB units.
    Optional: ``node_names`` (len-N), ``pin_direct`` (len-P of b"I"/b"O").

    Nodes are stably reordered into DREAMPlace's canonical segmentation [movable, fixed terminals,
    terminal_NIs]; ``perm`` maps canonical row k back to the caller's input index, so the placed
    coordinates can be returned in INPUT order.
    """
    import numpy as np
    import torch
    import PlaceDB

    db = PlaceDB.PlaceDB()
    db.dtype = np.float32
    db.rawdb = None
    db.device = torch.device("cuda" if params.gpu else "cpu")

    node_size = np.asarray(arrays["node_size"], np.float32).reshape(-1, 2)
    node_lpos = np.asarray(arrays["node_lpos"], np.float32).reshape(-1, 2)
    node_type = list(arrays["node_type"])
    N = node_size.shape[0]
    if node_lpos.shape[0] != N or len(node_type) != N:
        raise ValueError("node_size/node_lpos/node_type length mismatch")

    # Canonical segmentation: movable (0), fixed terminals (1), terminal_NIs / IO pins (2).
    rank = {"Mov": 0, "Fix": 1, "IOPin": 2}
    order = sorted(range(N), key=lambda i: rank[node_type[i]])
    perm = np.array(order, dtype=np.int64)
    inv = np.empty(N, np.int64)
    inv[perm] = np.arange(N)

    node_size = node_size[perm]
    node_lpos = node_lpos[perm]
    ntype = [node_type[i] for i in order]
    num_mov = ntype.count("Mov")
    num_fix = ntype.count("Fix")
    num_ni = ntype.count("IOPin")

    db.num_physical_nodes = N
    db.num_terminals = num_fix
    db.num_terminal_NIs = num_ni
    db.node_size_x = np.ascontiguousarray(node_size[:, 0])
    db.node_size_y = np.ascontiguousarray(node_size[:, 1])
    db.node_x = np.ascontiguousarray(node_lpos[:, 0])
    db.node_y = np.ascontiguousarray(node_lpos[:, 1])
    db.node_orient = np.array(["N"] * N, dtype=np.bytes_)
    names = list(arrays["node_names"]) if arrays.get("node_names") else ["n%d" % i for i in range(N)]
    names = [str(names[i]) for i in order]
    db.node_names = np.array(names, dtype=np.bytes_)
    db.node_name2id_map = {n: i for i, n in enumerate(names)}
    db.node2orig_node_map = np.arange(N, dtype=np.int32)

    # Pins: parent node remapped through the node permutation; offsets are lower-left relative.
    pin2node_in = np.asarray(arrays["pin_id2node_id"], np.int64).reshape(-1)
    P = pin2node_in.shape[0]
    if P and (int(pin2node_in.min()) < 0 or int(pin2node_in.max()) >= N):
        raise ValueError("pin_id2node_id out of range")
    pin_off = np.asarray(arrays["pin_rel_lpos"], np.float32).reshape(-1, 2)
    if pin_off.shape[0] != P:
        raise ValueError("pin_rel_lpos rows != num_pins")
    pin2node = inv[pin2node_in].astype(np.int32)
    db.pin_offset_x = np.ascontiguousarray(pin_off[:, 0])
    db.pin_offset_y = np.ascontiguousarray(pin_off[:, 1])
    db.pin_direct = np.array(list(arrays["pin_direct"]) if arrays.get("pin_direct") else [b"I"] * P,
                             dtype=np.bytes_)
    db.pin_names = np.array(["p%d" % i for i in range(P)], dtype=np.bytes_)
    db.pin_name2id_map = {"p%d" % i: i for i in range(P)}

    # Nets from CSR.
    net2pin_list = np.asarray(arrays["net2pin_list"], np.int64).reshape(-1)
    net2pin_end = np.asarray(arrays["net2pin_end"], np.int64).reshape(-1)
    num_nets = net2pin_end.shape[0]
    if P and int(net2pin_end[-1]) != P:
        raise ValueError("net2pin_end[-1] != num_pins")
    seg_start = np.zeros(num_nets, np.int64)
    seg_start[1:] = net2pin_end[:-1]
    net2pin_map = np.empty(num_nets, dtype=object)
    pin2net = np.full(P, -1, np.int32)
    for nid in range(num_nets):
        pins = net2pin_list[seg_start[nid]:net2pin_end[nid]].astype(np.int32)
        net2pin_map[nid] = pins
        pin2net[pins] = nid
    db.net2pin_map = net2pin_map
    db.pin2net_map = pin2net
    db.pin2node_map = pin2node
    db.flat_net2pin_map = net2pin_list.astype(np.int32)
    db.flat_net2pin_start_map = np.concatenate([[0], net2pin_end]).astype(np.int32)
    db.net_names = np.array(["net%d" % i for i in range(num_nets)], dtype=np.bytes_)
    db.net_name2id_map = {"net%d" % i: i for i in range(num_nets)}
    db.net_weights = np.ones(num_nets, np.float32)
    db.net_weight_deltas = np.zeros(num_nets, np.float32)
    db.net_criticality = np.zeros(num_nets, np.float32)
    db.net_criticality_deltas = np.zeros(num_nets, np.float32)

    # node -> pin CSR (pins grouped by canonical node id; ascending pin id within a node).
    node2pin_map = np.empty(N, dtype=object)
    buckets = [[] for _ in range(N)]
    for pid in range(P):
        buckets[int(pin2node[pid])].append(pid)
    flat_node2pin = []
    flat_start = [0]
    for nid in range(N):
        node2pin_map[nid] = np.array(buckets[nid], np.int32)
        flat_node2pin.extend(buckets[nid])
        flat_start.append(len(flat_node2pin))
    db.node2pin_map = node2pin_map
    db.flat_node2pin_map = np.array(flat_node2pin, np.int32)
    db.flat_node2pin_start_map = np.array(flat_start, np.int32)
    db.num_movable_pins = int((pin2node < num_mov).sum())

    # Die / rows / site.
    lx, hx, ly, hy = [float(v) for v in arrays["die_info"]]
    site_w, row_h = [float(v) for v in arrays["site_info"]]
    if not (hx > lx and hy > ly):
        raise ValueError("die_info degenerate (need xl<xh, yl<yh)")
    if not (site_w > 0 and row_h > 0):
        raise ValueError("site_info degenerate (need site_width>0, row_height>0)")
    db.xl, db.xh, db.yl, db.yh = lx, hx, ly, hy
    db.row_height = row_h
    db.site_width = site_w
    nrows = max(1, int(round((hy - ly) / row_h)))
    db.rows = np.array([[lx, ly + r * row_h, hx, ly + (r + 1) * row_h] for r in range(nrows)],
                       dtype=np.float32)
    db.total_space_area = float((hx - lx) * (hy - ly))

    # No fence regions: one implicit region = the whole die.
    db.regions = []
    db.flat_region_boxes = np.zeros((0, 4), np.float32)
    db.flat_region_boxes_start = np.array([0], np.int32)
    db.node2fence_region_map = np.zeros(N, np.int32)

    # Routing grids parked (routability opt off).
    db.num_routing_grids_x = params.route_num_bins_x
    db.num_routing_grids_y = params.route_num_bins_y
    db.num_routing_layers = 1
    db.unit_horizontal_capacity = params.unit_horizontal_capacity
    db.unit_vertical_capacity = params.unit_vertical_capacity
    db.routing_grid_xl, db.routing_grid_yl = db.xl, db.yl
    db.routing_grid_xh, db.routing_grid_yh = db.xh, db.yh
    db.max_net_weight = np.float64(params.max_net_weight)
    return db, perm, num_mov


def place_arrays(arrays, util=0.8, seed=1000, deterministic=True, gpu=0, timer=""):
    """Run DREAMPlace global placement + legalization on an in-memory array netlist (no DEF on disk).

    Returns ``{"ok": bool, "coords": [[x,y], ...], "gp_hpwl": float, "error": str}`` where ``coords``
    are placed LOWER-LEFT positions in DB units, one row per INPUT node (movable rows carry the placed
    position; fixed/IO rows echo their input position). Never raises across the boundary — any failure
    is reported in ["error"] with ok=False and no fabricated coordinates (rule #7).
    """
    result = {"ok": False, "coords": [], "gp_hpwl": -1.0, "error": ""}
    prev_cwd = os.getcwd()
    outdir = None
    try:
        import numpy as np
        import torch
        import NonLinearPlace

        if gpu and not torch.cuda.is_available():
            result["error"] = "gpu=1 requested but torch.cuda.is_available() is False (no CUDA/GPU)"
            return result
        if (timer or "").strip().lower() in ("opentimer", "gangsta"):
            # Timing-driven placement needs a Timer built from the design's .lib/.sdc, which WiseSyn does
            # not yet hand to the DREAMPlace inner loop (recorded 🟡: timing-driven array-ingest,
            # ADR-0034). Reject loudly here rather than run wirelength-driven and pretend it was timed (#7).
            result["error"] = ("inner-loop timer '%s' requested, but WiseSyn does not yet feed .lib/.sdc "
                               "to the DREAMPlace timer (deferred; ADR-0034). Run without -timer for "
                               "wirelength-driven placement." % timer)
            return result

        os.chdir(_HERE)
        logging.getLogger().setLevel(logging.WARNING)  # keep the embedded run quiet
        outdir = tempfile.mkdtemp(prefix="wise_dreamplace_arr_")
        params = _make_params(util, seed, deterministic, gpu, timer, outdir)

        np.random.seed(params.random_seed)
        torch.manual_seed(params.random_seed)

        db, perm, num_mov = _build_placedb_from_arrays(arrays, params)
        db.initialize(params)
        placer = NonLinearPlace.NonLinearPlace(params, db, None)
        metrics = placer(params, db, None)

        pos = placer.pos[0].data.cpu().numpy()
        num_nodes = db.num_nodes  # physical + fillers
        N = db.num_physical_nodes
        unscale = 1.0 / params.scale_factor
        xs = pos[:N] * unscale + params.shift_factor[0]
        ys = pos[num_nodes:num_nodes + N] * unscale + params.shift_factor[1]

        # Un-permute canonical rows back to input order; emit one [x,y] per input node.
        coords = [None] * N
        for k in range(N):
            coords[int(perm[k])] = [float(xs[k]), float(ys[k])]
        result["coords"] = coords
        try:
            result["gp_hpwl"] = float(metrics[-1].hpwl) if metrics else -1.0
        except Exception:
            pass
        result["ok"] = True
        return result
    except BaseException as e:  # never leak across the C++ boundary
        import traceback
        result["error"] = "dreamplace array driver: %s\n%s" % (e, traceback.format_exc())
        result["coords"] = []
        result["ok"] = False
        return result
    finally:
        os.chdir(prev_cwd)
        if outdir is not None:
            shutil.rmtree(outdir, ignore_errors=True)


def place(lef_paths, in_def, out_def, util=0.8, site="", seed=1000, deterministic=True, gpu=0, timer=""):
    """DEF-interchange fallback: run DREAMPlace's own LEF/DEF flow and copy the placed DEF to out_def.

    Returns ``{"ok": bool, "gp_hpwl": float, "error": str}``. Never raises across the boundary.
    """
    result = {"ok": False, "gp_hpwl": -1.0, "error": ""}
    prev_cwd = os.getcwd()
    outdir = None
    lef_paths = [os.path.abspath(p) for p in lef_paths]
    in_def = os.path.abspath(in_def)
    out_def = os.path.abspath(out_def)
    try:
        import torch
        import Placer

        if gpu and not torch.cuda.is_available():
            result["error"] = "gpu=1 requested but torch.cuda.is_available() is False (no CUDA/GPU)"
            return result
        if (timer or "").strip().lower() in ("opentimer", "gangsta"):
            result["error"] = ("inner-loop timer '%s' requested, but WiseSyn does not yet feed .lib/.sdc "
                               "to the DREAMPlace timer (deferred; ADR-0034). Run without -timer for "
                               "wirelength-driven placement." % timer)
            return result

        os.chdir(_HERE)
        logging.getLogger().setLevel(logging.WARNING)
        outdir = tempfile.mkdtemp(prefix="wise_dreamplace_def_")
        params = _make_params(util, seed, deterministic, gpu, timer, outdir)
        d = params.__dict__
        d["lef_input"] = list(lef_paths)
        d["def_input"] = in_def
        d["detailed_place_flag"] = 1  # E1: detailed placement, fair+clean vs XPlace

        Placer.place(params, None)

        # DREAMPlace writes <result_dir>/<design>/<design>.gp.def. design_name = basename(in_def).
        design = os.path.basename(in_def).replace(".def", "").replace(".DEF", "")
        pattern = os.path.join(outdir, design, "*.def")
        defs = sorted(glob.glob(pattern), key=os.path.getmtime)
        if not defs:
            result["error"] = "dreamplace wrote no placement DEF (looked for %s)" % pattern
            return result
        shutil.copyfile(defs[-1], out_def)
        result["ok"] = os.path.exists(out_def)
        if not result["ok"]:
            result["error"] = "failed to copy placed DEF to %s" % out_def
        return result
    except BaseException as e:
        import traceback
        result["error"] = "dreamplace DEF driver: %s\n%s" % (e, traceback.format_exc())
        return result
    finally:
        os.chdir(prev_cwd)
        if outdir is not None:
            shutil.rmtree(outdir, ignore_errors=True)
