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

#include <cstring>
#include <vector>

using namespace OpenSubdiv;

struct osdc_topology {
    int                       num_cage_verts;
    int                       limit_vert_count;
    Far::StencilTable const*  stencil_table;
    std::vector<int>          limit_face_counts;
    std::vector<int>          limit_face_indices;
};

extern "C" osdc_topology_t* osdc_topology_create(
    int        num_cage_verts,
    int        num_cage_faces,
    const int* face_vert_counts,
    const int* face_vert_indices,
    int        max_level)
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

    Sdc::SchemeType scheme = Sdc::SCHEME_CATMARK;
    Sdc::Options    sdcOpts;
    // Edge-only boundary interpolation matches the default DCC behaviour
    // for "Hard Crease at Corner" + "Crease on Boundary"-style edges.
    sdcOpts.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);

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

    delete refiner;
    return h;
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
