// D-OpenSubdiv — C shim implementation.
//
// Wraps Far::TopologyRefiner + Far::StencilTable + Osd::CpuEvaluator in
// an opaque handle. Build is one-shot; eval is fed straight float* into
// CpuEvaluator's "explicit pointer" stencil overload (no buffer-object
// allocation per frame).

#include "osd_c.h"

#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefiner.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#include <opensubdiv/far/stencilTable.h>
#include <opensubdiv/far/stencilTableFactory.h>
#include <opensubdiv/osd/cpuEvaluator.h>
#include <opensubdiv/osd/bufferDescriptor.h>
#include <opensubdiv/osd/glXFBEvaluator.h>

// OSD's bundled glLoader resolves GL function pointers via dlsym /
// glXGetProcAddress into its own internal namespace, separate from
// whatever loader the application uses (vibe3d uses bindbc-opengl).
// Must be called once before any OSD GL entry point — otherwise the
// internal function-pointer table is all-null and the first GL call
// segfaults.
namespace OpenSubdiv {
namespace internal {
namespace GLLoader {
extern bool applicationInitializeGL();
}}}

#include <cstring>
#include <vector>

using namespace OpenSubdiv;

struct osdc_topology {
    int                       num_cage_verts;
    int                       limit_vert_count;
    Far::StencilTable const*  stencil_table;
    std::vector<int>          limit_face_counts;
    std::vector<int>          limit_face_indices;

    // Trace-back arrays — derived once during topology_create by
    // walking the parent→child face/vert/edge relations through every
    // refinement level. Same semantics as vibe3d's SubpatchTrace.
    //   *_origins[i] == -1  iff element i was introduced by subdivision
    //                       (face-point / edge-point verts, internal
    //                       edges of a refined face, no entries for
    //                       faces since CC subdivides every cage face).
    std::vector<int>          face_origins;
    std::vector<int>          vert_origins;
    std::vector<int>          limit_edge_verts;   // 2 limit-vert idx per edge
    std::vector<int>          edge_origins;
    std::vector<int>          input_edge_verts;   // 2 input-vert idx per cage edge
    std::vector<int>          input_edge_children;// level-1 vert idx per cage edge (edge-point)
};

