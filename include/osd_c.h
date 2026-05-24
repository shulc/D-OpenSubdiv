// D-OpenSubdiv — C shim around Pixar's OpenSubdiv 3.x.
//
// Surface area is intentionally tiny: build a topology + stencil table
// once from a polygon mesh, then evaluate limit-surface positions per
// frame as the cage moves. Targets the "topology fixed, geometry
// animating" workflow that drives interactive subpatch previews in DCC
// apps — see api_overview.html in OpenSubdiv's docs.
//
// All entry points are extern "C" so this header binds cleanly from
// D via extern(C). No STL, no exceptions, no C++ types cross the
// boundary; the opaque handle hides everything underneath.

#ifndef OSDC_H
#define OSDC_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct osdc_topology osdc_topology_t;

// Build a topology from a polygon mesh and pre-compute a stencil table
// that maps cage vertices to subdivision-level `max_level` limit
// vertices. Polygon sizes are arbitrary (n-gons supported); the
// scheme is Catmull-Clark.
//
//   num_cage_verts       — cage vertex count
//   num_cage_faces       — cage face count
//   face_vert_counts     — verts per face, length = num_cage_faces
//   face_vert_indices    — concatenated per-face vertex indices,
//                          length = sum(face_vert_counts)
//   max_level            — subdivision depth (>= 1)
//
// Returns NULL if topology construction fails (degenerate input).
osdc_topology_t* osdc_topology_create(int num_cage_verts,
                                       int num_cage_faces,
                                       const int* face_vert_counts,
                                       const int* face_vert_indices,
                                       int max_level);

// Same as osdc_topology_create, with optional crease/corner sharpness
// arrays applied to OSD's TopologyDescriptor. Pass num_creases=0 /
// num_corners=0 to skip either set.
//
//   num_creases               — number of sharpened edges
//   crease_vert_pairs         — 2 * num_creases ints; each pair
//                               (vA, vB) names a cage edge to crease.
//                               Pass weights >= 10 for "infinitely
//                               sharp" (OSD's Sdc::Crease
//                               SHARPNESS_INFINITE).
//   crease_weights            — num_creases floats
//   num_corners               — number of sharpened verts
//   corner_vert_indices       — num_corners ints
//   corner_weights            — num_corners floats
//
// Used by selective-subpatch callers to "lock" unmarked regions into
// flat polygons while marked interiors smooth normally — set every
// edge / vert touching an unmarked face to SHARPNESS_INFINITE.
osdc_topology_t* osdc_topology_create_sharp(
    int          num_cage_verts,
    int          num_cage_faces,
    const int*   face_vert_counts,
    const int*   face_vert_indices,
    int          max_level,
    int          num_creases,
    const int*   crease_vert_pairs,
    const float* crease_weights,
    int          num_corners,
    const int*   corner_vert_indices,
    const float* corner_weights);

void osdc_topology_destroy(osdc_topology_t* t);

// Counts for the refined (limit) mesh. Use to size buffers passed
// to osdc_topology_limit_topology() and osdc_evaluate().
int  osdc_topology_limit_vert_count(const osdc_topology_t* t);
int  osdc_topology_limit_face_count(const osdc_topology_t* t);
int  osdc_topology_limit_index_count(const osdc_topology_t* t);

// Copy out the refined mesh's face topology. After Catmull-Clark all
// faces are quads, so `face_vert_counts_out[i] == 4` for every i —
// callers that know this can skip the counts pointer (pass NULL).
//
//   face_vert_counts_out  — length osdc_topology_limit_face_count(),
//                           or NULL
//   face_vert_indices_out — length osdc_topology_limit_index_count()
void osdc_topology_limit_topology(const osdc_topology_t* t,
                                   int* face_vert_counts_out,
                                   int* face_vert_indices_out);

// Number of edges in the refined (limit) mesh.
int  osdc_topology_limit_edge_count(const osdc_topology_t* t);

// Copy out the refined-mesh edge list. Each edge contributes two
// limit-vertex indices to `out_verts` (length 2 * limit_edge_count).
void osdc_topology_limit_edges(const osdc_topology_t* t, int* out_verts);

