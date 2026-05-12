/// D bindings for the D-OpenSubdiv C shim.
///
/// Mirrors include/osd_c.h exactly — one struct per opaque handle, one
/// extern(C) declaration per entry point, no D-side wrapping yet. Higher-
/// level convenience layers (RAII handles, slice-based eval) belong in a
/// sibling module so the raw surface stays available for callers that
/// need to mix manual lifetime management with their own arena/pool
/// schemes.
module osd.c;

extern (C) @nogc nothrow:

/// Opaque handle to a topology + cached stencil table. Created via
/// osdc_topology_create, freed via osdc_topology_destroy. All other
/// entry points are tolerant of NULL.
struct osdc_topology_t;

/// Build a topology + stencil table for `max_level` uniform Catmull-
/// Clark refinement of an n-gon polygon mesh. See osd_c.h for the
/// detailed parameter contract; returns null on invalid input.
osdc_topology_t* osdc_topology_create(int   num_cage_verts,
                                       int   num_cage_faces,
                                       const int* face_vert_counts,
                                       const int* face_vert_indices,
                                       int   max_level);

void osdc_topology_destroy(osdc_topology_t* t);

int  osdc_topology_limit_vert_count (const osdc_topology_t* t);
int  osdc_topology_limit_face_count (const osdc_topology_t* t);
int  osdc_topology_limit_index_count(const osdc_topology_t* t);

/// Copy the refined-mesh's face counts + flat vertex-index array into
/// caller-provided buffers. Pass null for `face_vert_counts_out` when
/// you only need the index stream (Catmull-Clark always produces quads
/// past level 1).
void osdc_topology_limit_topology(const osdc_topology_t* t,
                                   int* face_vert_counts_out,
                                   int* face_vert_indices_out);

/// Evaluate the limit-surface positions for the current cage. One
/// sparse matrix-vector product; safe to call many times per second.
///
///   cage_xyz — tightly-packed [x0,y0,z0, x1,y1,z1, ...],
///              length = 3 * num_cage_verts
///   out_xyz  — caller-allocated, length = 3 * osdc_topology_limit_vert_count
void osdc_evaluate(osdc_topology_t* t,
                    const float* cage_xyz,
                    float* out_xyz);
