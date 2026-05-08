#include "pch_engine.h"
#include "../ShaderTestBench.h"
#include "../TestCommon.h"

#include <cstdio>

namespace ShaderLab::Tests
{
    void TestColorMatrices(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: Color Matrices ===\n");

        // ---- scRGB <-> XYZ ------------------------------------------------
        // scRGB(1,1,1) -> XYZ should land on the matrix's chosen D65. The
        // codebase's REC709_TO_XYZ matrix sums to (0.9504559, 1.0, 1.0890580)
        // for input (1,1,1) — slightly different from the textbook D65
        // (0.95047, 1.0, 1.08883) because the matrix is derived from the
        // exact CIE xy chromaticities rather than tabulated XYZ. Test
        // against the algebraic value, not the textbook one.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(ScRGBToXYZ(float3(1.0, 1.0, 1.0)), 0);
            )");
            TEST("ScRGBToXYZ(1,1,1) ~= (0.9505, 1.0, 1.0891) algebraic D65",
                !r.empty()
                && Near(r[0].x, 0.9504559f, 5e-4f)
                && Near(r[0].y, 1.0000000f, 1e-5f)
                && Near(r[0].z, 1.0890580f, 5e-4f));
        }
        // Inverse: XYZ(matrix-D65) -> ScRGB should land on (1,1,1).
        {
            auto r = bench.Run(R"(
                Result[0] = float4(XYZToScRGB(float3(0.9504559, 1.0, 1.0890580)), 0);
            )");
            TEST("XYZToScRGB(matrix-D65) ~= (1, 1, 1)",
                !r.empty()
                && Near(r[0].x, 1.0f, 1e-4f)
                && Near(r[0].y, 1.0f, 1e-4f)
                && Near(r[0].z, 1.0f, 1e-4f));
        }
        // ScRGB(0,0,0) -> XYZ(0,0,0).
        {
            auto r = bench.Run(R"(
                Result[0] = float4(ScRGBToXYZ(float3(0,0,0)), 0);
            )");
            TEST("ScRGBToXYZ(0) == 0",
                !r.empty()
                && Near(r[0].x, 0.0f, 1e-6f)
                && Near(r[0].y, 0.0f, 1e-6f)
                && Near(r[0].z, 0.0f, 1e-6f));
        }
        // Inverse-matrix product: XYZ_TO_REC709 * REC709_TO_XYZ ~= I.
        // Test by routing the three basis vectors through both matrices.
        {
            auto r = bench.Run(R"(
                float3 x = XYZToScRGB(ScRGBToXYZ(float3(1,0,0)));
                float3 y = XYZToScRGB(ScRGBToXYZ(float3(0,1,0)));
                float3 z = XYZToScRGB(ScRGBToXYZ(float3(0,0,1)));
                // Reduce to a single max-deviation scalar.
                float3 ex = x - float3(1,0,0);
                float3 ey = y - float3(0,1,0);
                float3 ez = z - float3(0,0,1);
                float maxErr = max(max(
                    max(max(abs(ex.x), abs(ex.y)), abs(ex.z)),
                    max(max(abs(ey.x), abs(ey.y)), abs(ey.z))),
                    max(max(abs(ez.x), abs(ez.y)), abs(ez.z)));
                Result[0] = float4(maxErr, 0, 0, 0);
            )");
            TEST("XYZ_TO_REC709 * REC709_TO_XYZ ~= I (max basis err < 1e-5)",
                !r.empty() && r[0].x < 1e-5f);
        }

        // ---- 6-color round trip through XYZ -------------------------------
        {
            auto r = bench.Run(R"(
                float3 colors[6] = {
                    float3(1, 0, 0),       // primary R
                    float3(0, 1, 0),       // primary G
                    float3(0, 0, 1),       // primary B
                    float3(0.5, 0.5, 0.5), // gray
                    float3(0.18, 0.18, 0.18), // 18% gray
                    float3(0.9, 0.7, 0.6)  // skin tone
                };
                float maxErr = 0.0;
                [unroll]
                for (int i = 0; i < 6; ++i) {
                    float3 rt = XYZToScRGB(ScRGBToXYZ(colors[i]));
                    float3 d = abs(rt - colors[i]);
                    maxErr = max(maxErr, max(max(d.x, d.y), d.z));
                }
                Result[0] = float4(maxErr, 0, 0, 0);
            )");
            TEST("ScRGB <-> XYZ round trip on 6-color suite (max err < 1e-5)",
                !r.empty() && r[0].x < 1e-5f);
        }

        // ---- scRGB <-> ICtCp ----------------------------------------------
        // Analytic anchor: scRGB(1,1,1) is 80-nit neutral white. Through
        // ScRGBToXYZ * 80 -> (76.04, 80, 87.13). XYZ_TO_LMS_ICTCP picks
        // exactly equal LMS components (~80 each, by construction). After
        // PQ_InvEOTF(80), PQLMS_TO_ICTCP first row sums to 1.0 * pq for I,
        // and the Ct/Cp rows sum to zero on neutrals. So ICtCp(white) is
        // (PQ_InvEOTF(80nits), 0, 0) ~= (0.498, 0, 0). Tolerance loose on
        // the I component since it follows PQ_InvEOTF's curve exactly.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(ScRGBToICtCp(float3(1, 1, 1)), 0);
            )");
            TEST("ScRGBToICtCp(white@80nits) ~= (PQ_InvEOTF(80), 0, 0); chroma channels neutral",
                !r.empty()
                && Near(r[0].x, 0.498f, 2e-2f)
                && Near(r[0].y, 0.0f,    1e-4f)
                && Near(r[0].z, 0.0f,    1e-4f));
        }
        // Inverse: ICtCp(0.498, 0, 0) -> scRGB white. Use the same I value
        // we expect from the forward direction so the round trip is exact.
        {
            auto r = bench.Run(R"(
                float I = ScRGBToICtCp(float3(1, 1, 1)).x;
                Result[0] = float4(ICtCpToScRGB(float3(I, 0, 0)), 0);
            )");
            TEST("ICtCpToScRGB(forward(white)) ~= (1, 1, 1) [round trip on neutral]",
                !r.empty()
                && Near(r[0].x, 1.0f, 5e-3f)
                && Near(r[0].y, 1.0f, 5e-3f)
                && Near(r[0].z, 1.0f, 5e-3f));
        }
        // ScRGB <-> ICtCp 6-color round trip. Tolerance loose: PQ encode
        // at small scRGB values has more relative quantization error.
        {
            auto r = bench.Run(R"(
                float3 colors[6] = {
                    float3(1, 0, 0),
                    float3(0, 1, 0),
                    float3(0, 0, 1),
                    float3(1, 1, 1),
                    float3(0.5, 0.5, 0.5),
                    float3(0.9, 0.7, 0.6)
                };
                float maxErr = 0.0;
                [unroll]
                for (int i = 0; i < 6; ++i) {
                    float3 rt = ICtCpToScRGB(ScRGBToICtCp(colors[i]));
                    float3 d = abs(rt - colors[i]);
                    maxErr = max(maxErr, max(max(d.x, d.y), d.z));
                }
                Result[0] = float4(maxErr, 0, 0, 0);
            )");
            TEST("ScRGB <-> ICtCp round trip on 6-color suite (max err < 5e-3)",
                !r.empty() && r[0].x < 5e-3f);
        }

        // ---- Negative scRGB protection ------------------------------------
        // ScRGBToICtCp clamps `max(rgb, 0)` before the XYZ matrix to keep
        // the LMS path well-defined. Confirm: a slightly-negative input
        // produces the same ICtCp as the all-zero input.
        {
            auto r = bench.Run(R"(
                float3 a = ScRGBToICtCp(float3(-0.1, -0.1, -0.1));
                float3 b = ScRGBToICtCp(float3( 0.0,  0.0,  0.0));
                float3 d = abs(a - b);
                float maxErr = max(max(d.x, d.y), d.z);
                Result[0] = float4(maxErr, a.x, a.y, a.z);
            )");
            TEST("ScRGBToICtCp negative-input clamp matches zero (max err < 1e-5)",
                !r.empty() && r[0].x < 1e-5f);
        }
    }
}
