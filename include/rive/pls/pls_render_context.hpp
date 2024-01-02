/*
 * Copyright 2022 Rive
 */

#pragma once

#include "rive/math/mat2d.hpp"
#include "rive/math/vec2d.hpp"
#include "rive/pls/pls.hpp"
#include "rive/pls/pls_factory.hpp"
#include "rive/pls/pls_render_target.hpp"
#include "rive/pls/trivial_block_allocator.hpp"
#include "rive/shapes/paint/color.hpp"
#include <functional>
#include <unordered_map>

namespace rive
{
class RawPath;
} // namespace rive

namespace rive::pls
{
class GradientLibrary;
class PLSGradient;
class PLSPaint;
class PLSPath;
class PLSRenderContextImpl;

// Used as a key for complex gradients.
class GradientContentKey
{
public:
    inline GradientContentKey(rcp<const PLSGradient> gradient);
    inline GradientContentKey(GradientContentKey&& other);
    bool operator==(const GradientContentKey&) const;
    const PLSGradient* gradient() const { return m_gradient.get(); }

private:
    rcp<const PLSGradient> m_gradient;
};

// Hashes all stops and all colors in a complex gradient.
class DeepHashGradient
{
public:
    size_t operator()(const GradientContentKey&) const;
};

// Top-level, API agnostic rendering context for PLSRenderer. This class manages all the GPU
// buffers, context state, and other resources required for Rive's pixel local storage path
// rendering algorithm.
//
// Main algorithm:
// https://docs.google.com/document/d/19Uk9eyFxav6dNSYsI2ZyiX9zHU1YOaJsMB2sdDFVz6s/edit
//
// Batching multiple unique paths:
// https://docs.google.com/document/d/1DLrQimS5pbNaJJ2sAW5oSOsH6_glwDPo73-mtG5_zns/edit
//
// Batching strokes as well:
// https://docs.google.com/document/d/1CRKihkFjbd1bwT08ErMCP4fwSR7D4gnHvgdw_esY9GM/edit
//
// Intended usage pattern of this class:
//
//   context->beginFrame(...);
//   for (path : paths)
//   {
//       context->pushPath(...);
//       for (contour : path.contours)
//       {
//           context->pushContour(...);
//           for (cubic : contour.cubics)
//           {
//               context->pushCubic(...);
//           }
//       }
//   }
//   context->flush();
class PLSRenderContext : public PLSFactory
{
public:
    PLSRenderContext(std::unique_ptr<PLSRenderContextImpl>);
    ~PLSRenderContext();

    // Manually releases all references in the draw list and then resets it back to empty.
    void resetDrawList();

    PLSRenderContextImpl* impl() { return m_impl.get(); }
    template <typename T> T* static_impl_cast() { return static_cast<T*>(m_impl.get()); }

    // Returns the number of flushes that have been executed over the entire lifetime of this class,
    // intermediate or otherwise.
    uint64_t getFlushCount() const { return m_flushCount; }

    // Options for controlling how and where a frame is rendered.
    struct FrameDescriptor
    {
        rcp<const PLSRenderTarget> renderTarget;
        LoadAction loadAction = LoadAction::clear;
        ColorInt clearColor = 0;

        // Enables an experimental pixel local storage mode that makes use of atomics and barriers
        // instead of raster ordering in order to synchronize overlapping fragments.
        bool enableExperimentalAtomicMode = false;

        // Testing flags.
        bool wireframe = false;
        bool fillsDisabled = false;
        bool strokesDisabled = false;

        // Only used for metal backend.
        void* backendSpecificData = nullptr;
    };

    // Called at the beginning of a frame and establishes where and how it will be rendered.
    //
    // All rendering related calls must be made between beginFrame() and flush().
    void beginFrame(FrameDescriptor&&);

    const FrameDescriptor& frameDescriptor() const
    {
        assert(m_didBeginFrame);
        return m_frameDescriptor;
    }

    // Generates a unique clip ID that is guaranteed to not exist in the current clip buffer.
    //
    // Returns 0 if a unique ID could not be generated, at which point the caller must flush before
    // continuing.
    uint32_t generateClipID();

