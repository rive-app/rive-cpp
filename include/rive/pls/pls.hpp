/*
 * Copyright 2022 Rive
 */

#pragma once

#include "rive/math/aabb.hpp"
#include "rive/math/mat2d.hpp"
#include "rive/math/path_types.hpp"
#include "rive/math/simd.hpp"
#include "rive/math/vec2d.hpp"
#include "rive/shapes/paint/color.hpp"
#include "rive/shapes/paint/stroke_join.hpp"

namespace rive
{
class GrInnerFanTriangulator;
}

// This header defines constants and data structures for Rive's pixel local storage path rendering
// algorithm.
//
// Main algorithm:
// https://docs.google.com/document/d/19Uk9eyFxav6dNSYsI2ZyiX9zHU1YOaJsMB2sdDFVz6s/edit
//
// Batching multiple unique paths:
// https://docs.google.com/document/d/1DLrQimS5pbNaJJ2sAW5oSOsH6_glwDPo73-mtG5_zns/edit
//
// Batching strokes as well:
// https://docs.google.com/document/d/1CRKihkFjbd1bwT08ErMCP4fwSR7D4gnHvgdw_esY9GM/edit
namespace rive::pls
{
// Tessellate in parametric space until each segment is within 1/4 pixel of the true curve.
constexpr static int kParametricPrecision = 4;

// Tessellate in polar space until the outset edge is within 1/8 pixel of the true stroke.
constexpr static int kPolarPrecision = 8;

// Maximum supported numbers of tessellated segments in a single curve.
constexpr static uint32_t kMaxParametricSegments = 1023;
constexpr static uint32_t kMaxPolarSegments = 1023;

// We allocate all our GPU buffers in rings. This ensures the CPU can prepare frames in parallel
// while the GPU renders them.
constexpr static int kBufferRingSize = 3;

// Every coverage value in pixel local storage has an associated 16-bit path ID. This ID enables us
// to batch multiple paths together without having to clear the coverage buffer in between. This ID
// is implemented as an fp16, so the maximum path ID therefore cannot be NaN (or conservatively, all
// 5 exponent bits cannot be 1's). We also skip denormalized values (exp == 0) because they have
// been empirically unreliable on Android as ID values.
constexpr static int kLargestFP16BeforeExponentAll1s = (0x1f << 10) - 1;
constexpr static int kLargestDenormalizedFP16 = 1023;
constexpr static int MaxPathID(int granularity)
{
    // Floating point equality gets funky when the exponent bits are all 1's, so the largest pathID
    // we can support is kLargestFP16BeforeExponentAll1s.
    //
    // The shader converts an integer path ID to fp16 as:
    //
    //     (id + kLargestDenormalizedFP16) * granularity
    //
    // So the largest path ID we can support is as follows.
    return kLargestFP16BeforeExponentAll1s / granularity - kLargestDenormalizedFP16;
}

// In order to support WebGL2, we implement the path data buffer as a texture.
constexpr static size_t kPathTextureWidthInItems = 128;
constexpr static size_t kPathTexelsPerItem = 3;
constexpr static size_t kPathTextureWidthInTexels = kPathTextureWidthInItems * kPathTexelsPerItem;

// Each contour has its own unique ID, which it uses to index a data record containing per-contour
// information. This value is currently 16 bit.
constexpr static size_t kMaxContourID = 65535;
constexpr static uint32_t kContourIDMask = 0xffff;
static_assert((kMaxContourID & kContourIDMask) == kMaxContourID);

// In order to support WebGL2, we implement the contour data buffer as a texture.
constexpr static size_t kContourTextureWidthInItems = 256;
constexpr static size_t kContourTexelsPerItem = 1;
constexpr static size_t kContourTextureWidthInTexels =
    kContourTextureWidthInItems * kContourTexelsPerItem;

// Tessellation is performed by rendering vertices into a data texture. These values define the
// dimensions of the tessellation data texture.
constexpr static size_t kTessTextureWidth = 2048; // GL_MAX_TEXTURE_SIZE spec minimum on ES3/WebGL2.
constexpr static size_t kTessTextureWidthLog2 = 11;
static_assert(1 << kTessTextureWidthLog2 == kTessTextureWidth);

// Gradients are implemented by sampling a horizontal ramp of pixels allocated in a global gradient
// texture.
constexpr size_t kGradTextureWidth = 512;
constexpr size_t kGradTextureWidthInSimpleRamps = kGradTextureWidth / 2;

// Flags for on-GPU rendering data.
namespace flags
{
// Tells shaders that a cubic should actually be drawn as the single, non-AA triangle: [p0, p1, p3].
// This is used to squeeze in more rare triangles, like "grout" triangles from self intersections on
// interior triangulation, where it wouldn't be worth it to put them in their own dedicated draw
// call.
constexpr static uint32_t kRetrofittedTriangle = 1u << 31;

// Tells the tessellation shader to re-run Wang's formula on the given curve, figure out how many
// segments it actually needs, and make any excess segments degenerate by co-locating their vertices
// at T=0. (Used on the "outerCurve" patches that are drawn with interior triangulations.)
constexpr static uint32_t kCullExcessTessellationSegments = 1u << 30;

// Flags for specifying the join type.
constexpr static uint32_t kJoinTypeMask = 3u << 28;
constexpr static uint32_t kMiterClipJoin = 3u << 28;   // Miter that clips when too sharp.
constexpr static uint32_t kMiterRevertJoin = 2u << 28; // Standard miter that pops when too sharp.
constexpr static uint32_t kBevelJoin = 1u << 28;

// When a join is being used to emulate a stroke cap, the shader emits additional vertices at T=0
// and T=1 for round joins, and changes the miter limit to 1 for miter-clip joins.
constexpr static uint32_t kEmulatedStrokeCap = 1u << 27;

RIVE_ALWAYS_INLINE static uint32_t JoinTypeFlags(StrokeJoin join)
{
    switch (join)
    {
        case StrokeJoin::miter:
            return flags::kMiterRevertJoin;
        case StrokeJoin::round:
            return 0;
        case StrokeJoin::bevel:
            return flags::kBevelJoin;
    }
    RIVE_UNREACHABLE();
}

// Tells the GPU that a given path has an even-odd fill rule.
constexpr static uint32_t kEvenOdd = 1u << 31;

// Tells the GPU that a given paint is a gradient.
constexpr static uint32_t kGradient = 1u << 30;

// Tells the GPU that a given gradient is a radial gradient.
constexpr static uint32_t kRadialGradient = 1u << 29;

// Says which part of the patch a vertex belongs to.
constexpr static int32_t kStrokeVertex = 0;
constexpr static int32_t kFanVertex = 1;
constexpr static int32_t kFanMidpointVertex = 2;
} // namespace flags

// Index of each pixel local storage plane.
constexpr static int kFramebufferPlaneIdx = 0;
constexpr static int kCoveragePlaneIdx = 1;
constexpr static int kOriginalDstColorPlaneIdx = 2;
constexpr static int kClipPlaneIdx = 3;

// Index at which we access each texture.
constexpr static int kTessVertexTextureIdx = 0;
constexpr static int kPathTextureIdx = 1;
constexpr static int kContourTextureIdx = 2;
constexpr static int kGradTextureIdx = 3;

// Backend-specific capabilities/workarounds and fine tuning.
struct PlatformFeatures
{
    uint8_t pathIDGranularity = 1; // Workaround for precision issues. Determines how far apart we
                                   // space unique path IDs.
    bool avoidFlatVaryings = false;
    bool invertOffscreenY = false; // Invert Y when drawing to offscreen render targets? (Gradient
                                   // and tessellation textures.)
};

// Per-flush shared uniforms used by all shaders.
struct FlushUniforms
{
    FlushUniforms(size_t complexGradientsHeight,
                  size_t tessDataHeight,
                  size_t renderTargetWidth,
                  size_t renderTargetHeight,
                  size_t gradTextureHeight,
                  const PlatformFeatures& platformFeatures) :
        inverseViewports(
            (platformFeatures.invertOffscreenY ? float4{-2.f, -2.f, 2.f, 2.f} : float4(2.f)) /
            float4{static_cast<float>(complexGradientsHeight),
                   static_cast<float>(tessDataHeight),
                   static_cast<float>(renderTargetWidth),
                   static_cast<float>(renderTargetHeight)}),
        gradTextureInverseHeight(1.f / static_cast<float>(gradTextureHeight)),
        pathIDGranularity(platformFeatures.pathIDGranularity)
    {}
    float4 inverseViewports; // [complexGradientsY, tessDataY, renderTargetX, renderTargetY]
    float gradTextureInverseHeight;
    uint32_t pathIDGranularity; // Spacing between adjacent path IDs (1 if IEEE compliant).
    float vertexDiscardValue = std::numeric_limits<float>::quiet_NaN();
    uint32_t pad = 0;
};
static_assert(sizeof(FlushUniforms) == 8 * sizeof(uint32_t));

// Gradient color stops are implemented as a horizontal span of pixels in a global gradient texture.
// They are rendered by "GradientSpan" instances.
struct GradientSpan
{
    // x0Fixed and x1Fixed are normalized texel x coordinates, in the fixed-point range 0..65535.
    RIVE_ALWAYS_INLINE void set(uint32_t x0Fixed,
                                uint32_t x1Fixed,
                                float y_,
                                ColorInt color0_,
                                ColorInt color1_)
    {
        assert(x0Fixed < 65536);
        assert(x1Fixed < 65536);
        horizontalSpan = (x1Fixed << 16) | x0Fixed;
        y = y_;
        color0 = color0_;
        color1 = color1_;
    }
    uint32_t horizontalSpan;
    uint32_t y;
    uint32_t color0;
    uint32_t color1;
};

enum class PaintType : uint32_t
{
    solidColor,
    linearGradient,
    radialGradient,
    clipReplace // Replace the clip buffer with path coverage instead of painting color.
};

// Mirrors rive::BlendMode, but 0-based and contiguous for tighter packing.
enum class PLSBlendMode : uint32_t
{
    // Tier 1.
    srcOver,

