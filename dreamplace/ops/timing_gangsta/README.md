# timing_gangsta — GangSTA timer backend for DREAMPlace (WIP scaffold)

A third `timer_engine` (beside `opentimer` and `heterosta`) backed by the
[GangSTA](../../../../) STA engine, for timing-driven placement and timer comparison. GangSTA's C
API is a functional-parity clone of `heterosta.h`, so this op is a copy of `timing_heterosta/` with
the HeteroSTA calls remapped to GangSTA.

> **STATUS: WIP scaffold — does NOT yet build or run.** It is intentionally *not* registered in
> `dreamplace/ops/CMakeLists.txt`, so it cannot break the build. The mechanical renames are done; the
> remaining seams below need finishing AND validation on a real timing design (ICCAD-2015), which is
> not in the local corpus. Do not enable it until the seams are validated — several are wrong-by-
> default for GangSTA and would silently produce incorrect timing.

## What GangSTA already provides (done + unit-tested in the gangsta repo)

The two HeteroSTA-only capabilities this op depends on are implemented and validated:
- **In-memory netlist** — `gangsta_set_netlist_inmem(...)` (replaces `netlistdb_new` +
  `heterosta_set_netlistdb`). Mirrors `NetlistDBCppInterface`. Byte-identical to a Verilog build
  (`inmem_netlist_test`).
- **RC from placement** — `gangsta_extract_rc_from_placement(...)` (same signature as HeteroSTA's;
  CPU per-sink **star** model, a documented approximation of FLUTE/PDR). `extract_rc_test`.
Plus the rest of the surface (`gangsta_read_liberty/sdc`, `build_graph`, `update_delay/arrivals`,
`report_wns_tns`, `report_slacks`, `get_is_endpoint`, bulk `read_pin_timing`, `num_pins/pin_name`).

## Remaining work to finish the op

### A. Mechanical C-API signature fixes (determinable; just edit the call sites)

| HeteroSTA call (still in this scaffold) | GangSTA replacement |
|---|---|
| `heterosta_set_delay_calculator_elmore(&sta)` | `gangsta_set_delay_calculator(&sta, GANGSTA_DELAY_ELMORE)` |
| `heterosta_report_slacks_at_max(&sta, a, uc)` | `gangsta_report_slacks(&sta, GANGSTA_MAX, a, uc)` |
| `heterosta_report_wns_tns_max(&sta, &w, &t, uc)` | `gangsta_report_wns_tns(&sta, GANGSTA_MAX, &w, &t, uc)` |
| `heterosta_dump_paths_max_to_file(&sta, n, nw, p, uc)` | `gangsta_dump_paths_to_file(&sta, GANGSTA_MAX, n, nw, p, uc)` |
| `heterosta_init_license(...)` | *delete* (GangSTA is open; no license) |
| `heterosta_launch_debug_shell(...)` | *delete* (already commented) |
| `netlistdb_new` / `heterosta_set_netlistdb` / `NetlistDB*` | *delete*; call `gangsta_set_netlist_inmem` (see B) |

(`heterosta_new/free/init_logger/read_liberty/batch_read_liberty/read_sdc/read_spef/build_graph/
zero_slew/flatten/get_is_endpoint/update_delay/update_arrivals/extract_rc_from_placement/reset/
write_spef` were already renamed 1:1 by the scaffold.)

### B. Netlist setup — `src/timing_gs_io_cpp.cpp`

Replace the `NetlistDB` build in `buildTimerDB()` with, after `gangsta_read_liberty`:
```c
gangsta_set_delay_calculator(&sta, GANGSTA_DELAY_ELMORE);
gangsta_set_netlist_inmem(&sta, g_netlist_data.design_name.c_str(),
    num_cells, cell_name_ptrs, cell_type_ptrs,
    num_pins, pin_name_ptrs, pin_directions, pin2cell_map, pin2net_map,
    num_nets, net_name_ptrs);
gangsta_build_graph(&sta);
```
`build_netlistdb_from_dreamplace` keeps populating `g_netlist_data` but returns `bool` (no
`NetlistDB*`).