    // Get/set a "clip content ID" that uniquely identifies the current contents of the clip buffer.
    // This ID is reset to 0 on every flush.
    void setClipContentID(uint32_t clipID)
    {
        assert(m_didBeginFrame);
        m_clipContentID = clipID;
    }
    uint32_t getClipContentID()
    {
        assert(m_didBeginFrame);
        return m_clipContentID;
    }

    // Utility for calculating the total tessellation vertex count of a batch of paths, and finding
    // how much padding to insert between paths.
    class TessVertexCounter
    {
    public:
        TessVertexCounter(PLSRenderContext* context) :
            m_initialTessVertexCount(context->m_tessVertexCount),
            m_runningTessVertexCount(context->m_tessVertexCount)
        {
            // Don't create a path calculator while the context is in the middle of pushing a path.
            assert(m_runningTessVertexCount == context->m_expectedTessVertexCountAtEndOfPath);
        }

        // Returns the required number of padding vertices to insert before the path.
        template <size_t PatchSize>
        [[nodiscard]] size_t countPath(size_t pathTessVertexCount, bool isStroked)
        {
            // Ensure there is always at least one padding vertex at the beginning of the
            // tessellation texture.
            size_t padding = m_runningTessVertexCount != 0
                                 ? PaddingToAlignUp<PatchSize>(m_runningTessVertexCount)
                                 : PatchSize;
            m_runningTessVertexCount += padding;
            m_runningTessVertexCount += isStroked ? pathTessVertexCount : pathTessVertexCount * 2;
            return padding;
        }

    private:
        friend class PLSRenderContext;

        size_t totalVertexCountIncludingReflectionsAndPadding() const
        {
            return m_runningTessVertexCount - m_initialTessVertexCount;
        }

        size_t m_initialTessVertexCount;
        size_t m_runningTessVertexCount;
    };

    // Reserves space for 'pathCount', 'contourCount', 'curveCount', and 'tessVertexCount' records
    // in their respective GPU buffers, prior to calling pushPath(), pushContour(), pushCubic().
    //
    // Returns false if there was too much data to accomodate, at which point the caller must flush
    // before continuing.
    [[nodiscard]] bool reservePathData(size_t pathCount,
                                       size_t contourCount,
                                       size_t curveCount,
                                       const TessVertexCounter&);

    // Allocates a horizontal span of texels in the gradient texture and schedules either a texture
    // upload or a draw that fills it with the given gradient's color ramp.
    //
    // Fills out a PaintData record that tells the shader how to access the new gradient.
    //
    // Returns false if the gradient texture is out of space, at which point the caller must flush
    // before continuing.
    [[nodiscard]] bool pushGradient(const PLSGradient*, PaintData*);

    // Pushes a record to the GPU for the given path, which will be referenced by future calls to
    // pushContour() and pushCubic().
    //
    // The first curve of the path will be pre-padded with 'paddingVertexCount' tessellation
    // vertices, colocated at T=0. The caller must use this argument to align the beginning of the
    // path on a boundary of the patch size. (See PLSRenderContext::TessVertexCounter.)
    void pushPath(PatchType,
                  const Mat2D&,
                  float strokeRadius,
                  FillRule,
                  PaintType,
                  const PaintData&,
                  const PLSTexture* imageTexture,
                  uint32_t clipID,
                  const pls::ClipRectInverseMatrix*, // Null if there is no clipRect.
                  BlendMode,
                  uint32_t tessVertexCount,
                  uint32_t paddingVertexCount);

    // Pushes a contour record to the GPU for the given contour, which references the most-recently
    // pushed path and will be referenced by future calls to pushCubic().
    //
    // The first curve of the contour will be pre-padded with 'paddingVertexCount' tessellation
    // vertices, colocated at T=0. The caller must use this argument to align the end of the contour
    // on a boundary of the patch size. (See pls::PaddingToAlignUp().)
    void pushContour(Vec2D midpoint, bool closed, uint32_t paddingVertexCount);