    // Tier 2.
    screen,
    overlay,
    darken,
    lighten,
    colorDodge,
    colorBurn,
    hardLight,
    softLight,
    difference,
    exclusion,
    multiply,

    // Tier 3.
    hue,
    saturation,
    color,
    luminosity,
};

// Packs the data for a gradient or solid color into 4 floats.
struct PaintData
{
    void setColor(ColorInt color) { UnpackColorToRGBA32F(color, data); }
    void setGradient(uint32_t row, uint32_t left, uint32_t right, const float coeffs[3])
    {
        static_assert(kGradTextureWidth <= (1 << 10));
        assert(row < (1 << 12));
        // Subtract 1 from right so we can support a 1024-wide gradient texture (so the rightmost
        // pixel would be 0x3ff and still fit in 10 bits).
        uint32_t span = (row << 20) | ((right - 1) << 10) | left;
        RIVE_INLINE_MEMCPY(data, &span, sizeof(float));
        RIVE_INLINE_MEMCPY(data + 1, coeffs, 3 * sizeof(float));
    }
    float data[4]; // Packed, type-specific paint data.
};

// Each path has a unique data record on the GPU that is accessed from the vertex shader.
struct PathData
{
    RIVE_ALWAYS_INLINE void set(const Mat2D& m,
                                float strokeRadius_, // 0 if the path is filled.
                                FillRule fillRule,
                                PaintType paintType,
                                uint32_t clipID,
                                PLSBlendMode blendMode,
                                const PaintData& paintData_)
    {
        matrix = m;
        strokeRadius = strokeRadius_;
        uint32_t localParams = static_cast<uint32_t>(blendMode);
        localParams |= clipID << 4;
        localParams |= static_cast<uint32_t>(paintType) << 20;
        if (fillRule == FillRule::evenOdd && strokeRadius_ == 0)
        {
            localParams |= flags::kEvenOdd;
        }
        params = localParams;
        paintData = paintData_;
    }
    Mat2D matrix;
    float strokeRadius; // "0" indicates that the path is filled, not stroked.
    uint32_t params;    // [fillRule, paintType, clipID, blendMode]
    PaintData paintData;
};

// Each contour of every path has a unique data record on the GPU that is accessed from the vertex
// shader.
struct ContourData
{
    Vec2D midpoint;        // Midpoint of the curve endpoints in just this contour.
    uint32_t pathID;       // ID of the path this contour belongs to.
    uint32_t vertexIndex0; // Index of the first tessellation vertex of the contour.
};

// Each curve gets tessellated into vertices. This is performed by rendering a horizontal span of
// positions and normals into the tessellation data texture, GP-GPU style. TessVertexSpan defines
// one instance of a horizontal tessellation span for rendering.
//
// Each span has an optional reflection, rendered right to left, with the same vertices in reverse
// order. These are used to draw mirrored patches with negative coverage when we have back-face
// culling enabled. This emits every triangle twice, once clockwise and once counterclockwise, and
// back-face culling naturally selects the triangle with the appropriately signed coverage
// (discarding the other).
struct TessVertexSpan
{
    RIVE_ALWAYS_INLINE void set(const Vec2D pts_[4],
                                Vec2D joinTangent_,
                                float y_,
                                int32_t x0,
                                int32_t x1,
                                uint32_t parametricSegmentCount,
                                uint32_t polarSegmentCount,
                                uint32_t joinSegmentCount,
                                uint32_t contourIDWithFlags_)
    {
        set(pts_,
            joinTangent_,
            y_,
            x0,
            x1,
            std::numeric_limits<float>::quiet_NaN(), // Discard the reflection.
            -1,
            -1,
            parametricSegmentCount,
            polarSegmentCount,
            joinSegmentCount,
            contourIDWithFlags_);
    }