extern "C" osdc_topology_t* osdc_topology_create_sharp(
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
    const float* corner_weights)
{
    if (num_cage_verts <= 0 || num_cage_faces <= 0 || max_level < 1)
        return nullptr;
    if (face_vert_counts == nullptr || face_vert_indices == nullptr)
        return nullptr;

    // Build the topology descriptor — OpenSubdiv reads but does not
    // retain the count/index arrays; they only need to outlive the
    // refiner factory call below.
    Far::TopologyDescriptor desc;
    desc.numVertices        = num_cage_verts;
    desc.numFaces           = num_cage_faces;
    desc.numVertsPerFace    = face_vert_counts;
    desc.vertIndicesPerFace = face_vert_indices;
    if (num_creases > 0 && crease_vert_pairs != nullptr && crease_weights != nullptr) {
        desc.numCreases               = num_creases;
        desc.creaseVertexIndexPairs   = crease_vert_pairs;
        desc.creaseWeights            = crease_weights;
    }
    if (num_corners > 0 && corner_vert_indices != nullptr && corner_weights != nullptr) {
        desc.numCorners               = num_corners;
        desc.cornerVertexIndices      = corner_vert_indices;
        desc.cornerWeights            = corner_weights;
    }

    Sdc::SchemeType scheme = Sdc::SCHEME_CATMARK;
    Sdc::Options    sdcOpts;
    // Edge-only boundary interpolation matches the default DCC behaviour
    // for "Hard Crease at Corner" + "Crease on Boundary"-style edges.
    // EDGE_AND_CORNER pins boundary corner verts to their cage
    // positions — needed by callers that feed OSD a SUBSET of a
    // larger cage and then stitch the OSD output back against
    // un-subdivided faces of the original cage (selective subdivide).
    // EDGE_ONLY would smooth boundary verts and the stitched edges
    // would no longer line up. For closed-manifold cages (the
    // common case) the option has no effect — there's no boundary.
    sdcOpts.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER);

    using Factory = Far::TopologyRefinerFactory<Far::TopologyDescriptor>;
    Far::TopologyRefiner* refiner = Factory::Create(
        desc, Factory::Options(scheme, sdcOpts));
    if (refiner == nullptr) return nullptr;

    // Uniform refinement — `fullTopologyInLastLevel` is required to
    // walk faces/vertices on the deepest level via TopologyLevel.
    Far::TopologyRefiner::UniformOptions refOpts(max_level);
    refOpts.fullTopologyInLastLevel = true;
    refiner->RefineUniform(refOpts);

    // Stencil table: cage verts → limit verts, one sparse linear map.
    // `generateIntermediateLevels=false` means we only get level
    // `max_level`'s stencils — not the intermediate levels. That's
    // exactly what we want for the "evaluate at max depth" workflow.
    Far::StencilTableFactory::Options stOpts;
    stOpts.generateOffsets             = true;
    stOpts.generateIntermediateLevels  = false;
    Far::StencilTable const* table =
        Far::StencilTableFactory::Create(*refiner, stOpts);

    osdc_topology_t* h = new osdc_topology;
    h->num_cage_verts   = num_cage_verts;
    h->limit_vert_count = table->GetNumStencils();
    h->stencil_table    = table;

    // Cache level-N topology so callers can pull index/count arrays
    // without re-walking OpenSubdiv structures. After Catmull-Clark
    // every face is a quad, but we keep counts in the API anyway so
    // future schemes (Loop on triangles) can plug in.
    Far::TopologyLevel const& top = refiner->GetLevel(max_level);
    int nf = top.GetNumFaces();
    h->limit_face_counts.reserve(nf);
    h->limit_face_indices.reserve(nf * 4);
    for (int f = 0; f < nf; ++f) {
        Far::ConstIndexArray vs = top.GetFaceVertices(f);
        h->limit_face_counts.push_back(vs.size());
        for (int i = 0; i < vs.size(); ++i)
            h->limit_face_indices.push_back(vs[i]);
    }

    // Limit-mesh edge list (2 limit-vert indices per edge).
    int ne = top.GetNumEdges();
    h->limit_edge_verts.reserve(2 * ne);
    for (int e = 0; e < ne; ++e) {
        Far::ConstIndexArray ev = top.GetEdgeVertices(e);
        h->limit_edge_verts.push_back(ev[0]);
        h->limit_edge_verts.push_back(ev[1]);
    }

    // Input-cage edge endpoints — needed for callers that want to map
    // `edge_origins[i]` (an input-edge index in OSD's enumeration) back
    // to an edge index in their own cage's edge table.
    Far::TopologyLevel const& cage = refiner->GetLevel(0);
    int neIn = cage.GetNumEdges();
    h->input_edge_verts.reserve(2 * neIn);
    for (int e = 0; e < neIn; ++e) {
        Far::ConstIndexArray ev = cage.GetEdgeVertices(e);
        h->input_edge_verts.push_back(ev[0]);
        h->input_edge_verts.push_back(ev[1]);
    }

    // Edge-point child verts at level 1 — one per input edge. GetEdgeChildVertex
    // returns the index of the vert that bisects the edge at the next level.
    // For max_level==1 this is the limit-mesh vert; deeper refinements yield an
    // intermediate-level index that callers usually don't need.
    h->input_edge_children.reserve(neIn);
    if (max_level >= 1) {
        for (int e = 0; e < neIn; ++e) {
            h->input_edge_children.push_back(cage.GetEdgeChildVertex(e));
        }
    } else {
        h->input_edge_children.resize(neIn, -1);
    }

    // ---- Trace-back arrays ----------------------------------------
    // Walk parent->child face / vert / edge relationships once per
    // level. Each pass extends the cage-origin mapping by one level;
    // after `max_level` passes the mappings point at cage indices
    // (or -1 for elements introduced by subdivision).
    //
    // Face refinement: every child face inherits its parent's cage
    // index. CC subdivides every cage face, so no -1s appear here.
    {
        std::vector<int> curr(refiner->GetLevel(0).GetNumFaces());
        for (size_t i = 0; i < curr.size(); ++i) curr[i] = (int)i;
        for (int lvl = 1; lvl <= max_level; ++lvl) {
            Far::TopologyLevel const& parent = refiner->GetLevel(lvl - 1);
            Far::TopologyLevel const& child  = refiner->GetLevel(lvl);
            std::vector<int> next(child.GetNumFaces(), -1);
            int npf = parent.GetNumFaces();
            for (int pf = 0; pf < npf; ++pf) {
                Far::ConstIndexArray ch = parent.GetFaceChildFaces(pf);
                for (int i = 0; i < ch.size(); ++i)
                    next[ch[i]] = curr[pf];
            }
            curr.swap(next);
        }
        h->face_origins = std::move(curr);
    }

    // Vert refinement: only vert-children inherit the parent vert's
    // cage index. Face-children (face points) and edge-children (edge
    // points) are new verts with no cage origin → stay at -1.
    {
        std::vector<int> curr(refiner->GetLevel(0).GetNumVertices());
        for (size_t i = 0; i < curr.size(); ++i) curr[i] = (int)i;
        for (int lvl = 1; lvl <= max_level; ++lvl) {
            Far::TopologyLevel const& parent = refiner->GetLevel(lvl - 1);
            Far::TopologyLevel const& child  = refiner->GetLevel(lvl);
            std::vector<int> next(child.GetNumVertices(), -1);
            int npv = parent.GetNumVertices();
            for (int pv = 0; pv < npv; ++pv) {
                Far::Index cv = parent.GetVertexChildVertex(pv);
                if (cv >= 0) next[cv] = curr[pv];
            }
            curr.swap(next);
        }
        h->vert_origins = std::move(curr);
    }

    // Edge refinement: only edge-children inherit the parent edge's
    // cage index. Face-children (interior edges introduced by face
    // subdivision) stay at -1.
    {
        std::vector<int> curr(refiner->GetLevel(0).GetNumEdges());
        for (size_t i = 0; i < curr.size(); ++i) curr[i] = (int)i;
        for (int lvl = 1; lvl <= max_level; ++lvl) {
            Far::TopologyLevel const& parent = refiner->GetLevel(lvl - 1);
            Far::TopologyLevel const& child  = refiner->GetLevel(lvl);
            std::vector<int> next(child.GetNumEdges(), -1);
            int npe = parent.GetNumEdges();
            for (int pe = 0; pe < npe; ++pe) {
                Far::ConstIndexArray ch = parent.GetEdgeChildEdges(pe);
                for (int i = 0; i < ch.size(); ++i)
                    next[ch[i]] = curr[pe];
            }
            curr.swap(next);
        }
        h->edge_origins = std::move(curr);
    }

    delete refiner;
    return h;
}