    // Appends a cubic curve and join to the most-recently pushed contour, and reserves the
    // appropriate number of tessellated vertices in the tessellation texture.
    //
    // An instance consists of a cubic curve with "parametricSegmentCount + polarSegmentCount"
    // segments, followed by a join with "joinSegmentCount" segments, for a grand total of
    // "parametricSegmentCount + polarSegmentCount + joinSegmentCount - 1" vertices.
    //
    // If a cubic has already been pushed to the current contour, pts[0] must be equal to the former
    // cubic's pts[3].
    //
    // "joinTangent" is the ending tangent of the join that follows the cubic.
    void pushCubic(const Vec2D pts[4],
                   Vec2D joinTangent,
                   uint32_t additionalContourFlags,
                   uint32_t parametricSegmentCount,
                   uint32_t polarSegmentCount,
                   uint32_t joinSegmentCount);

    // Pushes triangles to be drawn using the data records from the most recent calls to pushPath()
    // and pushPaint().
    void pushInteriorTriangulation(GrInnerFanTriangulator*,
                                   PaintType,
                                   const PaintData&,
                                   const PLSTexture* imageTexture,
                                   uint32_t clipID,
                                   bool hasClipRect,
                                   BlendMode);

    // Reserves space for a single image draw (imageRect or imageMesh) in the GPU buffers.
    //
    // Returns false if there was not room for another image draw, at which point the caller must
    // flush before continuing.
    [[nodiscard]] bool reserveImageDrawUniforms();

    // Pushes an imageRect to the draw list, using the space reserved previously by
    // reserveImageDrawUniforms().
    // This should only be used when we don't have bindless textures in atomic mode. Otherwise,
    // images should be drawn as rectangular paths with an image paint.
    void pushImageRect(const Mat2D&,
                       float opacity,
                       const PLSTexture* imageTexture,
                       uint32_t clipID,
                       const pls::ClipRectInverseMatrix*, // Null if there is no clipRect.
                       BlendMode);

    // Pushes an imageMesh to the draw list, using the space reserved previously by
    // reserveImageDrawUniforms().
    void pushImageMesh(const Mat2D&,
                       float opacity,
                       const PLSTexture* imageTexture,
                       const RenderBuffer* vertexBuffer,
                       const RenderBuffer* uvBuffer,
                       const RenderBuffer* indexBuffer,
                       uint32_t indexCount,
                       uint32_t clipID,
                       const pls::ClipRectInverseMatrix*, // Null if there is no clipRect.
                       BlendMode);

    enum class FlushType : bool
    {
        // The flush was kicked off mid-frame because GPU buffers ran out of room. The flush
        // follwing this one will load the color buffer instead of clearing it, and color data may
        // not make it all the way to the main framebuffer.
        intermediate,

        // This is the final flush of the frame. The flush follwing this one will always clear color
        // buffer, and the main framebuffer is guaranteed to be updated.
        endOfFrame
    };

    // Renders all paths that have been pushed since the last call to flush(). Rendering is done in
    // two steps:
    //
    //  1. Tessellate all cubics into positions and normals in the "tessellation texture".
    //  2. Render the tessellated curves using Rive's pixel local storage path rendering algorithm.
    //
    void flush(FlushType = FlushType::endOfFrame);

    // Reallocates all GPU resources to the basline minimum allocation size.
    void resetGPUResources();

    // Shrinks GPU resource allocations to the maximum per-flush limits seen since the most recent
    // previous call to shrinkGPUResourcesToFit(). This method is intended to be called at a fixed
    // temporal interval. (GPU resource allocations automatically grow based on usage, but can only
    // shrink if the application calls this method.)
    void shrinkGPUResourcesToFit();

    // Returns the context's TrivialBlockAllocator, which is automatically reset at the end of every
    // flush.
    TrivialBlockAllocator* perFrameAllocator()
    {
        assert(m_didBeginFrame);
        return &m_perFrameAllocator;
    }

    // Allocates a trivially destructible object that will be automatically deleted at the end of
    // the current flush.
    template <typename T, typename... Args> T* make(Args&&... args)
    {
        assert(m_didBeginFrame);
        return m_perFrameAllocator.make<T>(std::forward<Args>(args)...);
    }