    RIVE_ALWAYS_INLINE void set(const Vec2D pts_[4],
                                Vec2D joinTangent_,
                                float y_,
                                int32_t x0,
                                int32_t x1,
                                float reflectionY_,
                                int32_t reflectionX0,
                                int32_t reflectionX1,
                                uint32_t parametricSegmentCount,
                                uint32_t polarSegmentCount,
                                uint32_t joinSegmentCount,
                                uint32_t contourIDWithFlags_)
    {
        RIVE_INLINE_MEMCPY(pts, pts_, sizeof(pts));
        joinTangent = joinTangent_;
        y = y_;
        reflectionY = reflectionY_;
        x0x1 = (x1 << 16) | (x0 & 0xffff);
        reflectionX0X1 = (reflectionX1 << 16) | (reflectionX0 & 0xffff);
        segmentCounts =
            (joinSegmentCount << 20) | (polarSegmentCount << 10) | parametricSegmentCount;
        contourIDWithFlags = contourIDWithFlags_;

        // Ensure we didn't lose any data from packing.
        assert(x0 == x0x1 << 16 >> 16);
        assert(x1 == x0x1 >> 16);
        assert(reflectionX0 == reflectionX0X1 << 16 >> 16);
        assert(reflectionX1 == reflectionX0X1 >> 16);
        assert((segmentCounts & 0x3ff) == parametricSegmentCount);
        assert(((segmentCounts >> 10) & 0x3ff) == polarSegmentCount);
        assert(segmentCounts >> 20 == joinSegmentCount);
    }