// Legacy entry point — no crease / corner sharpness. Forwards to the
// sharpened variant with empty arrays.
extern "C" osdc_topology_t* osdc_topology_create(
    int        num_cage_verts,
    int        num_cage_faces,
    const int* face_vert_counts,
    const int* face_vert_indices,
    int        max_level)
{
    return osdc_topology_create_sharp(
        num_cage_verts, num_cage_faces,
        face_vert_counts, face_vert_indices,
        max_level,
        0, nullptr, nullptr,
        0, nullptr, nullptr);
}

extern "C" void osdc_topology_destroy(osdc_topology_t* t) {
    if (t == nullptr) return;
    delete t->stencil_table;
    delete t;
}

extern "C" int osdc_topology_limit_vert_count(const osdc_topology_t* t) {
    return t ? t->limit_vert_count : 0;
}

extern "C" int osdc_topology_limit_face_count(const osdc_topology_t* t) {
    return t ? (int)t->limit_face_counts.size() : 0;
}

extern "C" int osdc_topology_limit_index_count(const osdc_topology_t* t) {
    return t ? (int)t->limit_face_indices.size() : 0;
}

extern "C" void osdc_topology_limit_topology(const osdc_topology_t* t,
                                              int* face_vert_counts_out,
                                              int* face_vert_indices_out)
{
    if (t == nullptr) return;
    if (face_vert_counts_out)
        std::memcpy(face_vert_counts_out, t->limit_face_counts.data(),
                    t->limit_face_counts.size() * sizeof(int));
    if (face_vert_indices_out)
        std::memcpy(face_vert_indices_out, t->limit_face_indices.data(),
                    t->limit_face_indices.size() * sizeof(int));
}