    // Allocators for intermediate path processing buffers.
    TrivialArrayAllocator<uint8_t>& numChopsAllocator() { return m_numChopsAllocator; }
    TrivialArrayAllocator<Vec2D>& chopVerticesAllocator() { return m_chopVerticesAllocator; }
    TrivialArrayAllocator<std::array<Vec2D, 2>>& tangentPairsAllocator()
    {
        return m_tangentPairsAllocator;
    }
    TrivialArrayAllocator<uint32_t, alignof(float4)>& polarSegmentCountsAllocator()
    {
        return m_polarSegmentCountsAllocator;
    }
    TrivialArrayAllocator<uint32_t, alignof(float4)>& parametricSegmentCountsAllocator()
    {
        return m_parametricSegmentCountsAllocator;
    }

    // Simple linked list whose nodes are allocated on a context's TrivialBlockAllocator.
    template <typename T> class PerFlushLinkedList
    {
    public:
        size_t count() const;
        bool empty() const { return count() == 0; }
        T& tail() const;
        template <typename... Args> T& emplace_back(PLSRenderContext* context, Args... args);
        void reset();

        struct Node
        {
            template <typename... Args> Node(Args... args) : data(std::forward<Args>(args)...) {}
            T data;
            Node* next = nullptr;
        };

        template <typename U> class Iter
        {
        public:
            Iter(Node* current) : m_current(current) {}
            bool operator!=(const Iter& other) const { return m_current != other.m_current; }
            void operator++() { m_current = m_current->next; }
            U& operator*() { return m_current->data; }

        private:
            Node* m_current;
        };
        Iter<T> begin() { return {m_head}; }
        Iter<T> end() { return {nullptr}; }
        Iter<const T> begin() const { return {m_head}; }
        Iter<const T> end() const { return {nullptr}; }

    private:
        Node* m_head = nullptr;
        Node* m_tail = nullptr;
        size_t m_count = 0;
    };

    // Linked list of draws to be issued by the impl during onFlush().
    struct Draw
    {
        Draw(DrawType drawType_, uint32_t baseElement_) :
            drawType(drawType_), baseElement(baseElement_)
        {}

        // We can't have a destructor because we're block-allocated. Instead, the context calls this
        // method before clearing the drawList to release all our held references.
        void releaseRefs();

        const DrawType drawType;
        uint32_t baseElement;      // Base vertex, index, or instance.
        uint32_t elementCount = 0; // Vertex, index, or instance count. Calculated during flush().
        ShaderFeatures shaderFeatures = ShaderFeatures::NONE;
        const PLSTexture* imageTextureRef = nullptr;

        // DrawType-specific data that must be initialized by the caller.
        union
        {
            struct // DrawType::interiorTriangulation.
            {
                GrInnerFanTriangulator* triangulator;
            };
            struct // DrawType::imageMesh.
            {
                const RenderBuffer* vertexBufferRef;
                const RenderBuffer* uvBufferRef;
                const RenderBuffer* indexBufferRef;
                size_t imageDrawDataOffset;
            };
        };
    };

    // Extra data for when FrameDescriptor::enableExperimentalAtomicMode is enabled.
    // TODO: integrate this data more cleanly if/when the atomic mode proves ready to ship.
    struct ExperimentalAtomicModeData
    {
        // +1 so we can index by pathID (pathID is always >0), and use index 0 for the clear color.
        constexpr static size_t kBufferLength = pls::MaxPathID(1) + 1;

        const PLSTexture* m_imageTextures[kBufferLength];

        struct Paint
        {
            uint32_t params; // [clipID, flags, paintType]
            union
            {
                uint32_t color;
                float gradTextureY;
                float opacity;
                uint32_t shiftedClipReplacementID;
            };
        } m_paints[kBufferLength];
        static_assert(sizeof(Paint) == sizeof(uint32_t) * 2);

        std::array<float, 4> m_paintMatrices[kBufferLength];