    Vec2D pts[4];      // Cubic bezier curve.
    Vec2D joinTangent; // Ending tangent of the join that follows the cubic.
    float y;
    float reflectionY;
    int32_t x0x1;
    int32_t reflectionX0X1;
    uint32_t segmentCounts;      // [joinSegmentCount, polarSegmentCount, parametricSegmentCount]
    uint32_t contourIDWithFlags; // flags | contourID
};
static_assert(sizeof(TessVertexSpan) == sizeof(float) * 16);

// Per-vertex data for shaders that draw triangles.
struct TriangleVertex
{
    TriangleVertex() = default;
    TriangleVertex(Vec2D point_, int16_t weight, uint16_t pathID) :
        point(point_), weight_pathID((static_cast<int32_t>(weight) << 16) | pathID)
    {}
    Vec2D point;
    int32_t weight_pathID; // [(weight << 16 | pathID]
};
static_assert(sizeof(TriangleVertex) == sizeof(float) * 3);

template <typename T> class WriteOnlyMappedMemory
{
public:
    WriteOnlyMappedMemory() { reset(); }
    WriteOnlyMappedMemory(void* ptr, size_t count) { reset(ptr, count); }

    void reset() { reset(nullptr, 0); }

    void reset(void* ptr, size_t count)
    {
        m_mappedMemory = reinterpret_cast<T*>(ptr);
        m_nextMappedItem = m_mappedMemory;
        m_mappingEnd = m_mappedMemory + count;
    }

    operator bool() const { return m_mappedMemory; }

    // How many bytes have been written to the buffer?
    size_t bytesWritten() const
    {
        return reinterpret_cast<uintptr_t>(m_nextMappedItem) -
               reinterpret_cast<uintptr_t>(m_mappedMemory);
    }

    // Is there room to push() itemCount items to the buffer?
    bool hasRoomFor(size_t itemCount) { return m_nextMappedItem + itemCount <= m_mappingEnd; }

    // Append and write a new item to the buffer. In order to enforce the write-only requirement of
    // a mapped buffer, these methods do not return any pointers to the client.
    template <typename... Args> RIVE_ALWAYS_INLINE void emplace_back(Args&&... args)
    {
        push() = {std::forward<Args>(args)...};
    }
    template <typename... Args> RIVE_ALWAYS_INLINE void set_back(Args&&... args)
    {
        push().set(std::forward<Args>(args)...);
    }

private:
    template <typename... Args> RIVE_ALWAYS_INLINE T& push()
    {
        assert(hasRoomFor(1));
        return *m_nextMappedItem++;
    }

    T* m_mappedMemory;
    T* m_nextMappedItem;
    const T* m_mappingEnd;
};

// Once all curves in a contour have been tessellated, we render the tessellated vertices in
// "patches" (aka specific instanced geometry).
//
// See:
// https://docs.google.com/document/d/19Uk9eyFxav6dNSYsI2ZyiX9zHU1YOaJsMB2sdDFVz6s/edit#heading=h.fa4kubk3vimk
//
// With strokes:
// https://docs.google.com/document/d/1CRKihkFjbd1bwT08ErMCP4fwSR7D4gnHvgdw_esY9GM/edit#heading=h.dcd0c58pxfs5
//
// A single patch spans N tessellation segments, connecting N + 1 tessellation vertices. It is
// composed of a an AA border and fan triangles. The specifics of the fan triangles depend on the
// PatchType.
enum class PatchType
{
    // Patches fan around the contour midpoint. Outer edges are inset by ~1px, followed by a ~1px AA
    // ramp.
    midpointFan,