extern "C" int osdc_topology_limit_edge_count(const osdc_topology_t* t) {
    return t ? (int)(t->limit_edge_verts.size() / 2) : 0;
}

extern "C" void osdc_topology_limit_edges(const osdc_topology_t* t, int* out_verts) {
    if (t == nullptr || out_verts == nullptr) return;
    std::memcpy(out_verts, t->limit_edge_verts.data(),
                t->limit_edge_verts.size() * sizeof(int));
}

extern "C" void osdc_topology_face_origins(const osdc_topology_t* t, int* out) {
    if (t == nullptr || out == nullptr) return;
    std::memcpy(out, t->face_origins.data(),
                t->face_origins.size() * sizeof(int));
}

extern "C" void osdc_topology_vert_origins(const osdc_topology_t* t, int* out) {
    if (t == nullptr || out == nullptr) return;
    std::memcpy(out, t->vert_origins.data(),
                t->vert_origins.size() * sizeof(int));
}

extern "C" void osdc_topology_edge_origins(const osdc_topology_t* t, int* out) {
    if (t == nullptr || out == nullptr) return;
    std::memcpy(out, t->edge_origins.data(),
                t->edge_origins.size() * sizeof(int));
}

extern "C" int osdc_topology_input_edge_count(const osdc_topology_t* t) {
    return t ? (int)(t->input_edge_verts.size() / 2) : 0;
}

extern "C" void osdc_topology_input_edges(const osdc_topology_t* t, int* out_verts) {
    if (t == nullptr || out_verts == nullptr) return;
    std::memcpy(out_verts, t->input_edge_verts.data(),
                t->input_edge_verts.size() * sizeof(int));
}

extern "C" void osdc_topology_input_edge_children(const osdc_topology_t* t, int* out_verts) {
    if (t == nullptr || out_verts == nullptr) return;
    std::memcpy(out_verts, t->input_edge_children.data(),
                t->input_edge_children.size() * sizeof(int));
}

// ===========================================================================
// GL evaluator — see osd_c.h for the workflow contract.
// ===========================================================================

namespace {
// Tiny wrapper that lets OSD's templated EvalStencils accept a raw GL
// buffer name as its src / dst. The templated path only ever calls
// `BindVBO()` on the buffer object — it doesn't need a constructor,
// destructor, or any size info.
struct ExternalVBO {
    unsigned int vbo;
    unsigned int BindVBO() const { return vbo; }
};
} // namespace

struct osdc_gl_evaluator {
    OpenSubdiv::Osd::GLStencilTableTBO* table;
    OpenSubdiv::Osd::GLXFBEvaluator*    evaluator;
};

extern "C" osdc_gl_evaluator_t* osdc_gl_create(const osdc_topology_t* t) {
    if (t == nullptr || t->stencil_table == nullptr) return nullptr;

    using namespace OpenSubdiv;

    // Lazy one-time init of OSD's GL function-pointer table.
    static bool s_gl_loader_init = false;
    if (!s_gl_loader_init) {
        if (!OpenSubdiv::internal::GLLoader::applicationInitializeGL())
            return nullptr;
        s_gl_loader_init = true;
    }

    auto* table = Osd::GLStencilTableTBO::Create(t->stencil_table, nullptr);
    if (table == nullptr) return nullptr;

    Osd::BufferDescriptor srcDesc(0, 3, 3);
    Osd::BufferDescriptor dstDesc(0, 3, 3);
    auto* evaluator = Osd::GLXFBEvaluator::Create(
        srcDesc, dstDesc,
        Osd::BufferDescriptor(),  // no u-derivative output
        Osd::BufferDescriptor()); // no v-derivative output
    if (evaluator == nullptr) {
        delete table;
        return nullptr;
    }

    auto* h = new osdc_gl_evaluator;
    h->table     = table;
    h->evaluator = evaluator;
    return h;
}

extern "C" void osdc_gl_destroy(osdc_gl_evaluator_t* e) {
    if (e == nullptr) return;
    delete e->evaluator;
    delete e->table;
    delete e;
}