        struct PaintTranslate
        {
            Vec2D translate;
            union
            {
                std::array<float, 2> gradTextureHorizontalSpan;
                std::array<uint32_t, 2> bindlessTextureHandle;
            };
        } m_paintTranslates[kBufferLength];
        static_assert(sizeof(PaintTranslate) == sizeof(float) * 4);

        std::array<float, 4> m_clipRectMatrices[kBufferLength];

        struct ClipRectTranslate
        {
            Vec2D translate;
            Vec2D inverseFwidth; // -1 / fwidth(clipMatrix * pixelPosition) -- for antialiasing.
        } m_clipRectTranslates[kBufferLength];

        // Fills in the buffers at index 'pathID' with the info for drawing the given path.
        void setDataForPath(uint32_t pathID,
                            const Mat2D&,
                            FillRule,
                            pls::PaintType,
                            const pls::PaintData&,
                            const PLSTexture*,
                            uint32_t clipID,
                            const pls::ClipRectInverseMatrix* clipRectInverseMatrix,
                            BlendMode,
                            const PLSRenderTarget*,
                            float gradientTextureHeight,
                            const PlatformFeatures&);
    };

    // Describes a flush for PLSRenderContextImpl.
    struct FlushDescriptor
    {
        void* backendSpecificData = nullptr;
        const PLSRenderTarget* renderTarget = nullptr;
        LoadAction loadAction = LoadAction::clear;
        ColorInt clearColor = 0;
        pls::ShaderFeatures combinedShaderFeatures = pls::ShaderFeatures::NONE;
        size_t complexGradSpanCount = 0;
        size_t tessVertexSpanCount = 0;
        uint16_t simpleGradTexelsWidth = 0;
        uint16_t simpleGradTexelsHeight = 0;
        uint32_t complexGradRowsTop = 0;
        uint32_t complexGradRowsHeight = 0;
        uint32_t tessDataHeight = 0;
        bool needsClipBuffer = false;
        bool hasTriangleVertices = false;
        bool wireframe = false;
        const PerFlushLinkedList<Draw>* drawList = nullptr;
        size_t pathCount = 0;
        pls::InterlockMode interlockMode = pls::InterlockMode::rasterOrdered;
        ExperimentalAtomicModeData* experimentalAtomicModeData = nullptr;

        // In atomic mode, we can skip the inital clear of the color buffer in some cases. This is
        // because we will be resolving the framebuffer anyway, and can work the clear into that
        // operation.
        bool canSkipColorClear() const
        {
            return interlockMode == pls::InterlockMode::experimentalAtomics &&
                   loadAction == LoadAction::clear && colorAlpha(clearColor) == 255;
        }
    };

    // Backend-specific PLSFactory implementation.
    rcp<RenderBuffer> makeRenderBuffer(RenderBufferType, RenderBufferFlags, size_t) override;
    rcp<RenderImage> decodeImage(Span<const uint8_t>) override;

private:
    // Either appends a draw to m_drawList or merges into m_lastDraw.
    // Updates the draw's ShaderFeatures according to the passed parameters.
    void pushPathDraw(DrawType,
                      size_t baseVertex,
                      FillRule,
                      PaintType,
                      const PaintData&,
                      const PLSTexture* imageTexture,
                      uint32_t clipID,
                      bool hasClipRect,
                      BlendMode);
    void pushDraw(DrawType,
                  size_t baseVertex,
                  PaintType,
                  const PLSTexture* imageTexture,
                  uint32_t clipID,
                  bool hasClipRect,
                  BlendMode);

    // Writes padding vertices to the tessellation texture, with an invalid contour ID that is
    // guaranteed to not be the same ID as any neighbors.
    void pushPaddingVertices(uint32_t count);

    // Allocates a (potentially wrapped) span in the tessellation texture and pushes an instance to
    // render it. If the span does wraps, pushes multiple instances to render each horizontal
    // segment.
    RIVE_ALWAYS_INLINE void pushTessellationSpans(const Vec2D pts[4],
                                                  Vec2D joinTangent,
                                                  uint32_t totalVertexCount,
                                                  uint32_t parametricSegmentCount,
                                                  uint32_t polarSegmentCount,
                                                  uint32_t joinSegmentCount,
                                                  uint32_t contourIDWithFlags);