    // Patches only cover the AA ramps and interiors of bezier curves. The interior path triangles
    // that connect the outer curves are triangulated on the CPU to eliminate overlap, and are drawn
    // in a separate call. AA ramps are split down the middle (on the same lines as the interior
    // triangulation), and drawn with a ~1/2px outset AA ramp and a ~1/2px inset AA ramp that
    // overlaps the inner tessellation and has negative coverage. A lone bowtie join is emitted at
    // the end of the patch to tie the outer curves together.
    outerCurves,
};

struct PatchVertex
{
    void set(float localVertexID_, float outset_, float fillCoverage_, float params_)
    {
        localVertexID = localVertexID_;
        outset = outset_;
        fillCoverage = fillCoverage_;
        params = params_;
        setMirroredPosition(localVertexID_, outset_, fillCoverage_);
    }

    // Patch vertices can have an optional, alternate position when mirrored. This is so we can
    // ensure the diagonals inside the stroke line up on both versions of the patch (mirrored and
    // not).
    void setMirroredPosition(float localVertexID_, float outset_, float fillCoverage_)
    {
        mirroredVertexID = localVertexID_;
        mirroredOutset = outset_;
        mirroredFillCoverage = fillCoverage_;
    }

    float localVertexID; // 0 or 1 -- which tessellated vertex of the two that we are connecting?
    float outset;        // Outset from the tessellated position, in the direction of the normal.
    float fillCoverage;  // 0..1 for the stroke. 1 all around for the triangles.
                         // (Coverage will be negated later for counterclockwise triangles.)
    int32_t params;      // "(patchSize << 2) | [flags::kStrokeVertex,
                         //                      flags::kFanVertex,
                         //                      flags::kFanMidpointVertex]"
    float mirroredVertexID;
    float mirroredOutset;
    float mirroredFillCoverage;
    int32_t pad = 0;
};
static_assert(sizeof(PatchVertex) == sizeof(float) * 8);

// # of tessellation segments spanned by the midpoint fan patch.
constexpr static uint32_t kMidpointFanPatchSegmentSpan = 8;

// # of tessellation segments spanned by the outer curve patch. (In this particular instance, the
// final segment is a bowtie join with zero length and no fan triangle.)
constexpr static uint32_t kOuterCurvePatchSegmentSpan = 17;

// Define vertex and index buffers that contain all the triangles in every PatchType.
constexpr static uint32_t kMidpointFanPatchVertexCount =
    kMidpointFanPatchSegmentSpan * 4 /*Stroke and/or AA outer ramp*/ +
    (kMidpointFanPatchSegmentSpan + 1) /*Curve fan*/ + 1 /*Triangle from path midpoint*/;
constexpr static uint32_t kMidpointFanPatchIndexCount =
    kMidpointFanPatchSegmentSpan * 6 /*Stroke and/or AA outer ramp*/ +
    (kMidpointFanPatchSegmentSpan - 1) * 3 /*Curve fan*/ + 3 /*Triangle from path midpoint*/;
constexpr static uint32_t kMidpointFanPatchBaseIndex = 0;
static_assert((kMidpointFanPatchBaseIndex * sizeof(uint16_t)) % 4 == 0);
constexpr static uint32_t kOuterCurvePatchVertexCount =
    kOuterCurvePatchSegmentSpan * 8 /*AA center ramp with bowtie*/ +
    kOuterCurvePatchSegmentSpan /*Curve fan*/;
constexpr static uint32_t kOuterCurvePatchIndexCount =
    kOuterCurvePatchSegmentSpan * 12 /*AA center ramp with bowtie*/ +
    (kOuterCurvePatchSegmentSpan - 2) * 3 /*Curve fan*/;
constexpr static uint32_t kOuterCurvePatchBaseIndex = kMidpointFanPatchIndexCount;
static_assert((kOuterCurvePatchBaseIndex * sizeof(uint16_t)) % 4 == 0);
constexpr static uint32_t kPatchVertexBufferCount =
    kMidpointFanPatchVertexCount + kOuterCurvePatchVertexCount;
constexpr static uint32_t kPatchIndexBufferCount =
    kMidpointFanPatchIndexCount + kOuterCurvePatchIndexCount;
void GeneratePatchBufferData(PatchVertex[kPatchVertexBufferCount],
                             uint16_t indices[kPatchIndexBufferCount]);

enum class DrawType : uint8_t
{
    midpointFanPatches, // Standard paths and/or strokes.
    outerCurvePatches,  // Just the outer curves of a path; the interior will be triangulated.
    interiorTriangulation
};

constexpr static uint32_t PatchSegmentSpan(DrawType drawType)
{
    switch (drawType)
    {
        case DrawType::midpointFanPatches:
            return kMidpointFanPatchSegmentSpan;
        case DrawType::outerCurvePatches:
            return kOuterCurvePatchSegmentSpan;
        default:
            RIVE_UNREACHABLE();
    }
}

constexpr static uint32_t PatchIndexCount(DrawType drawType)
{
    switch (drawType)
    {
        case DrawType::midpointFanPatches:
            return kMidpointFanPatchIndexCount;
        case DrawType::outerCurvePatches:
            return kOuterCurvePatchIndexCount;
        default:
            RIVE_UNREACHABLE();
    }
}

constexpr static uintptr_t PatchBaseIndex(DrawType drawType)
{
    switch (drawType)
    {
        case DrawType::midpointFanPatches:
            return kMidpointFanPatchBaseIndex;
        case DrawType::outerCurvePatches:
            return kOuterCurvePatchBaseIndex;
        default:
            RIVE_UNREACHABLE();
    }
}

// Specifies what to do with the render target at the beginning of a flush.
enum class LoadAction : bool
{
    clear,
    preserveRenderTarget
};

// Indicates how much blendMode support will be needed in the "uber" draw shader.
enum class BlendTier : uint8_t
{
    srcOver,     // Every draw uses srcOver.
    advanced,    // Draws use srcOver *and* advanced blend modes, excluding HSL modes.
    advancedHSL, // Draws use srcOver *and* advanced blend modes *and* advanced HSL modes.
};

// Used by ShaderFeatures to generate keys and source code.
enum class SourceType : bool
{
    vertexOnly,
    wholeProgram
};

// Indicates which "uber shader" features to enable in the draw shader.
struct ShaderFeatures
{
    enum PreprocessorDefines : uint32_t
    {
        ENABLE_ADVANCED_BLEND = 1 << 0,
        ENABLE_PATH_CLIPPING = 1 << 1,
        ENABLE_EVEN_ODD = 1 << 2,
        ENABLE_HSL_BLEND_MODES = 1 << 3,
    };