// Trace-back arrays — for each limit element, the index of the cage
// element it descended from. -1 means the element was introduced by
// subdivision and has no direct counterpart on the cage. All three
// match the semantics of vibe3d's SubpatchTrace.{face,vert,edge}Origin.
//
//   out_face_origins — length osdc_topology_limit_face_count()
//   out_vert_origins — length osdc_topology_limit_vert_count()
//   out_edge_origins — length osdc_topology_limit_edge_count()
void osdc_topology_face_origins(const osdc_topology_t* t, int* out_face_origins);
void osdc_topology_vert_origins(const osdc_topology_t* t, int* out_vert_origins);
void osdc_topology_edge_origins(const osdc_topology_t* t, int* out_edge_origins);

// Number of edges in the INPUT topology (level 0 — the cage that was
// passed to osdc_topology_create). OpenSubdiv infers cage edges from
// the face-vertex list; callers that only know the face list don't
// otherwise have access to OSD's edge enumeration order, which is
// what `osdc_topology_edge_origins` indexes against.
int  osdc_topology_input_edge_count(const osdc_topology_t* t);

// Input-edge endpoint pairs — 2 input-vert indices per input edge,
// tightly packed. Output length = 2 * osdc_topology_input_edge_count.
// Use this to map an `edge_origins[i]` value (an input-edge index) to
// a pair of input verts, then look those up in your own cage's edge
// table if you need a cage-edge index.
void osdc_topology_input_edges(const osdc_topology_t* t, int* out_verts);

// For each input (level-0) edge, the index of its edge-point at the
// FIRST refinement level. Output length = osdc_topology_input_edge_count.
// At max_level=1 these indices ARE the limit-mesh vert indices (the
// only level OSD produced). For max_level>1 they're intermediate-
// level indices and most callers don't need them.
//
// Used by selective-refinement callers (subdivide-selected) to insert
// edge-point verts into adjacent un-refined faces — preserves a
// manifold result without T-junctions across the boundary between
// the refined subset and the rest of the mesh.
void osdc_topology_input_edge_children(const osdc_topology_t* t, int* out_verts);

// Apply the cached stencil table to `cage_xyz` and write limit-surface
// vertex positions into `out_xyz`. Both buffers are tightly packed
// triples (xyz, xyz, ...). One sparse SpMV under the hood — this is
// the hot per-frame call.
//
//   cage_xyz  — length 3 * num_cage_verts
//   out_xyz   — length 3 * osdc_topology_limit_vert_count()
void osdc_evaluate(osdc_topology_t* t,
                    const float* cage_xyz,
                    float* out_xyz);

// ===========================================================================
// GPU evaluator — runs the stencil table on the GPU via OpenGL transform
// feedback. Same math as `osdc_evaluate`, but limit positions land in a
// GL buffer object (VBO) directly without a CPU round-trip.
//
// Caller responsibilities:
//   * Active GL 3.3+ context on the calling thread for every GL entry
//     point. The evaluator compiles a GLSL transform-feedback shader at
//     osdc_gl_create time.
//   * Source VBO holds tightly-packed XYZ floats for every cage vert
//     (length = 3 * num_cage_verts floats). Caller fills it with the
//     current cage positions per frame via glBufferSubData / glMapBuffer.
//   * Destination VBO must have at least 3 * limit_vert_count floats of
//     space. The evaluator overwrites those bytes via transform feedback.
//
// Typical workflow:
//   topo = osdc_topology_create(...);
//   gl   = osdc_gl_create(topo);
//   // per frame:
//   glBufferSubData(GL_ARRAY_BUFFER, 0, ..., cage_xyz);
//   osdc_gl_evaluate(gl, cage_vbo, limit_vbo);
// ===========================================================================
typedef struct osdc_gl_evaluator osdc_gl_evaluator_t;

// Compile the GLSL kernel + upload the stencil table to GL texture-buffer
// objects. Returns NULL on shader-compile failure (caller should fall
// back to the CPU evaluator).
osdc_gl_evaluator_t* osdc_gl_create(const osdc_topology_t* t);

