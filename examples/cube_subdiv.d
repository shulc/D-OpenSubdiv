/// Smoke test — build a unit cube, refine it twice with Catmull-Clark,
/// print the resulting limit vertex count and the bounding box of the
/// limit positions. Verifies the C shim + D bindings + linker plumbing
/// all line up.
///
/// Run via:  dub --root=examples --single cube_subdiv.d
///   …or, from the package root: rdmd -Isource examples/cube_subdiv.d
///                                  -Lbuild/libosdc.a -Lbuild/lib/libosd_static_cpu.a
///                                  -L-lstdc++
module cube_subdiv;

import std.stdio  : writefln, writeln;
import osd.c;

void main() {
    // Cube cage — 8 verts, 6 quads.
    immutable int[6]  faceCounts  = [4, 4, 4, 4, 4, 4];
    immutable int[24] faceIndices = [
        0, 1, 3, 2,   // -Y
        4, 6, 7, 5,   // +Y
        0, 2, 6, 4,   // -X
        1, 5, 7, 3,   // +X
        0, 4, 5, 1,   // -Z
        2, 3, 7, 6,   // +Z
    ];
    immutable float[24] cageXyz = [
        -1, -1, -1,   1, -1, -1,
        -1, -1,  1,   1, -1,  1,
        -1,  1, -1,   1,  1, -1,
        -1,  1,  1,   1,  1,  1,
    ];

    auto topo = osdc_topology_create(
        /*num_cage_verts =*/ 8,
        /*num_cage_faces =*/ 6,
        faceCounts.ptr, faceIndices.ptr,
        /*max_level      =*/ 2);
    assert(topo !is null, "osdc_topology_create returned null");
    scope (exit) osdc_topology_destroy(topo);

    int nLimitVerts  = osdc_topology_limit_vert_count(topo);
    int nLimitFaces  = osdc_topology_limit_face_count(topo);

    // Catmull-Clark at level 2 on a cube: 8 → 26 → 98 verts;
    // 6 → 24 → 96 quads. Hard-code as a regression sentinel.
    assert(nLimitVerts == 98, "expected 98 limit verts at level 2");
    assert(nLimitFaces == 96, "expected 96 limit faces at level 2");

    auto limit = new float[](3 * nLimitVerts);
    osdc_evaluate(topo, cageXyz.ptr, limit.ptr);

    // Bounding box — should fall inside the cage cube (CC contracts
    // toward the centroid).
    float minX = float.max, maxX = -float.max;
    float minY = float.max, maxY = -float.max;
    float minZ = float.max, maxZ = -float.max;
    for (size_t i = 0; i < cast(size_t)nLimitVerts; i++) {
        float x = limit[3*i+0], y = limit[3*i+1], z = limit[3*i+2];
        if (x < minX) minX = x; if (x > maxX) maxX = x;
        if (y < minY) minY = y; if (y > maxY) maxY = y;
        if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
    }
    writefln("limit verts = %d, faces = %d", nLimitVerts, nLimitFaces);
    writefln("limit bbox  = [%.3f, %.3f] x [%.3f, %.3f] x [%.3f, %.3f]",
             minX, maxX, minY, maxY, minZ, maxZ);
    writeln("OK");
}
