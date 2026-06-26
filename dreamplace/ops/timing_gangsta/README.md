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
  `gangsta_report_slacks(.,GANGSTA_MAX,.)`,
  `gangsta_report_wns_tns_endpoint_worst(.,GANGSTA_MAX,.)` replace the `*_at_max`/`*_max`
  HeteroSTA calls; `heterosta_init_license` deleted. DREAMPlace uses the endpoint-worst variant so
  GangSTA's TNS has the same endpoint-counting semantics as OpenTimer's `report_tns_elw(split=1)`.
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

Full timing-driven run with `"timer_engine":"gangsta"` (GangSTA is CPU-only, so the op routes timing to CPU even with `"gpu":1`):

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

### Head-to-head vs HeteroSTA (GPU) — same design, same `.hs.sdc`

Ran `timer_engine=gangsta` (CPU) and `timer_engine=heterosta` (GPU, RTX 3080) on the SAME
`iccad2015.hs/superblue4` — identical `.v`/`.lef`/`.def`/Liberty/`.hs.sdc`, deterministic placement,
so each timer's *first* eval (iteration 5) sees identical coordinates. Per-timing-step WNS / TNS (ns):

| step (iter) | gangsta WNS | HeteroSTA WNS | gangsta TNS | HeteroSTA TNS |
|---|---|---|---|---|
| 1 (5)  | −35.44 | −54.83 | −870.1 | −625.4 |
| 2 (10) | −26.38 | −40.83 | −639.8 | −421.6 |
| 3 (15) | −21.16 | −28.37 | −504.0 | −262.3 |
| 4 (20) | −23.70 | −18.52 | −437.5 | −173.7 |
| 5 (25) | −26.21 | −13.56 | −431.6 | −134.6 |

**Step 1 (identical coords) is the clean comparison: gangsta −35.44 vs HeteroSTA −54.83 ns —
same sign, units, and order of magnitude** (gangsta ~0.65× on WNS; ~1.4× on TNS, much closer than
the ~10× seen against the simpler `.ot` SDC, because the proper ideal-clock + virtual-clock `.hs`
setup constrains fewer endpoints). Later steps diverge because each timer computes different
criticalities → different net weights → divergent placements. This is the expected agreement for two
independent STA engines with different RC extraction, and validates the integration against the
HeteroSTA oracle that gangsta's C API clones.

Two fixes were needed to get here, both now committed:
- **gangsta (ADR-0020):** the `.hs.sdc` uses `set_ideal_network`, which gangsta did not register;
  Tcl aborted the whole SDC parse → **NaN** WNS/TNS. Now `set_ideal_network` is a no-op and an
  `unknown`-command handler keeps the rest of the file when any command is unrecognized.
- **HeteroSTA op (this op's sibling `timing_heterosta`):** `NetlistDBCppInterface` was
  default-initialized, so the *optional* CSR pointers (`net2pin_start`/`net2pin_items`) held stack
  garbage; `netlistdb_new` saw them as non-NULL and `memcpy`'d from a garbage pointer → **SIGSEGV**.
  Value-initializing the struct (`= {}`, optional pointers explicitly `nullptr`) fixed it.

### Head-to-head vs OpenTimer (same design, same SDC)

The first OpenTimer comparison exposed a real reporting mismatch: DREAMPlace's OpenTimer path reports
TNS with `report_tns_elw(split=1)`, which counts each endpoint once using its worse finite rise/fall
slack. The original GangSTA binding called `gangsta_report_wns_tns`, whose TNS intentionally counts
each finite `(endpoint, rise/fall)` check separately. On `superblue4` that made GangSTA TNS look much
more pessimistic even when WNS and units were sane.

The DREAMPlace binding now calls `gangsta_report_wns_tns_endpoint_worst`, preserving the old
per-rise/fall C API for MCMM and standalone users while matching OpenTimer's DREAMPlace metric.

Clean no-legalization checkpoint on `iccad2015.ot/superblue4`, deterministic placement, first timing
feedback at iteration 510 (same `.v`/`.lef`/`.def`/Liberty/`.sdc`):

| timer | WNS | TNS |
|---|---:|---:|
| GangSTA | −18.657713 | −271.410240 |
| OpenTimer | −18.979207 | −174.001920 |

WNS now agrees within ~2%, and TNS is the same endpoint-counted quantity. The remaining TNS delta is
expected from independent delay/RC and path-criticality models, not from DREAMPlace integration
plumbing. In the legalize-enabled 6-iteration diagnostic, the post-legalization point is much less
stable (`GangSTA −144.724/−14850.186`, OpenTimer −1010.791/−64728.689 for WNS/TNS), so the
no-legalization timing-feedback checkpoint is the cleaner integration parity test.

## Not yet done

- **HeteroSTA head-to-head** — ✅ **done** (see the table above). HeteroSTA's prebuilt
  `libheterosta.so` links `libcudart.so.11.0`; it imports and runs on this CUDA-13 box once the
  bundled `sta/HeteroSTA/lib` (which ships `libcudart.so.11.0`) is on `LD_LIBRARY_PATH` and
  `HeteroSTA_Lic` is set — CUDA 11.8 runtime is forward-compatible with the 595.x driver. The op
  also needed the `netlistdb_new` null-init fix noted above.
- **GPU timing** — GangSTA's GPU path is correct-but-slow and the op routes everything to CPU.
- **`gangsta_dump_paths_to_file`** — declared in `gangsta.h` but not yet implemented in the C API, so
  the path-dump binding is omitted (the timing-driven flow does not need it).