extern "C" int osdc_gl_evaluate(osdc_gl_evaluator_t* e,
                                 unsigned int src_vbo,
                                 unsigned int dst_vbo)
{
    if (e == nullptr || src_vbo == 0 || dst_vbo == 0) return 0;
    using namespace OpenSubdiv;
    ExternalVBO src{src_vbo};
    ExternalVBO dst{dst_vbo};
    Osd::BufferDescriptor srcDesc(0, 3, 3);
    Osd::BufferDescriptor dstDesc(0, 3, 3);
    return Osd::GLXFBEvaluator::EvalStencils(
        &src, srcDesc,
        &dst, dstDesc,
        e->table,
        e->evaluator,
        nullptr) ? 1 : 0;
}

extern "C" void osdc_evaluate(osdc_topology_t* t,
                               const float* cage_xyz,
                               float* out_xyz)
{
    if (t == nullptr || cage_xyz == nullptr || out_xyz == nullptr) return;

    // Buffer descriptors: offset=0, length=3 (xyz), stride=3 (tight).
    Osd::BufferDescriptor srcDesc(0, 3, 3);
    Osd::BufferDescriptor dstDesc(0, 3, 3);

    // OpenSubdiv 3.7's templated EvalStencils requires class buffers
    // with a BindCpuBuffer() accessor. The raw-pointer overload
    // (cpuEvaluator.h:104) takes the stencil-table sub-arrays directly,
    // so we fan them out from the cached Far::StencilTable accessors —
    // one less copy than wrapping into CpuVertexBuffer, and identical
    // throughput.
    Far::StencilTable const& tbl = *t->stencil_table;
    int n = tbl.GetNumStencils();
    if (n <= 0) return;

    Osd::CpuEvaluator::EvalStencils(
        cage_xyz, srcDesc,
        out_xyz,  dstDesc,
        &tbl.GetSizes()[0],
        &tbl.GetOffsets()[0],
        &tbl.GetControlIndices()[0],
        &tbl.GetWeights()[0],
        /*start=*/ 0,
        /*end=*/   n);
}

// ===========================================================================
// Limit-surface patch evaluator — feature-adaptive patches, Gregory-basis
// end-caps. See osd_c.h for the workflow contract.
// ===========================================================================

#include <opensubdiv/far/patchTable.h>
#include <opensubdiv/far/patchTableFactory.h>
#include <opensubdiv/far/patchMap.h>
#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/ptexIndices.h>

#include <cmath>

namespace {
// Minimal primvar element for PrimvarRefiner — 3 floats, the only two
// operations the refiner ever invokes (Clear / AddWithWeight). Stored
// densely so a std::vector<OsdVertex3> aliases a tightly-packed float
// buffer (POD, no padding).
struct OsdVertex3 {
    void Clear(void* = nullptr) { p[0] = p[1] = p[2] = 0.0f; }
    void AddWithWeight(OsdVertex3 const& src, float w) {
        p[0] += w * src.p[0];
        p[1] += w * src.p[1];
        p[2] += w * src.p[2];
    }
    float p[3];
};
} // namespace

struct osdc_patch {
    int                       num_cage_verts;
    int                       n_refined;     // refiner->GetNumVerticesTotal()
    int                       n_local;       // patch table local (Gregory) points
    Far::TopologyRefiner*     refiner;
    Far::PatchTable*          patch_table;
    Far::PatchMap*            patch_map;
    Far::PtexIndices*         ptex_indices;
    std::vector<int>          ptex_to_base;  // ptex face -> cage face id
    // Reusable control-point buffer: [0 .. n_refined) refined verts then
    // [n_refined .. n_refined+n_local) Gregory local points. 3 floats each.
    std::vector<float>        cv;
};

