# D-OpenSubdiv

D bindings for a thin C shim around Pixar's **OpenSubdiv** 3.x, scoped to
the "build topology + stencil table once, evaluate limit positions per
frame" workflow that drives interactive subdivision-surface previews.

## Layout

```
include/osd_c.h            — C API (extern "C")
csrc/osd_c.cpp             — implementation, wraps Far + Osd::CpuEvaluator
source/osd/c.d             — D extern(C) bindings, 1-to-1 with osd_c.h
CMakeLists.txt             — builds libosdc.a + vendored OpenSubdiv
extern/OpenSubdiv          — git submodule, pinned to v3_7_0
examples/cube_subdiv.d     — smoke test
```

## First build

```sh
git submodule update --init --recursive
dub build
```

The `preBuildCommands-posix` hook in `dub.json` invokes CMake on the
submodule + shim before `dub` compiles the D side, so a plain
`dub build` is enough. Output: `libd-opensubdiv.a` (D bindings,
package root) + `build/libosdc.a` and
`build/extern/OpenSubdiv/opensubdiv/libosdCPU.a` (native).

## Smoke test

```sh
rdmd -Isource \
     -L=build/libosdc.a \
     -L=build/extern/OpenSubdiv/opensubdiv/libosdCPU.a \
     -L=-lstdc++ -L=-lm \
     examples/cube_subdiv.d
```

Expected:
```
limit verts = 98, faces = 96
limit bbox  = [-0.878, 0.878] x [-0.878, 0.878] x [-0.878, 0.878]
OK
```

## Consuming from another dub project

In the consumer's `dub.json` add (assuming sibling checkouts):

```json
"dependencies": {
    "d-opensubdiv": { "path": "../D-OpenSubdiv" }
}
```

…then `import osd.c;` and call `osdc_topology_create` / `osdc_evaluate`.

## API in one breath

```d
import osd.c;

auto t = osdc_topology_create(numCageVerts, numCageFaces,
                              faceVertCounts.ptr, faceVertIndices.ptr,
                              maxLevel);
scope (exit) osdc_topology_destroy(t);

auto limit = new float[](3 * osdc_topology_limit_vert_count(t));

// Hot path: re-call every drag frame with new cage positions.
osdc_evaluate(t, cageXyz.ptr, limit.ptr);
```

## Scope

* Catmull-Clark only (Loop / Bilinear can be exposed trivially —
  hard-coded scheme in `osd_c.cpp`).
* Uniform refinement (one stencil table, level N → level N positions).
  No feature-adaptive / Gregory patches yet.
* CPU evaluator only; OpenSubdiv's GL/CUDA/Metal backends are disabled
  in the vendored build (see `CMakeLists.txt`).

## License

The shim + bindings are Apache-2.0 (matching OpenSubdiv upstream).
OpenSubdiv itself is © Pixar Animation Studios, also Apache-2.0.