    // Returns a bitmask of which preprocessor macros must be defined in order to support the
    // current feature set.
    uint32_t getPreprocessorDefines(SourceType) const;

    struct
    {
        BlendTier blendTier = BlendTier::srcOver;
        bool enablePathClipping = false;
    } programFeatures;

    struct
    {
        bool enableEvenOdd = false;
    } fragmentFeatures;
};

inline static uint32_t ShaderUniqueKey(SourceType sourceType,
                                       DrawType drawType,
                                       const ShaderFeatures& shaderFeatures)
{
    return (shaderFeatures.getPreprocessorDefines(sourceType) << 1) |
           (drawType == DrawType::interiorTriangulation);
}

// Linked list of draws to be issued by the subclass during onFlush().
struct Draw
{
    Draw(DrawType drawType_, uint32_t baseVertexOrInstance_) :
        drawType(drawType_), baseVertexOrInstance(baseVertexOrInstance_)
    {}
    const DrawType drawType;
    uint32_t baseVertexOrInstance;
    uint32_t vertexOrInstanceCount = 0; // Calculated during PLSRenderContext::flush().
    ShaderFeatures shaderFeatures;
    GrInnerFanTriangulator* triangulator = nullptr; // Used by "interiorTriangulation" draws.
};

// Simple gradients only have 2 texels, so we write them to mapped texture memory from the CPU
// instead of rendering them.
struct TwoTexelRamp
{
    void set(const ColorInt colors[2])
    {
        UnpackColorToRGBA8(colors[0], colorData);
        UnpackColorToRGBA8(colors[1], colorData + 4);
    }
    uint8_t colorData[8];
};
static_assert(sizeof(TwoTexelRamp) == 8 * sizeof(uint8_t));

// Returns the smallest number that can be added to 'value', such that 'value % alignment' == 0.
template <uint32_t Alignment> RIVE_ALWAYS_INLINE uint32_t PaddingToAlignUp(uint32_t value)
{
    constexpr uint32_t maxMultipleOfAlignment =
        std::numeric_limits<uint32_t>::max() / Alignment * Alignment;
    uint32_t padding = (maxMultipleOfAlignment - value) % Alignment;
    assert((value + padding) % Alignment == 0);
    return padding;
}

// Returns the area of the (potentially non-rectangular) quadrilateral that results from
// transforming the given bounds by the given matrix.
float FindTransformedArea(const AABB& bounds, const Mat2D&);
} // namespace rive::pls
