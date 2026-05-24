# D-OpenSubdiv

D bindings for a thin C shim around Pixar's **OpenSubdiv** 3.x. Two
workflows are exposed: (1) "build topology + stencil table once, evaluate
limit positions per frame" for interactive subdivision-surface previews,
and (2) feature-adaptive **Gregory-patch** evaluation of the analytic
limit surface (position + normal) at arbitrary (u, v).

## Layout

```
include/osd_c.h            — C API (extern "C")
csrc/osd_c.cpp             — implementation, wraps Far + Osd::CpuEvaluator
source/osd/c.d             — D extern(C) bindings, 1-to-1 with osd_c.h
CMakeLists.txt             — builds libosdc.a + vendored OpenSubdiv
extern/OpenSubdiv          — git submodule, pinned to v3_7_0
examples/cube_subdiv.d     — smoke test: uniform CC stencil eval
examples/patch_eval.d      — smoke test: Gregory-patch limit eval
```

## First build

```sh
git submodule update --init --recursive
dub build
```

The `preBuildCommands-posix` hook in `dub.json` invokes CMake on the
submodule + shim before `dub` compiles the D side, so a plain
`dub build` is enough. Output: `libd-opensubdiv.a` (D bindings,
package root) + `build/libosdc.a` and the vendored OpenSubdiv libs
under `build/extern/OpenSubdiv/opensubdiv/`.

## Smoke tests

The examples are `dub` executable configurations, so `dub` runs the
CMake prebuild and links the full native lib set (`libs-posix`) for you —
no hand-rolled `rdmd -L=...` invocation:

```sh
dub run --config=cube_subdiv    # uniform Catmull-Clark stencil eval
dub run --config=patch_eval     # feature-adaptive Gregory-patch limit eval
```

`cube_subdiv` expected:
```
limit verts = 98, faces = 96
limit bbox  = [-0.878, 0.878] x [-0.878, 0.878] x [-0.878, 0.878]
OK
```

`patch_eval` evaluates a unit cube's limit surface at patch (u, v) for a
smooth and a fully-creased case and prints positions + normals, ending in
`OK`.

## Consuming from another dub project

In the consumer's `dub.json` add (assuming sibling checkouts):

```json
"dependencies": {
    "d-opensubdiv": { "path": "../D-OpenSubdiv" }
}
```

…then `import osd.c;`. dub resolves the dependency to the `library`
configuration (a static library).

## API in one breath

```d
import osd.c;

// (1) Uniform refinement + stencil table — hot per-frame eval.
auto t = osdc_topology_create(numCageVerts, numCageFaces,
                              faceVertCounts.ptr, faceVertIndices.ptr,
                              maxLevel);
scope (exit) osdc_topology_destroy(t);
auto limit = new float[](3 * osdc_topology_limit_vert_count(t));
osdc_evaluate(t, cageXyz.ptr, limit.ptr);   // re-call each drag frame

// (2) Feature-adaptive Gregory-patch limit eval (position + normal).
auto p = osdc_patch_create(numCageVerts, numCageFaces,
                           faceVertCounts.ptr, faceVertIndices.ptr,
                           0, null, null, 0, null, null, /*isolation*/ 3);
scope (exit) osdc_patch_destroy(p);
osdc_patch_refine(p, cageXyz.ptr);
float[3] pos, nrm;
osdc_patch_evaluate(p, ptexFace, u, v, pos.ptr, nrm.ptr);
```

## Scope

* Catmull-Clark only (Loop / Bilinear can be exposed trivially —
  hard-coded scheme in `osd_c.cpp`).
* Two evaluation paths:
  * Uniform refinement → one stencil table (level N → level N positions),
    run on the CPU (`osdc_evaluate`) or on the GPU via GL transform
    feedback (`osdc_gl_*`).
  * Feature-adaptive patches with Gregory-basis end-caps for analytic
    limit-surface evaluation + normals (`osdc_patch_*`, CPU). Crease /
    corner sharpness is honoured on both paths.

## License

The shim + bindings are Apache-2.0 (matching OpenSubdiv upstream).
OpenSubdiv itself is © Pixar Animation Studios, also Apache-2.0.