extern "C" osdc_patch_t* osdc_patch_create(
    int          num_cage_verts,
    int          num_cage_faces,
    const int*   face_vert_counts,
    const int*   face_vert_indices,
    int          num_creases,
    const int*   crease_vert_pairs,
    const float* crease_weights,
    int          num_corners,
    const int*   corner_vert_indices,
    const float* corner_weights,
    int          isolation_level)
{
    if (num_cage_verts <= 0 || num_cage_faces <= 0 || isolation_level < 1)
        return nullptr;
    if (face_vert_counts == nullptr || face_vert_indices == nullptr)
        return nullptr;

    // ---- Topology descriptor (same crease/corner wiring as the
    //      stencil-path factory) ------------------------------------
    Far::TopologyDescriptor desc;
    desc.numVertices        = num_cage_verts;
    desc.numFaces           = num_cage_faces;
    desc.numVertsPerFace    = face_vert_counts;
    desc.vertIndicesPerFace = face_vert_indices;
    if (num_creases > 0 && crease_vert_pairs != nullptr && crease_weights != nullptr) {
        desc.numCreases             = num_creases;
        desc.creaseVertexIndexPairs = crease_vert_pairs;
        desc.creaseWeights          = crease_weights;
    }
    if (num_corners > 0 && corner_vert_indices != nullptr && corner_weights != nullptr) {
        desc.numCorners          = num_corners;
        desc.cornerVertexIndices = corner_vert_indices;
        desc.cornerWeights       = corner_weights;
    }

    Sdc::SchemeType scheme = Sdc::SCHEME_CATMARK;
    Sdc::Options    sdcOpts;
    sdcOpts.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER);

    using Factory = Far::TopologyRefinerFactory<Far::TopologyDescriptor>;
    Far::TopologyRefiner* refiner =
        Factory::Create(desc, Factory::Options(scheme, sdcOpts));
    if (refiner == nullptr) return nullptr;

    // ---- Feature-adaptive refinement around extraordinary verts /
    //      creases. useInfSharpPatch makes inf-sharp creases produce
    //      sharp patches instead of isolating forever. -----------------
    Far::TopologyRefiner::AdaptiveOptions adOpts(isolation_level);
    adOpts.useInfSharpPatch = true;
    refiner->RefineAdaptive(adOpts);

    // ---- Patch table with Gregory-basis end-caps ------------------
    Far::PatchTableFactory::Options o;
    o.SetEndCapType(Far::PatchTableFactory::Options::ENDCAP_GREGORY_BASIS);
    o.useInfSharpPatch = true;
    Far::PatchTable* pt = Far::PatchTableFactory::Create(*refiner, o);
    if (pt == nullptr) {
        delete refiner;
        return nullptr;
    }

    Far::PatchMap*    pmap = new Far::PatchMap(*pt);
    Far::PtexIndices* ptex = new Far::PtexIndices(*refiner);

    osdc_patch_t* h = new osdc_patch;
    h->num_cage_verts = num_cage_verts;
    h->n_refined      = refiner->GetNumVerticesTotal();
    h->n_local        = pt->GetNumLocalPoints();
    h->refiner        = refiner;
    h->patch_table    = pt;
    h->patch_map      = pmap;
    h->ptex_indices   = ptex;
    h->cv.assign((size_t)3 * (h->n_refined + h->n_local), 0.0f);

    // ---- ptex face -> base (cage) face map ------------------------
    // Ptex faces are numbered by walking level-0 faces in order: a
    // quad contributes exactly 1 ptex face, an N-gon (N != 4) splits
    // into N ptex faces (one per corner). This matches OSD's own ptex
    // enumeration (PtexIndices), so FindPatch's ptex id lines up.
    {
        Far::TopologyLevel const& cage = refiner->GetLevel(0);
        int nf = cage.GetNumFaces();
        h->ptex_to_base.reserve(nf);
        for (int f = 0; f < nf; ++f) {
            int nv = cage.GetFaceVertices(f).size();
            int n  = (nv == 4) ? 1 : nv;   // quad -> 1, n-gon -> n
            for (int i = 0; i < n; ++i)
                h->ptex_to_base.push_back(f);
        }
    }

    return h;
}

extern "C" int osdc_patch_ptex_face_count(const osdc_patch_t* p) {
    return p ? (int)p->ptex_to_base.size() : 0;
}

extern "C" int osdc_patch_ptex_to_base_face(const osdc_patch_t* p, int ptex_face) {
    if (p == nullptr) return -1;
    if (ptex_face < 0 || ptex_face >= (int)p->ptex_to_base.size()) return -1;
    return p->ptex_to_base[ptex_face];
}