### C. ⚠️ Semantic seams — wrong-by-default for GangSTA; MUST validate on real data

1. **Cell indexing.** HeteroSTA reserves cell 0 as a top-module sentinel and uses `pin2cell =
   node_id + 1` (`setup_cell_data` pushes an empty cell 0 = designName; `setup_pin_data` line ~535).
   GangSTA wants **0-based real cells** (no sentinel): `totalcells = numMovable + numFixed`, drop the
   sentinel push, `pin2cell = node_id`. A `celltype` of the design name is not a library cell and
   would make `build()` fail.
2. **Pin-name separator.** `setup_pin_data` does `std::replace(name, ':', '/')` for instance pins.
   GangSTA detects ports by the **absence of `:`** and parses the library pin name **after `:`** —
   so KEEP the `:` (remove the replace), else every instance pin is misread as a top port.
3. **Top-port direction.** GangSTA's `pindirection` is standard-cell-oriented: a top **input** port
   must be marked as an **output pin (1)**, a top output as input (0). Verify whether DREAMPlace's
   `pin_direct` is already inverted for top ports; if not, invert it for port pins here.
4. **Pin permutation.** HeteroSTA preserves DREAMPlace's pin order; GangSTA's `build_netlist`
   **renumbers** pins. After `build_graph`, build maps by name:
   `g2d[g] = dreamplace_id_of(gangsta_pin_name(g))`, `d2g[d] = gangsta_lookup_pin(pinname[d])`.
   In `timing_gs_cpp.cpp`: reorder coords DREAMPlace→GangSTA before `gangsta_extract_rc_from_placement`
   (`xs_g[g] = x[g2d[g]]`), and remap slacks GangSTA→DREAMPlace after `gangsta_report_slacks`
   (`slack_d[d] = slack_g[d2g[d]]`) so the net-weight loop indexes correctly.
5. **RC units.** The op's `unit_cap_xy`/`unit_res_xy` are tuned for HeteroSTA's Rust canonical units
   (`res_unit=1e3`, `cap_unit=1e-15`). GangSTA's star RC expects ff/kohm; confirm the scale so
   absolute delays are meaningful (load *ratios* are fine regardless).

### D. Build — `CMakeLists.txt`

Replace the `-lheterosta` link + `HETEROSTA_*` vars with the GangSTA static lib (as the Xplace
integration does): find `libgangsta.a` under `<gangsta>/build/src`, add `<gangsta>/include`, and link
`${GANGSTA_LIB} ${TCL_LIBRARY}` (GangSTA needs `libtcl8.6`). Then add
`add_subdirectory(timing_gangsta)` to `dreamplace/ops/CMakeLists.txt`.

### E. Dispatch

- `dreamplace/Timer.py`: add `"gangsta"` to the allowed engines (line ~25) and an import branch
  (lines ~38-46) importing `dreamplace.ops.timing_gangsta.timing_gs` +
  `...timing_gangsta_cpp`.
- `dreamplace/ops/timing_gangsta/timing_gs.py`: the scaffold already renamed the module imports;
  verify against the finished `timing_gangsta_cpp` symbol names.
- `dreamplace/BasicPlace.py` / `dreamplace/NonLinearPlace.py`: add `gangsta` alongside the
  `heterosta` branches (same `TimingOpt` wiring).
- `params.json`: document `gangsta` as a valid `timer_engine`.

### F. Validation (blocked on data)

No ICCAD-2015 design is present locally (`benchmarks/` has only LEF/DEF ispd sets without
`*_Early/_Late.lib` + `.sdc`). Finishing C-E without a timing design to run means the semantic seams
(C) cannot be verified — so the op stays unregistered until an ICCAD-2015 design is available and a
`timer_engine=gangsta` run can be compared against `heterosta`/`opentimer`.
