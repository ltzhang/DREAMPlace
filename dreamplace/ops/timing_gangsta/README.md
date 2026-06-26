# timing_gangsta — GangSTA timer backend for DREAMPlace

A third `timer_engine` (beside `opentimer` and `heterosta`) backed by the
[GangSTA](../../../../) STA engine, for timing-driven placement and timer comparison. GangSTA's open
C API is a functional-parity clone of `heterosta.h`, so this op mirrors `timing_heterosta/` with the
HeteroSTA calls remapped to GangSTA (no license; GangSTA is open-source).

> **STATUS: delivered and validated on ICCAD-2015.** Builds (registered in `dreamplace/ops/CMakeLists.txt`),
> imports, and runs the full timing-driven loop. Enable with `"timer_engine": "gangsta"` in the JSON
> config (requires `timing_opt_flag: 1` and the Liberty/SDC inputs).

## Build

The op statically links `libgangsta.a` (PIC) + its embedded-Tcl SDC dependency. CMake locates the
gangsta repo from `<gangsta>/placement/DREAMPlace/../..`; override with `-DGANGSTA_ROOT=<path>` if the
repo lives elsewhere. Build gangsta first (`cmake --build <gangsta>/build`), then build this op:

```bash
cd <gangsta>/placement/DREAMPlace/build
cmake . -DGANGSTA_ROOT=<gangsta>
cmake --build . --target timing_gangsta_cpp -j   # then `make install` (or copy the .so + *.py)
```

## What was implemented (vs the HeteroSTA scaffold)

- **Open C API remap (no NetlistDB, no license):** `gangsta_set_netlist_inmem` replaces
  `netlistdb_new`/`heterosta_set_netlistdb`; `gangsta_set_delay_calculator(.,GANGSTA_DELAY_ELMORE)`,
  `gangsta_report_slacks(.,GANGSTA_MAX,.)`, `gangsta_report_wns_tns(.,GANGSTA_MAX,.)` replace the
  `*_at_max`/`*_max` HeteroSTA calls; `heterosta_init_license` deleted.
- **Semantic seams (validated on `superblue4`):**
  1. **0-based cells** — `totalcells = numMovable + numFixed`, no top-module sentinel; `pin2cell = node_id`.
  2. **Keep the `:` pin separator** — GangSTA detects top ports by the *absence* of `:` and reads the
     library pin name *after* `:`. (Confirmed: the pin permutation below matches 100% by name.)
  3. **Port direction passthrough** — GangSTA wants a top input as an output pin (1) and a top output
     as input (0); DREAMPlace/place_io *already* stores ports with this convention, so it is passed
     through, **not** inverted. (Inverting made output ports false drivers → "net has multiple drivers".)
  4. **Pin permutation** — GangSTA renumbers pins at build, so after `build_graph` the op builds
     `g2d`/`d2g` maps by name (`gangsta_pin_name` ↔ DREAMPlace pin name), reorders coords
     DREAMPlace→GangSTA before `extract_rc_from_placement`, and remaps slacks GangSTA→DREAMPlace.
  5. **RC units** — the per-micron R/C are scaled to ff/kΩ per DEF-unit (GangSTA's `extract_rc` units),
     identical to HeteroSTA's canonical scaling.
- **Black-box hard macros** — GangSTA now black-boxes a `cell_type` absent from Liberty (LEF blocks
  like `block_9x9_0`); the op feeds per-pin directions so the build succeeds. (gangsta-side change,
  `black_box_test`.)
- **CPU routing** — GangSTA is CPU-only, so `forward`/`update_net_weights` move coords/tensors to CPU,
  run on the CPU launcher, and copy results back; the CUDA kernel is retained for ABI parity but unused.
- **Dispatch** — `Timer.py` (engine allowlist + lazy import), `BasicPlace.py` (shares the HeteroSTA
  `TimingOpt` via module alias), `NonLinearPlace.py` (gangsta grouped with heterosta at the 3 branches).

## Validation (ICCAD-2015 `superblue4`)

Full timing-driven run with `"timer_engine":"gangsta"` (CPU; the box's GPU dropped off the bus mid-session):

- Netlist assembled in-memory: **795645 cells, 2497940 pins (6623 top ports), 802513 nets**;
  `build_graph` succeeds (black-boxing the LEF macros).
- **Pin permutation: 2497940/2497940 mapped, 0 unmatched** — confirms seams C.1–C.3 are correct
  (every gangsta pin name equals its DREAMPlace pin name).
- Per timing step: `extract_rc_from_placement` (2.50M pins) → `update_delay`/`update_arrivals` →
  `report` in ~1.2 s; net-weight update ~0.45 s.
- **WNS improves over the loop**: −43.96 → −26.6 ns across 5 timing steps as criticality-driven
  net-weighting tightens timing — the expected timing-driven-placement behavior.

The numbers are pessimistic in absolute terms (early in placement; star RC is approximate;
`set_driving_cell` is not modeled) but finite, sane, and trending correctly.

## Not yet done

- **Head-to-head vs `heterosta`/`opentimer` on the same design** — blocked on the HeteroSTA/OpenTimer
  benchmark packages (`benchmarks/iccad2015.hs` / `.ot`, with `.hs.sdc`/`.ot.sdc`), which are separate
  Google-Drive downloads not present locally. The gangsta op was validated against the standalone
  gangsta engine (self-consistent) instead.
- **GPU timing** — GangSTA's GPU path is correct-but-slow and the op routes everything to CPU.
- **`gangsta_dump_paths_to_file`** — declared in `gangsta.h` but not yet implemented in the C API, so
  the path-dump binding is omitted (the timing-driven flow does not need it).
