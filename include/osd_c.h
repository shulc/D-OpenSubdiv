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

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OSDC_H
