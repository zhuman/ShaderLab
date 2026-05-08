#include "pch_engine.h"
#include "../ShaderTestBench.h"
#include "../TestCommon.h"

#include <cstdio>

namespace ShaderLab::Tests
{
    void TestGamut(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: Gamut & Encodings ===\n");

        // ---- sRGB <-> linear (piecewise gamma) ----------------------------
        {
            // Linear 1.0 -> sRGB 1.0 anchor.
            auto r = bench.Run(R"(
                Result[0] = float4(LinearToSRGB(float3(1.0, 0.5, 0.0)), 0);
            )");
            TEST("LinearToSRGB(1,0.5,0) ~= (1.0, 0.7354, 0.0) anchor",
                !r.empty()
                && Near(r[0].x, 1.0000f, 1e-4f)
                && Near(r[0].y, 0.7354f, 1e-3f)
                && Near(r[0].z, 0.0000f, 1e-4f));
        }
        {
            // sRGB 0.5 -> linear ~= 0.2140. Standard sRGB anchor.
            auto r = bench.Run(R"(
                Result[0] = float4(SRGBToLinear(float3(0.5, 0.5, 0.5)), 0);
            )");
            TEST("SRGBToLinear(0.5,0.5,0.5) ~= (0.2140) BT.709 anchor",
                !r.empty()
                && Near(r[0].x, 0.2140f, 1e-3f)
                && Near(r[0].y, 0.2140f, 1e-3f)
                && Near(r[0].z, 0.2140f, 1e-3f));
        }
        {
            // Toe behavior: tiny values use the linear segment so should
            // round-trip exactly. SRGBToLinear(0.04045) is exactly the
            // linear-segment cutoff.
            auto r = bench.Run(R"(
                float a = SRGBToLinear(float3(0.04045, 0, 0)).x;
                float b = SRGBToLinear(float3(0.0, 0, 0)).x;
                Result[0] = float4(a, b, 0, 0);
            )");
            TEST("SRGBToLinear toe: 0.04045 ~= 0.04045/12.92, 0 == 0",
                !r.empty()
                && Near(r[0].x, 0.04045f / 12.92f, 1e-6f)
                && Near(r[0].y, 0.0f, 1e-7f));
        }
        {
            // sRGB <-> linear 6-color round trip.
            auto r = bench.Run(R"(
                float3 colors[6] = {
                    float3(0.0, 0.0, 0.0),
                    float3(0.5, 0.5, 0.5),
                    float3(1.0, 1.0, 1.0),
                    float3(1.0, 0.0, 0.0),
                    float3(0.0, 1.0, 0.0),
                    float3(0.18, 0.5, 0.9)
                };
                float maxErr = 0.0;
                [unroll]
                for (int i = 0; i < 6; ++i) {
                    float3 rt = LinearToSRGB(SRGBToLinear(colors[i]));
                    float3 d = abs(rt - colors[i]);
                    maxErr = max(maxErr, max(max(d.x, d.y), d.z));
                }
                Result[0] = float4(maxErr, 0, 0, 0);
            )");
            TEST("LinearToSRGB(SRGBToLinear(x)) ~= x on 6-color suite",
                !r.empty() && r[0].x < 1e-5f);
        }

        // ---- xyY <-> XYZ --------------------------------------------------
        {
            // xyYToXYZ at D65 anchor: x=0.3127, y=0.3290, Y=1.0 -> standard
            // D65 XYZ (0.95047, 1.0, 1.08883). The codebase's D65 white
            // constant uses (0.3127, 0.3290) so test against the expected
            // X = x*Y/y = 0.95046, Z = (1-x-y)*Y/y = 1.08906 (using these
            // exact xy values; the canonical D65 XYZ uses slightly
            // different rounding so the test compares to the algebraic
            // result, not the ScRGBToXYZ constant).
            auto r = bench.Run(R"(
                Result[0] = float4(xyYToXYZ(float3(0.3127, 0.3290, 1.0)), 0);
            )");
            // Expected: X = 0.3127/0.3290 = 0.9504559; Y = 1.0; Z = (1-0.3127-0.3290)/0.3290 = 1.0890578.
            TEST("xyYToXYZ(D65 chromaticities, Y=1.0) ~= algebraic XYZ",
                !r.empty()
                && Near(r[0].x, 0.9504559f, 1e-4f)
                && Near(r[0].y, 1.0000000f, 1e-6f)
                && Near(r[0].z, 1.0890578f, 1e-4f));
        }
        {
            // Round trip on D65 + the three Rec.709 primaries (need Y > 0
            // since Y == 0 collapses xy via the y < 1e-10 branch).
            auto r = bench.Run(R"(
                float3 xyY[4] = {
                    float3(0.3127, 0.3290, 1.0),  // D65
                    float3(0.6400, 0.3300, 0.5),  // Rec.709 R
                    float3(0.3000, 0.6000, 0.5),  // Rec.709 G
                    float3(0.1500, 0.0600, 0.5)   // Rec.709 B
                };
                float maxErr = 0.0;
                [unroll]
                for (int i = 0; i < 4; ++i) {
                    float3 rt = XYZToxyY(xyYToXYZ(xyY[i]));
                    float3 d = abs(rt - xyY[i]);
                    maxErr = max(maxErr, max(max(d.x, d.y), d.z));
                }
                Result[0] = float4(maxErr, 0, 0, 0);
            )");
            TEST("xyYToXYZ <-> XYZToxyY round trip on D65 + Rec.709 primaries",
                !r.empty() && r[0].x < 1e-5f);
        }
        {
            // Y == 0 degenerate branch returns D65 chromaticity by
            // convention (the codebase guards y<1e-10 returning (0.3127, 0.3290)).
            // Pass tiny non-zero values below the 1e-10 threshold so the
            // HLSL compiler can't statically prove a divide-by-zero in
            // the not-taken branch (X4008 → X3129 warnings-as-errors).
            auto r = bench.Run(R"(
                Result[0] = float4(XYZToxyY(float3(1e-15, 1e-15, 1e-15)), 0);
            )");
            TEST("XYZToxyY(near-zero) returns D65 chromaticity fallback",
                !r.empty()
                && Near(r[0].x, 0.3127f, 1e-4f)
                && Near(r[0].y, 0.3290f, 1e-4f));
        }

        // ---- PointInTriangle (gamut tests) --------------------------------
        // D65 (0.3127, 0.3290) is inside the Rec.709 triangle.
        {
            auto r = bench.Run(R"(
                bool inside = PointInTriangle(D65_WHITE, GAMUT_709_R, GAMUT_709_G, GAMUT_709_B);
                Result[0] = float4(inside ? 1.0 : 0.0, 0, 0, 0);
            )");
            TEST("PointInTriangle: D65 is inside Rec.709",
                !r.empty() && Near(r[0].x, 1.0f, 1e-6f));
        }
        // BT.2020 red primary (0.708, 0.292) is OUTSIDE Rec.709
        // (Rec.709 R is 0.640, 0.330 — much less saturated).
        {
            auto r = bench.Run(R"(
                bool inside = PointInTriangle(GAMUT_2020_R, GAMUT_709_R, GAMUT_709_G, GAMUT_709_B);
                Result[0] = float4(inside ? 1.0 : 0.0, 0, 0, 0);
            )");
            TEST("PointInTriangle: BT.2020 red primary is OUTSIDE Rec.709",
                !r.empty() && Near(r[0].x, 0.0f, 1e-6f));
        }
        // Rec.709 R is itself inside (boundary point — barycentric u==1, v==0).
        {
            auto r = bench.Run(R"(
                bool inside = PointInTriangle(GAMUT_709_R, GAMUT_709_R, GAMUT_709_G, GAMUT_709_B);
                Result[0] = float4(inside ? 1.0 : 0.0, 0, 0, 0);
            )");
            TEST("PointInTriangle: Rec.709 R primary is on/inside Rec.709 boundary",
                !r.empty() && Near(r[0].x, 1.0f, 1e-6f));
        }

        // ---- OKLab ---------------------------------------------------------
        // OKLab(white) should land on (1, 0, 0): lightness 1, neutral chroma.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(LinearToOKLab(float3(1, 1, 1)), 0);
            )");
            TEST("LinearToOKLab(white) ~= (1, 0, 0)",
                !r.empty()
                && Near(r[0].x, 1.0f, 1e-3f)
                && Near(r[0].y, 0.0f, 1e-3f)
                && Near(r[0].z, 0.0f, 1e-3f));
        }
        // OKLab(black) should land on (0, 0, 0).
        {
            auto r = bench.Run(R"(
                Result[0] = float4(LinearToOKLab(float3(0, 0, 0)), 0);
            )");
            TEST("LinearToOKLab(black) ~= (0, 0, 0)",
                !r.empty()
                && Near(r[0].x, 0.0f, 1e-5f)
                && Near(r[0].y, 0.0f, 1e-5f)
                && Near(r[0].z, 0.0f, 1e-5f));
        }

        // ---- ScRGBToLab anchor --------------------------------------------
        // CIE Lab(scRGB white) should be approximately (100, 0, 0)
        // assuming Y_n=1.0 and the scRGB pipeline treats 1.0 as the white
        // point. (Tolerance loose because the codebase's D65_XYZ
        // constants don't match the matrix's white point exactly.)
        {
            auto r = bench.Run(R"(
                Result[0] = float4(ScRGBToLab(float3(1, 1, 1)), 0);
            )");
            TEST("ScRGBToLab(white) ~= (100, 0, 0) within Lab D65 rounding",
                !r.empty()
                && Near(r[0].x, 100.0f, 0.5f)
                && Near(r[0].y, 0.0f,   1.0f)
                && Near(r[0].z, 0.0f,   1.0f));
        }
    }
}