    // Same as pushTessellationSpans(), but also pushes a reflection of the span, rendered right to
    // left, that emits a mirrored version of the patch with negative coverage. (See
    // TessVertexSpan.)
    RIVE_ALWAYS_INLINE void pushMirroredTessellationSpans(const Vec2D pts[4],
                                                          Vec2D joinTangent,
                                                          uint32_t totalVertexCount,
                                                          uint32_t parametricSegmentCount,
                                                          uint32_t polarSegmentCount,
                                                          uint32_t joinSegmentCount,
                                                          uint32_t contourIDWithFlags);

    // Capacities of all our GPU resource allocations.
    struct GPUResourceLimits
    {
        // Resources allocated at the beginning of a frame (before we actually know how big they
        // will need to be).
        size_t maxPathID;
        size_t maxContourID;
        size_t maxSimpleGradients;
        size_t maxComplexGradientSpans;
        size_t maxTessellationSpans;
        size_t maxImageDrawUniforms;

        // Resources allocated at flush time (after we already know exactly how big they need to
        // be).
        size_t triangleVertexBufferCount;
        size_t gradientTextureHeight;
        size_t tessellationTextureHeight;

        // "*this = max(*this, other)"
        void accumulateMax(const GPUResourceLimits& other)
        {
            maxPathID = std::max(maxPathID, other.maxPathID);
            maxContourID = std::max(maxContourID, other.maxContourID);
            maxSimpleGradients = std::max(maxSimpleGradients, other.maxSimpleGradients);
            maxComplexGradientSpans =
                std::max(maxComplexGradientSpans, other.maxComplexGradientSpans);
            maxTessellationSpans = std::max(maxTessellationSpans, other.maxTessellationSpans);
            maxImageDrawUniforms = std::max(maxImageDrawUniforms, other.maxImageDrawUniforms);
            triangleVertexBufferCount =
                std::max(triangleVertexBufferCount, other.triangleVertexBufferCount);
            gradientTextureHeight = std::max(gradientTextureHeight, other.gradientTextureHeight);
            tessellationTextureHeight =
                std::max(tessellationTextureHeight, other.tessellationTextureHeight);
            static_assert(sizeof(*this) == sizeof(size_t) * 9); // Make sure we got every field.
        }

        // Scale each limit > threshold by a factor of "scaleFactor".
        GPUResourceLimits makeScaledIfLarger(const GPUResourceLimits& threshold,
                                             double scaleFactor) const
        {
            GPUResourceLimits scaled = *this;
            if (maxPathID > threshold.maxPathID)
                scaled.maxPathID = static_cast<double>(maxPathID) * scaleFactor;
            if (maxContourID > threshold.maxContourID)
                scaled.maxContourID = static_cast<double>(maxContourID) * scaleFactor;
            if (maxSimpleGradients > threshold.maxSimpleGradients)
                scaled.maxSimpleGradients = static_cast<double>(maxSimpleGradients) * scaleFactor;
            if (maxComplexGradientSpans > threshold.maxComplexGradientSpans)
                scaled.maxComplexGradientSpans =
                    static_cast<double>(maxComplexGradientSpans) * scaleFactor;
            if (maxTessellationSpans > threshold.maxTessellationSpans)
                scaled.maxTessellationSpans =
                    static_cast<double>(maxTessellationSpans) * scaleFactor;
            if (maxImageDrawUniforms > threshold.maxImageDrawUniforms)
                scaled.maxImageDrawUniforms =
                    static_cast<double>(maxImageDrawUniforms) * scaleFactor;
            if (triangleVertexBufferCount > threshold.triangleVertexBufferCount)
                scaled.triangleVertexBufferCount =
                    static_cast<double>(triangleVertexBufferCount) * scaleFactor;
            if (gradientTextureHeight > threshold.gradientTextureHeight)
                scaled.gradientTextureHeight =
                    static_cast<double>(gradientTextureHeight) * scaleFactor;
            if (tessellationTextureHeight > threshold.tessellationTextureHeight)
                scaled.tessellationTextureHeight =
                    static_cast<double>(tessellationTextureHeight) * scaleFactor;
            static_assert(sizeof(*this) == sizeof(size_t) * 9); // Make sure we got every field.
            return scaled;
        }

