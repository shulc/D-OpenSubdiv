/// Limit-surface patch evaluation smoke test.
///
/// Builds a unit cube cage, refines it with feature-adaptive patches +
/// Gregory-basis end-caps, then evaluates the analytic Catmull-Clark
/// limit surface at a few (ptex_face, u, v) samples and checks the
/// positions + normals are sane. Runs the same cube twice: once smooth
/// (no creases) and once with all 12 edges infinitely sharp (≈ the
/// hard cube). Verifies the C-shim patch API + D bindings + linker
/// plumbing line up.
///
/// Compile / run (from package root):
///   dmd -Isource examples/patch_eval.d -of=/tmp/patch_eval \
///       -L-Lbuild -L-Lbuild/extern/OpenSubdiv/opensubdiv \
///       -L-losdc -L-losdCPU -L-losdGPU -L-lstdc++ -L-lGL -L-lm
///   /tmp/patch_eval
module patch_eval;

import std.stdio : writefln, writeln;
import std.math  : abs, sqrt, isFinite;
import osd.c;

// Unit cube centred at the origin — 8 verts, 6 quad faces.
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
    -0.5f, -0.5f, -0.5f,    0.5f, -0.5f, -0.5f,
    -0.5f, -0.5f,  0.5f,    0.5f, -0.5f,  0.5f,
    -0.5f,  0.5f, -0.5f,    0.5f,  0.5f, -0.5f,
    -0.5f,  0.5f,  0.5f,    0.5f,  0.5f,  0.5f,
];

float vlen(in float[3] v) { return sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); }
bool  finite3(in float[3] v) { return isFinite(v[0]) && isFinite(v[1]) && isFinite(v[2]); }

void evalSample(osdc_patch_t* p, int f, float u, float v,
                out float[3] pos, out float[3] nrm)
{
    osdc_patch_evaluate(p, f, u, v, pos.ptr, nrm.ptr);
}

void main() {
    writeln("=== SMOOTH cube (no creases, isolation=3) ===");
    {
        auto p = osdc_patch_create(
            /*num_cage_verts =*/ 8,
            /*num_cage_faces =*/ 6,
            faceCounts.ptr, faceIndices.ptr,
            /*num_creases =*/ 0, null, null,
            /*num_corners =*/ 0, null, null,
            /*isolation_level =*/ 3);
        assert(p !is null, "osdc_patch_create (smooth) returned null");
        scope (exit) osdc_patch_destroy(p);

        int nptex = osdc_patch_ptex_face_count(p);
        writefln("ptex face count = %d (cube: 6 quads -> 6)", nptex);
        assert(nptex == 6, "expected 6 ptex faces for a 6-quad cube");

        // ptex -> base map: each quad is its own base face, 1:1.
        foreach (i; 0 .. nptex) {
            int base = osdc_patch_ptex_to_base_face(p, i);
            assert(base == i, "quad cube: ptex i should map to base i");
        }

        osdc_patch_refine(p, cageXyz.ptr);

        // Face centres (u,v) = (0.5,0.5) — the limit point of a face
        // centre of a smooth unit cube pulls in toward the origin; the
        // dominant axis sits around ~0.36 (well inside ±0.5).
        foreach (f; 0 .. nptex) {
            float[3] pos, nrm;
            evalSample(p, f, 0.5f, 0.5f, pos, nrm);
            assert(finite3(pos), "face-centre pos not finite");
            assert(finite3(nrm), "face-centre normal not finite");
            float nl = vlen(nrm);
            assert(abs(nl - 1.0f) < 1e-3f, "face-centre normal not unit");
            // Dominant axis magnitude well inside the cage half-extent.
            float dom = 0;
            foreach (c; pos) if (abs(c) > dom) dom = abs(c);
            assert(dom < 0.5f, "smooth face centre should be inside ±0.5");
            assert(dom > 0.20f, "smooth face centre unexpectedly collapsed");
            writefln("  face %d centre: pos=(%+.4f %+.4f %+.4f) |dom|=%.4f  n=(%+.4f %+.4f %+.4f)",
                     f, pos[0], pos[1], pos[2], dom, nrm[0], nrm[1], nrm[2]);
        }

        // A corner sample (u,v) = (0,0) — still finite, normal unit.
        {
            float[3] pos, nrm;
            evalSample(p, 0, 0.0f, 0.0f, pos, nrm);
            assert(finite3(pos) && finite3(nrm), "corner sample not finite");
            assert(abs(vlen(nrm) - 1.0f) < 1e-3f, "corner normal not unit");
            writefln("  face 0 corner(0,0): pos=(%+.4f %+.4f %+.4f)  n=(%+.4f %+.4f %+.4f)",
                     pos[0], pos[1], pos[2], nrm[0], nrm[1], nrm[2]);
        }
    }

    writeln("=== CREASED cube (all 12 edges weight 10 ≈ hard cube) ===");
    {
        // All 12 cube edges, each (vA,vB) pair.
        immutable int[24] creasePairs = [
            0,1, 1,3, 3,2, 2,0,   // -Y face loop
            4,5, 5,7, 7,6, 6,4,   // +Y face loop
            0,4, 1,5, 3,7, 2,6,   // verticals
        ];
        float[12] creaseW = 10.0f;

        auto p = osdc_patch_create(
            8, 6, faceCounts.ptr, faceIndices.ptr,
            /*num_creases =*/ 12, creasePairs.ptr, creaseW.ptr,
            /*num_corners =*/ 0, null, null,
            /*isolation_level =*/ 3);
        assert(p !is null, "osdc_patch_create (creased) returned null");
        scope (exit) osdc_patch_destroy(p);

        osdc_patch_refine(p, cageXyz.ptr);
        int nptex = osdc_patch_ptex_face_count(p);

        // With every edge infinitely sharp the limit surface is the
        // sharp cube: each face centre sits AT ±0.5 on its axis and its
        // normal points straight along that axis.
        foreach (f; 0 .. nptex) {
            float[3] pos, nrm;
            evalSample(p, f, 0.5f, 0.5f, pos, nrm);
            assert(finite3(pos) && finite3(nrm), "creased sample not finite");
            float nl = vlen(nrm);
            assert(abs(nl - 1.0f) < 1e-3f, "creased normal not unit");
            float dom = 0; int domAxis = 0;
            foreach (a, c; pos) if (abs(c) > dom) { dom = abs(c); domAxis = cast(int)a; }
            // Hard cube: dominant face-centre coord ≈ 0.5.
            assert(abs(dom - 0.5f) < 0.02f, "creased face centre should be ≈ ±0.5");
            // Normal aligned with the dominant axis (≈ 1 on that axis).
            assert(abs(abs(nrm[domAxis]) - 1.0f) < 0.02f, "creased normal should be axis-aligned");
            writefln("  face %d centre: pos=(%+.4f %+.4f %+.4f) |dom|=%.4f  n=(%+.4f %+.4f %+.4f)",
                     f, pos[0], pos[1], pos[2], dom, nrm[0], nrm[1], nrm[2]);
        }
    }

    writeln("OK");
}