extern "C" void osdc_patch_refine(osdc_patch_t* p, const float* cage_xyz) {
    if (p == nullptr || cage_xyz == nullptr) return;
    if (p->cv.empty()) return;

    // Stage cage positions into the head of the cv buffer.
    OsdVertex3* verts = reinterpret_cast<OsdVertex3*>(p->cv.data());
    for (int i = 0; i < p->num_cage_verts; ++i) {
        verts[i].p[0] = cage_xyz[3*i+0];
        verts[i].p[1] = cage_xyz[3*i+1];
        verts[i].p[2] = cage_xyz[3*i+2];
    }

    // Interpolate level by level. PrimvarRefiner::Interpolate(level,
    // src, dst) reads level-(level-1) verts and writes level's verts;
    // advance src/dst pointers by each level's vertex count so the one
    // flat buffer holds every level back-to-back.
    Far::PrimvarRefiner primvar(*p->refiner);
    OsdVertex3* src = verts;
    OsdVertex3* dst = src + p->refiner->GetLevel(0).GetNumVertices();
    int maxLevel = p->refiner->GetMaxLevel();
    for (int lvl = 1; lvl <= maxLevel; ++lvl) {
        primvar.Interpolate(lvl, src, dst);
        src = dst;
        dst += p->refiner->GetLevel(lvl).GetNumVertices();
    }

    // Gregory-basis (and other) local points sit right after the
    // refined verts. ComputeLocalPointValues(src, dst) takes the full
    // refined-vert buffer as src and appends the derived local points.
    if (p->n_local > 0) {
        // ComputeLocalPointValues<T> drives a stencil table whose element
        // type T must expose Clear()/AddWithWeight() — raw float won't do
        // in OSD 3.7. Feed it our OsdVertex3 (3-float POD) so the local
        // points land contiguously after the refined verts in `cv`.
        p->patch_table->ComputeLocalPointValues(
            verts,
            verts + p->n_refined);
    }
}

extern "C" void osdc_patch_evaluate(const osdc_patch_t* p,
                                     int    ptex_face,
                                     float  u,
                                     float  v,
                                     float* out_pos3,
                                     float* out_normal3)
{
    if (out_pos3)    { out_pos3[0]    = out_pos3[1]    = out_pos3[2]    = 0.0f; }
    if (out_normal3) { out_normal3[0] = out_normal3[1] = out_normal3[2] = 0.0f; }
    if (p == nullptr || p->cv.empty()) return;

    Far::PatchMap::Handle const* handle =
        p->patch_map->FindPatch(ptex_face, (double)u, (double)v);
    if (handle == nullptr) return;

    // Plenty of headroom — Gregory-basis patches use 20 control points;
    // regular B-spline patches use 16. 24 covers everything OSD emits.
    float pWeights[24], duWeights[24], dvWeights[24];
    p->patch_table->EvaluateBasis(*handle, u, v, pWeights, duWeights, dvWeights);

    Far::ConstIndexArray cvs = p->patch_table->GetPatchVertices(*handle);
    const float* cv = p->cv.data();

    float pos[3] = {0,0,0};
    float du[3]  = {0,0,0};
    float dv[3]  = {0,0,0};
    for (int i = 0; i < cvs.size(); ++i) {
        const float* c = cv + 3 * cvs[i];
        float wp = pWeights[i], wu = duWeights[i], wv = dvWeights[i];
        pos[0] += wp * c[0]; pos[1] += wp * c[1]; pos[2] += wp * c[2];
        du[0]  += wu * c[0]; du[1]  += wu * c[1]; du[2]  += wu * c[2];
        dv[0]  += wv * c[0]; dv[1]  += wv * c[1]; dv[2]  += wv * c[2];
    }

    if (out_pos3) { out_pos3[0] = pos[0]; out_pos3[1] = pos[1]; out_pos3[2] = pos[2]; }

    if (out_normal3) {
        // normal = normalize(du x dv)
        float nx = du[1]*dv[2] - du[2]*dv[1];
        float ny = du[2]*dv[0] - du[0]*dv[2];
        float nz = du[0]*dv[1] - du[1]*dv[0];
        float len = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (len > 1e-12f) { nx /= len; ny /= len; nz /= len; }
        out_normal3[0] = nx; out_normal3[1] = ny; out_normal3[2] = nz;
    }
}

extern "C" void osdc_patch_destroy(osdc_patch_t* p) {
    if (p == nullptr) return;
    delete p->patch_table;
    delete p->patch_map;
    delete p->ptex_indices;
    delete p->refiner;
    delete p;
}