        // Scale all limits by a factor of "scaleFactor".
        GPUResourceLimits makeScaled(double scaleFactor) const
        {
            return makeScaledIfLarger(GPUResourceLimits{}, scaleFactor);
        }

        // The resources we allocate at flush time don't need to grow preemptively, since we don't
        // have to allocate them until we know exactly how big they need to be. This method provides
        // a way to reset them to zero, thus preventing them from growing preemptively.
        GPUResourceLimits resetFlushTimeLimits() const
        {
            GPUResourceLimits noFlushTimeLimits = *this;
            noFlushTimeLimits.triangleVertexBufferCount = 0;
            noFlushTimeLimits.gradientTextureHeight = 0;
            noFlushTimeLimits.tessellationTextureHeight = 0;
            return noFlushTimeLimits;
        }
    };

    // Reallocate any GPU resource whose size in 'targetUsage' is larger than its size in
    // 'm_currentResourceLimits'.
    //
    // The new allocation size will be "targetUsage.<resourceLimit> * scaleFactor".
    void growExceededGPUResources(const GPUResourceLimits& targetUsage, double scaleFactor);

    // Reallocate resources to the size specified in 'targets'.
    // Only reallocate the resources for which 'shouldReallocate' returns true.
    void allocateGPUResources(
        const GPUResourceLimits& targets,
        std::function<bool(size_t targetSize, size_t currentSize)> shouldReallocate);

    const std::unique_ptr<PLSRenderContextImpl> m_impl;
    const size_t m_maxPathID;

    // The number of flushes that have been executed over the entire lifetime of this class,
    // intermediate or otherwise.
    size_t m_flushCount = 0;

    GPUResourceLimits m_currentResourceLimits = {};
    GPUResourceLimits m_currentFrameResourceUsage = {};
    GPUResourceLimits m_maxRecentResourceUsage = {};

    // How many rows of the gradient texture are dedicated to simple (two-texel) ramps?
    // This is also the y-coordinate at which the complex color ramps begin.
    size_t m_reservedSimpleGradientRowCount = 0;

    // Per-frame state.
    FrameDescriptor m_frameDescriptor;
    bool m_isFirstFlushOfFrame = true;
    RIVE_DEBUG_CODE(bool m_didBeginFrame = false;)

    // Clipping state.
    uint32_t m_lastGeneratedClipID = 0;
    uint32_t m_clipContentID = 0;

    // Most recent path and contour state.
    bool m_currentPathIsStroked = false;
    bool m_currentPathNeedsMirroredContours = false;
    uint32_t m_currentPathID = 0;
    uint32_t m_currentContourID = 0;
    uint32_t m_currentContourPaddingVertexCount = 0; // Padding vertices to add to the first curve.
    uint32_t m_tessVertexCount = 0;
    uint32_t m_mirroredTessLocation = 0; // Used for back-face culling and mirrored patches.
    RIVE_DEBUG_CODE(uint32_t m_expectedTessVertexCountAtNextReserve = 0;)
    RIVE_DEBUG_CODE(uint32_t m_expectedTessVertexCountAtEndOfPath = 0;)
    RIVE_DEBUG_CODE(uint32_t m_expectedMirroredTessLocationAtEndOfPath = 0;)
    RIVE_DEBUG_CODE(uint32_t m_curveCount = 0;)
    RIVE_DEBUG_CODE(uint32_t m_expectedCurveCountAtNextReserve = 0;)

    // Simple gradients have one stop at t=0 and one stop at t=1. They're implemented with 2 texels.
    std::unordered_map<uint64_t, uint32_t> m_simpleGradients; // [color0, color1] -> rampTexelsIdx