void osdc_gl_destroy(osdc_gl_evaluator_t* e);

// Run the stencil eval on GPU. `src_vbo` reads cage positions (3 floats
// per vert), `dst_vbo` receives limit positions (3 floats per stencil).
// Returns non-zero on success.
int  osdc_gl_evaluate(osdc_gl_evaluator_t* e,
                       unsigned int src_vbo,
                       unsigned int dst_vbo);

// ===========================================================================
// LIMIT-SURFACE patch evaluator — feature-adaptive patches with Gregory-
// basis end-caps. Unlike the stencil path above (which produces a finite
// set of refined VERTICES), this evaluates the analytic Catmull-Clark
// LIMIT SURFACE at arbitrary (u,v) parameters on each ptex face. That's
// what a smooth, controllable-density tessellator needs: pick any
// sampling grid you like over the (ptex_face, u, v) domain and read back
// exact limit positions + normals.
//
// Workflow:
//   patch = osdc_patch_create(...);        // once — builds adaptive patch table
//   // per geometry change:
//   osdc_patch_refine(patch, cage_xyz);    // refines control points + Gregory local points
//   // for each tessellation sample:
//   osdc_patch_evaluate(patch, f, u, v, pos, nrm);
//   osdc_patch_destroy(patch);
//
// The tessellation domain is the set of "ptex faces": each quad cage
// face is one ptex face with (u,v) in [0,1]^2; an N-gon cage face splits
// into N ptex faces (one per corner). osdc_patch_ptex_to_base_face maps
// a ptex face back to the cage face it came from.
// ===========================================================================
typedef struct osdc_patch osdc_patch_t;

// Build a feature-adaptive patch table (Gregory-basis end-caps,
// inf-sharp patches enabled) from a polygon cage with optional
// crease / corner sharpness. `isolation_level` is the adaptive
// isolation depth around extraordinary vertices and creases (>= 1;
// 3 is a good default). Returns NULL on failure.
//
//   crease_vert_pairs   — 2 * num_creases ints, each (vA,vB) an edge
//   crease_weights      — num_creases floats (>= 10 ≈ infinitely sharp)
//   corner_vert_indices — num_corners ints
//   corner_weights      — num_corners floats
osdc_patch_t* osdc_patch_create(int          num_cage_verts,
                                 int          num_cage_faces,
                                 const int*   face_vert_counts,
                                 const int*   face_vert_indices,
                                 int          num_creases,
                                 const int*   crease_vert_pairs,
                                 const float* crease_weights,
                                 int          num_corners,
                                 const int*   corner_vert_indices,
                                 const float* corner_weights,
                                 int          isolation_level);

// Number of ptex faces — the size of the tessellation domain. Each is
// a unit (u,v) square. Iterate [0, count) as the outer tessellation
// loop.
int  osdc_patch_ptex_face_count(const osdc_patch_t* p);

// Base (cage) face index that the given ptex face belongs to. A quad
// cage face owns exactly one ptex face; an N-gon owns N consecutive
// ptex faces (its corners). Returns -1 if ptex_face is out of range.
int  osdc_patch_ptex_to_base_face(const osdc_patch_t* p, int ptex_face);

// Refresh the evaluator's control-point buffer for new cage positions.
// Copies the `num_cage_verts` cage positions in, interpolates them to
// every adaptive refinement level, then computes the Gregory-basis
// local points. MUST be called once per geometry change before any
// osdc_patch_evaluate.
//
//   cage_xyz — tightly-packed [x0,y0,z0, ...], length 3 * num_cage_verts
void osdc_patch_refine(osdc_patch_t* p, const float* cage_xyz);

// Evaluate the limit surface at (ptex_face, u, v). Writes the position
// to out_pos3 and the unit surface normal to out_normal3 (either may be
// NULL). On an invalid patch handle, writes zeros. u,v are in [0,1].
void osdc_patch_evaluate(const osdc_patch_t* p,
                          int    ptex_face,
                          float  u,
                          float  v,
                          float* out_pos3,
                          float* out_normal3);

void osdc_patch_destroy(osdc_patch_t* p);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OSDC_H
