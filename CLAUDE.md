# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

DREAMPlace is a GPU-accelerated VLSI global placement engine. It reframes nonlinear analytical placement as a deep-learning training problem and implements it on **PyTorch** (CPU multi-threaded, or CUDA GPU). It also integrates ABCDPlace (batch detailed placement) and optional timing-driven placement.

## Build

CMake drives the build; it compiles the C++/CUDA PyTorch extensions, builds the third-party submodules, and installs a runnable copy of `dreamplace/` into the install prefix.

```bash
git submodule init && git submodule update   # required: Limbo, OpenTimer, cub, pybind11, munkres-cpp
pip install -r requirements.txt

mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=../install -DPython_EXECUTABLE=$(which python)
make            # parallel: make -j$(nproc)
make install
```

Key CMake options: `CMAKE_CUDA_ARCHITECTURES` (defaults to `8.0 8.6` plus `8.9 9.0` on CUDA ≥ 12 — the older Pascal/Volta/Turing arches were dropped because CUDA 13's nvcc rejects them; override e.g. `-DCMAKE_CUDA_ARCHITECTURES=80` for a single target), `CMAKE_CXX_ABI` (0|1, default **1** to match modern PyTorch ≥ 2.x built with `_GLIBCXX_USE_CXX11_ABI=1` — **must match** your PyTorch/Boost or linking fails). Boost (>=1.55) and Bison (>=3.3) must be installed on the system; everything else is a submodule. If CUDA is not found, only the CPU path is built.

> **Verified toolchain (this workspace):** CUDA 13.2, PyTorch 2.12 (cu132, ABI=1), NumPy 2.x, GCC 13, on an RTX 3080 (compute 8.6). Builds against newer CUDA/NumPy required the ABI/arch defaults above plus the `np.string_`→`np.bytes_` change in `PlaceDB.py` (NumPy 2.x removed `np.string_`). Use the project venv at `/home/lintaoz/venv-cuda` (`-DPython_EXECUTABLE=$(which python)`).

**Critical workflow detail:** you run code from the *install* directory, not the source tree. `make install` copies the Python files (`dreamplace/*.py`, `dreamplace/ops/**/*.py`) into the install prefix alongside the compiled extensions. **Any edit under `dreamplace/` — even pure Python — requires re-running `make install`** (or manually copying the file) before it takes effect in a run. C++/CUDA edits require `make && make install`.

## Run

### Benchmarks (shared store)

Benchmark data is **not** committed (gitignored). In this workspace there is a single shared store one level up — `../benchmarks/<dataset>` — that is shared with the sibling **Xplace** checkout so each dataset is downloaded only once. It is wired into this repo as per-dataset symlinks under `benchmarks/`:

```
benchmarks/ispd2005 -> ../../benchmarks/ispd2005   # Bookshelf (.aux)
benchmarks/ispd2015 -> ../../benchmarks/ispd2015   # LEF/DEF
benchmarks/ispd2019 -> ../../benchmarks/ispd2019
```

Currently available in the shared store: `ispd2005`, `ispd2015`, `ispd2018`, `ispd2019`. To (re)fetch the classic sets from scratch instead, use `python benchmarks/ispd2005_2015.py` / `python benchmarks/ispd2019.py` (these download into `benchmarks/`).

### Run

Runs execute from the *install* dir (see Build), and `make install` does **not** copy the benchmark symlinks — so link the shared data into the install dir once after each `make install`:

```bash
cd install
ln -sfn ../../../benchmarks/ispd2005 benchmarks/ispd2005     # shared store is ../../../benchmarks/<dataset> from here
python dreamplace/Placer.py test/ispd2005/adaptec1.json      # verified GPU run: legal, writes results/adaptec1/
python dreamplace/Placer.py --help                           # list all JSON options
```

Configs are JSON (see `dreamplace/params.json` for the full schema with defaults/descriptions; `test/*/` for examples). Set `"gpu": 1` for the CUDA path or `"gpu": 0` for CPU. A run writes solutions and optional plots/GIFs to `params.result_dir`.

## Tests

Each operator has a standalone unittest that exercises the CPU and (if built) GPU kernels against a reference. Run a single one directly:

```bash
cd <install dir>
python unittest/ops/hpwl_unittest.py
python unittest/ops/place_io_unittest/place_io_unittest.py
```

`unittest/regression/` holds end-to-end placement regressions per benchmark suite (ispd2005, ispd2015, iccad2014, etc.).

## Architecture

**Python orchestration layer** (`dreamplace/`):
- `Placer.py` — entry point and top-level flow: load `Params` → build `PlaceDB` → optional `Timer` → run `NonLinearPlace` → write solution → optional external detailed placer (NTUPlace3/NTUplace_4dr).
- `Params.py` / `params.json` — every tunable is a JSON key with a default and help string in `params.json`; `Params` loads it. This is the single source of truth for run options.
- `PlaceDB.py` — the placement database. Reads benchmark inputs (Bookshelf `.aux`, or LEF/DEF, optionally Verilog) via the `place_io` C++ extension, holds netlist/geometry arrays, and writes solutions.
- `BasicPlace.py` — base class wiring the PlaceDB into PyTorch tensors and constructing the operator instances (wirelength, density, legalization, etc.).
- `NonLinearPlace.py` — the placement engine (subclasses `BasicPlace`). Drives the optimization loop over multiple density-weight stages, using `NesterovAcceleratedGradientOptimizer.py`.
- `PlaceObj.py` — the differentiable objective: combines weighted-average/log-sum-exp wirelength with the electrostatics-based density penalty; manages density weight scheduling, net weighting, region/timing terms.
- `Timer.py` — optional STA wrapper; `timer_engine` selects `opentimer` or `heterosta` (GPU). Only active when `timing_opt_flag` is set.
- `EvalMetrics.py` — per-iteration metrics (HPWL, overflow, density).

**Operator layer** (`dreamplace/ops/<op>/`): each operator is self-contained and follows one pattern — a Python wrapper exposing a `torch.autograd.Function` (`<op>.py`), and a `src/` with CPU C++ (`*.cpp`), CUDA (`*_cuda.cpp` + `*_cuda_kernel.cu`), and often an `_atomic` variant. These compile into pybind extensions imported as `<op>_cpp` / `<op>_cuda` (the `.py` picks CUDA vs CPU at runtime by `pos.is_cuda` and `configure.compile_configurations["CUDA_FOUND"]`). When adding/changing an operator, keep the CPU and CUDA paths and the `forward`/`backward` semantics in sync, register it in the op's `CMakeLists.txt`, and add/update its `unittest/ops/` test. Notable ops: `weighted_average_wirelength`, `logsumexp_wirelength`, `electric_potential` (density via FFT/DCT in `dct/`), the legalizers (`greedy_legalize`, `abacus_legalize`, `macro_legalize`), and detailed-placement ops (`independent_set_matching`, `global_swap`, `k_reorder`).

**Third party** (`thirdparty/`, git submodules): Limbo (parsers/util), OpenTimer (STA), HeteroSTA (GPU STA), cub, pybind11, munkres-cpp, flute, NCTUgr (routability).

## GangSTA timer backend (`timer_engine=gangsta`) — delivered + validated

A third timer engine backed by the external [GangSTA](../) STA engine, beside `opentimer`/`heterosta`.
GangSTA's open C API is a functional-parity clone of `heterosta.h`, so `dreamplace/ops/timing_gangsta/`
mirrors `timing_heterosta/` with the calls remapped (no license — GangSTA is open-source). It builds
(registered in `dreamplace/ops/CMakeLists.txt`), is dispatched in `Timer.py`/`BasicPlace.py`/
`NonLinearPlace.py`, and runs the full timing-driven loop.

**Enable:** set `"timer_engine": "gangsta"` (with `"timing_opt_flag": 1` and the Liberty/SDC inputs).
The op links `libgangsta.a` (PIC) statically; CMake finds the gangsta repo from
`<DREAMPlace>/../..`, override with `-DGANGSTA_ROOT=`. GangSTA is CPU-only, so the op routes
`forward`/net-weighting to CPU regardless of `gpu`.

**Validated on ICCAD-2015 `superblue4`** (CPU run): in-memory netlist (795645 cells / 2.50M pins /
802513 nets), `build_graph` success (LEF hard macros are black-boxed gangsta-side), pin permutation
100% matched by name (0 unmatched → seams correct), per-iteration `extract_rc`→`report` (~1.2 s), and
a real timing-driven loop where WNS improves (−43.96 → −26.6 ns over 5 timing steps). The five seams
(0-based cells, kept `:` separator, **port-direction passthrough** — place_io already uses GangSTA's
convention, so do NOT invert — gangsta↔dreamplace slack permutation, ff/kΩ RC units) are documented
and validated in `dreamplace/ops/timing_gangsta/README.md`.

**Not yet done:** a numeric head-to-head vs `heterosta`/`opentimer` is blocked on their preprocessed
benchmark packages (`benchmarks/iccad2015.hs` / `.ot` with `.hs.sdc`/`.ot.sdc`, separate Google-Drive
downloads absent locally); the op was validated against the standalone gangsta engine instead. A raw
ICCAD-2015 config (merged tech+cell LEF, raw `.sdc`) is the path used for the validation run.

## Conventions

- File headers use the `## @file / @author / @date / @brief` Doxygen-style block; match it in new files.
- Numerical heavy lifting lives in C++/CUDA ops; Python is orchestration. Prefer adding a tensor op over Python-loop numerics.
- Determinism: a `deterministic_flag` exists for run-to-run reproducibility at some runtime cost — preserve it when touching parallel ops.