    // Complex gradients have stop(s) between t=0 and t=1. In theory they should be scaled to a ramp
    // where every stop lands exactly on a pixel center, but for now we just always scale them to
    // the entire gradient texture width.
    std::unordered_map<GradientContentKey, uint32_t, DeepHashGradient>
        m_complexGradients; // [colors[0..n], stops[0..n]] -> rowIdx

    WriteOnlyMappedMemory<pls::PathData> m_pathData;
    WriteOnlyMappedMemory<pls::ContourData> m_contourData;
    // Simple gradients get written by the CPU.
    WriteOnlyMappedMemory<pls::TwoTexelRamp> m_simpleColorRampsData;
    // Complex gradients get rendered by the GPU.
    WriteOnlyMappedMemory<pls::GradientSpan> m_gradSpanData;
    WriteOnlyMappedMemory<pls::TessVertexSpan> m_tessSpanData;
    WriteOnlyMappedMemory<pls::ImageDrawUniforms> m_imageDrawUniformData;

    PerFlushLinkedList<Draw> m_drawList;

    // GrTriangulator provides an upper bound on the number of vertices it will emit. Triangulations
    // are not writen out until the last minute, during flush(), and this variable provides an upper
    // bound on the number of vertices that will be written.
    size_t m_maxTriangleVertexCount = 0;

    // Here we cache the contents of the uniform buffer. We use this cached value to determine
    // whether the buffer needs to be updated at the beginning of a flush.
    FlushUniforms m_cachedUniformData;

    std::unique_ptr<ExperimentalAtomicModeData> m_atomicModeData;

    // Simple allocator for trivially-destructible data that needs to persist until the current
    // flush has completed. Any object created with this allocator is automatically deleted during
    // the next call to flush().
    constexpr static size_t kPerFlushAllocatorInitialBlockSize = 1024 * 1024; // 1 MiB.
    TrivialBlockAllocator m_perFrameAllocator{kPerFlushAllocatorInitialBlockSize};

    // Allocators for intermediate path processing buffers.
    constexpr static size_t kIntermediateDataInitialStrokes = 8192;     // * 84 == 688 KiB.
    constexpr static size_t kIntermediateDataInitialFillCurves = 32768; // * 4 == 128 KiB.
    TrivialArrayAllocator<uint8_t> m_numChopsAllocator{kIntermediateDataInitialStrokes *
                                                       4}; // 4 byte per stroke curve.
    TrivialArrayAllocator<Vec2D> m_chopVerticesAllocator{kIntermediateDataInitialStrokes *
                                                         4}; // 32 bytes per stroke curve.
    TrivialArrayAllocator<std::array<Vec2D, 2>> m_tangentPairsAllocator{
        kIntermediateDataInitialStrokes * 2}; // 32 bytes per stroke curve.
    TrivialArrayAllocator<uint32_t, alignof(float4)> m_polarSegmentCountsAllocator{
        kIntermediateDataInitialStrokes * 4}; // 16 bytes per stroke curve.
    TrivialArrayAllocator<uint32_t, alignof(float4)> m_parametricSegmentCountsAllocator{
        kIntermediateDataInitialFillCurves}; // 4 bytes per fill curve.
};

template <typename T> size_t PLSRenderContext::PerFlushLinkedList<T>::count() const
{
    assert(!!m_head == !!m_tail);
    assert(!!m_tail == !!m_count);
    return m_count;
}

template <typename T> T& PLSRenderContext::PerFlushLinkedList<T>::tail() const
{
    assert(!empty());
    return m_tail->data;
}

template <typename T>
template <typename... Args>
T& PLSRenderContext::PerFlushLinkedList<T>::emplace_back(PLSRenderContext* context, Args... args)
{
    Node* node = context->make<Node>(std::forward<Args>(args)...);
    assert(!!m_head == !!m_tail);
    if (m_head == nullptr)
    {
        m_head = node;
    }
    else
    {
        m_tail->next = node;
    }
    m_tail = node;
    ++m_count;
    return m_tail->data;
}

template <typename T> void PLSRenderContext::PerFlushLinkedList<T>::reset()
{
    m_tail = nullptr;
    m_head = nullptr;
    m_count = 0;
}
} // namespace rive::pls
