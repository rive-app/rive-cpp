/*
 * Copyright 2022 Rive
 */

#include "rive/pls/pls_render_context.hpp"

#include "gr_inner_fan_triangulator.hpp"
#include "pls_path.hpp"
#include "pls_paint.hpp"
#include "rive/math/math_types.hpp"
#include "rive/pls/pls_render_context_impl.hpp"

#include <string_view>

namespace rive::pls
{
// Overallocate GPU resources by 25% of current usage, in order to create padding for increase.
constexpr static double kGPUResourcePadding = 1.25;

// When we exceed the capacity of a GPU resource mid-flush, double it immediately.
constexpr static double kGPUResourceIntermediateGrowthFactor = 2;

constexpr size_t kMinSimpleColorRampRows = 1;
constexpr size_t kMaxSimpleColorRampRows = 256; // 65k simple gradients.

constexpr size_t kMinComplexGradients = 31;
constexpr size_t kMinGradTextureHeight = kMinSimpleColorRampRows + kMinComplexGradients;
constexpr size_t kMaxGradTextureHeight = 2048; // TODO: Move this variable to PlatformFeatures.
constexpr size_t kMaxComplexGradients = kMaxGradTextureHeight - kMaxSimpleColorRampRows;

constexpr size_t kMinTessTextureHeight = 32;
constexpr size_t kMaxTessTextureHeight = 2048; // GL_MAX_TEXTURE_SIZE spec minimum.
constexpr size_t kMaxTessellationVertexCount = kMaxTessTextureHeight * kTessTextureWidth;

uint32_t ShaderFeatures::getPreprocessorDefines(SourceType sourceType) const
{
    uint32_t defines = 0;
    if (programFeatures.blendTier != BlendTier::srcOver)
    {
        defines |= PreprocessorDefines::ENABLE_ADVANCED_BLEND;
    }
    if (programFeatures.enablePathClipping)
    {
        defines |= PreprocessorDefines::ENABLE_PATH_CLIPPING;
    }
    if (sourceType != SourceType::vertexOnly)
    {
        if (fragmentFeatures.enableEvenOdd)
        {
            defines |= PreprocessorDefines::ENABLE_EVEN_ODD;
        }
        if (programFeatures.blendTier == BlendTier::advancedHSL)
        {
            defines |= PreprocessorDefines::ENABLE_HSL_BLEND_MODES;
        }
    }
    return defines;
}

BlendTier PLSRenderContext::BlendTierForBlendMode(PLSBlendMode blendMode)
{
    switch (blendMode)
    {
        case PLSBlendMode::srcOver:
            return BlendTier::srcOver;
        case PLSBlendMode::screen:
        case PLSBlendMode::overlay:
        case PLSBlendMode::darken:
        case PLSBlendMode::lighten:
        case PLSBlendMode::colorDodge:
        case PLSBlendMode::colorBurn:
        case PLSBlendMode::hardLight:
        case PLSBlendMode::softLight:
        case PLSBlendMode::difference:
        case PLSBlendMode::exclusion:
        case PLSBlendMode::multiply:
            return BlendTier::advanced;
        case PLSBlendMode::hue:
        case PLSBlendMode::saturation:
        case PLSBlendMode::color:
        case PLSBlendMode::luminosity:
            return BlendTier::advancedHSL;
    }
    RIVE_UNREACHABLE();
}

inline GradientContentKey::GradientContentKey(rcp<const PLSGradient> gradient) :
    m_gradient(std::move(gradient))
{}

inline GradientContentKey::GradientContentKey(GradientContentKey&& other) :
    m_gradient(std::move(other.m_gradient))
{}

bool GradientContentKey::operator==(const GradientContentKey& other) const
{
    if (m_gradient.get() == other.m_gradient.get())
    {
        return true;
    }
    else
    {
        return m_gradient->count() == other.m_gradient->count() &&
               !memcmp(m_gradient->stops(),
                       other.m_gradient->stops(),
                       m_gradient->count() * sizeof(float)) &&
               !memcmp(m_gradient->colors(),
                       other.m_gradient->colors(),
                       m_gradient->count() * sizeof(ColorInt));
    }
}

size_t DeepHashGradient::operator()(const GradientContentKey& key) const
{
    const PLSGradient* grad = key.gradient();
    std::hash<std::string_view> hash;
    size_t x = hash(std::string_view(reinterpret_cast<const char*>(grad->stops()),
                                     grad->count() * sizeof(float)));
    size_t y = hash(std::string_view(reinterpret_cast<const char*>(grad->colors()),
                                     grad->count() * sizeof(ColorInt)));
    return x ^ y;
}

PLSRenderContext::PLSRenderContext(std::unique_ptr<PLSRenderContextImpl> impl) :
    m_platformFeatures(impl->platformFeatures()),
    m_maxPathID(MaxPathID(m_platformFeatures.pathIDGranularity)),
    m_impl(std::move(impl))
{}

PLSRenderContext::~PLSRenderContext()
{
    // Always call flush() to avoid deadlock.
    assert(!m_didBeginFrame);
}

void PLSRenderContext::resetGPUResources()
{
    assert(!m_didBeginFrame);
    // Reset all GPU allocations to the minimum size.
    allocateGPUResources(GPUResourceLimits{}, [](size_t targetSize, size_t currentSize) {
        return targetSize != currentSize;
    });
    // Zero out m_maxRecentResourceUsage.
    m_maxRecentResourceUsage = {};
}

void PLSRenderContext::shrinkGPUResourcesToFit()
{
    assert(!m_didBeginFrame);
    // Shrink GPU allocations to 125% of their maximum recent usage, and only if it would reduce
    // memory by 1/3 or more.
    allocateGPUResources(m_maxRecentResourceUsage.makeScaled(kGPUResourcePadding),
                         [](size_t targetSize, size_t currentSize) {
                             // Only shrink if it would reduce memory by at least 1/3.
                             return targetSize <= currentSize * 2 / 3;
                         });
    // Zero out m_maxRecentResourceUsage for the next interval.
    m_maxRecentResourceUsage = {};
}

void PLSRenderContext::growExceededGPUResources(const GPUResourceLimits& targetLimits,
                                                double scaleFactor)
{
    // Reallocate any GPU resource whose size in 'targetLimits' is larger than its size in
    // 'm_currentResourceLimits'.
    //
    // The new allocation size will be "targetLimits.<resource> * scaleFactor".
    allocateGPUResources(
        targetLimits.makeScaledIfLarger(m_currentResourceLimits, scaleFactor),
        [](size_t targetSize, size_t currentSize) { return targetSize > currentSize; });
}

// How tall to make a resource texture in order to support the given number of items.
template <size_t WidthInItems> constexpr static size_t resource_texture_height(size_t itemCount)
{
    return (itemCount + WidthInItems - 1) / WidthInItems;
}

void PLSRenderContext::allocateGPUResources(
    const GPUResourceLimits& targets,
    std::function<bool(size_t targetSize, size_t currentSize)> shouldReallocate)
{
#if 0
    class Logger
    {
    public:
        void logChangedSize(const char* name, size_t oldSize, size_t newSize, size_t newSizeInBytes)
        {
            if (!m_hasChanged)
            {
                printf("PLSRenderContext::allocateGPUResources():\n");
                m_hasChanged = true;
            }
            printf("  resize %s: %zu -> %zu (%zu KiB)\n",
                   name,
                   oldSize,
                   newSize,
                   newSizeInBytes >> 10);
        }

        void countResourceSize(size_t sizeInBytes) { m_totalSizeInBytes += sizeInBytes; }

        ~Logger()
        {
            if (m_hasChanged)
            {
                printf("  TOTAL GPU resource usage: %zu KiB\n", m_totalSizeInBytes >> 10);
            }
        }

    private:
        bool m_hasChanged = false;
        size_t m_totalSizeInBytes = 0;
    } logger;
#define LOG_CHANGED_SIZE(NAME, OLD_SIZE, NEW_SIZE, NEW_SIZE_IN_BYTES)                              \
    logger.logChangedSize(NAME, OLD_SIZE, NEW_SIZE, NEW_SIZE_IN_BYTES)
#define COUNT_RESOURCE_SIZE(SIZE_IN_BYTES) logger.countResourceSize(SIZE_IN_BYTES)
#else
#define LOG_CHANGED_SIZE(NAME, OLD_SIZE, NEW_SIZE, NEW_SIZE_IN_BYTES)
#define COUNT_RESOURCE_SIZE(SIZE_IN_BYTES)
#endif

    // Path data texture ring.
    constexpr size_t kMinPathIDCount = kPathTextureWidthInItems * 32; // 32 texels tall.
    size_t targetMaxPathID = resource_texture_height<kPathTextureWidthInItems>(targets.maxPathID) *
                             kPathTextureWidthInItems;
    targetMaxPathID = std::clamp(targetMaxPathID, kMinPathIDCount, m_maxPathID);
    size_t targetPathTextureHeight =
        resource_texture_height<kPathTextureWidthInItems>(targetMaxPathID);
    size_t currentPathTextureHeight =
        resource_texture_height<kPathTextureWidthInItems>(m_currentResourceLimits.maxPathID);
    if (shouldReallocate(targetPathTextureHeight, currentPathTextureHeight))
    {
        assert(!m_pathData);
        m_impl->resizePathTexture(kPathTextureWidthInTexels, targetPathTextureHeight);
        LOG_CHANGED_SIZE("path texture height",
                         currentPathTextureHeight,
                         targetPathTextureHeight,
                         m_pathBuffer->totalSizeInBytes());
        m_currentResourceLimits.maxPathID = targetMaxPathID;
    }
    COUNT_RESOURCE_SIZE(m_pathBuffer->totalSizeInBytes());

    // Contour data texture ring.
    constexpr size_t kMinContourIDCount = kContourTextureWidthInItems * 32; // 32 texels tall.
    size_t targetMaxContourID =
        resource_texture_height<kContourTextureWidthInItems>(targets.maxContourID) *
        kContourTextureWidthInItems;
    targetMaxContourID = std::clamp(targetMaxContourID, kMinContourIDCount, kMaxContourID);
    size_t targetContourTextureHeight =
        resource_texture_height<kContourTextureWidthInItems>(targetMaxContourID);
    size_t currentContourTextureHeight =
        resource_texture_height<kContourTextureWidthInItems>(m_currentResourceLimits.maxContourID);
    if (shouldReallocate(targetContourTextureHeight, currentContourTextureHeight))
    {
        assert(!m_contourData);
        m_impl->resizeContourTexture(kContourTextureWidthInTexels, targetContourTextureHeight);
        LOG_CHANGED_SIZE("contour texture height",
                         currentContourTextureHeight,
                         targetContourTextureHeight,
                         m_contourBuffer->totalSizeInBytes());
        m_currentResourceLimits.maxContourID = targetMaxContourID;
    }
    COUNT_RESOURCE_SIZE(m_contourBuffer->totalSizeInBytes());

    // Simple gradient color ramp pixel unpack buffer ring.
    size_t targetSimpleGradientRowCount =
        resource_texture_height<kGradTextureWidthInSimpleRamps>(targets.maxSimpleGradients);
    targetSimpleGradientRowCount =
        std::clamp(targetSimpleGradientRowCount, kMinSimpleColorRampRows, kMaxSimpleColorRampRows);
    assert(m_currentResourceLimits.maxSimpleGradients % kGradTextureWidthInSimpleRamps == 0);
    assert(m_reservedSimpleGradientRowCount ==
           resource_texture_height<kGradTextureWidthInSimpleRamps>(
               m_currentResourceLimits.maxSimpleGradients));
    if (shouldReallocate(targetSimpleGradientRowCount, m_reservedSimpleGradientRowCount))
    {
        assert(!m_simpleColorRampsData);
        m_impl->resizeSimpleColorRampsBuffer(targetSimpleGradientRowCount *
                                             kGradTextureWidthInSimpleRamps * sizeof(TwoTexelRamp));
        LOG_CHANGED_SIZE("maxSimpleGradients",
                         m_reservedSimpleGradientRowCount * kGradTextureWidthInSimpleRamps,
                         targetSimpleGradientRowCount * kGradTextureWidthInSimpleRamps,
                         m_simpleColorRampsBuffer->totalSizeInBytes());
        m_currentResourceLimits.maxSimpleGradients =
            targetSimpleGradientRowCount * kGradTextureWidthInSimpleRamps;
        m_reservedSimpleGradientRowCount = targetSimpleGradientRowCount;
    }
    COUNT_RESOURCE_SIZE(m_simpleColorRampsBuffer->totalSizeInBytes());

    // Instance buffer ring for rendering complex gradients.
    constexpr size_t kMinComplexGradientSpans = kMinComplexGradients * 32;
    constexpr size_t kMaxComplexGradientSpans = kMaxComplexGradients * 64;
    size_t targetComplexGradientSpanCount = std::clamp(targets.maxComplexGradientSpans,
                                                       kMinComplexGradientSpans,
                                                       kMaxComplexGradientSpans);
    if (shouldReallocate(targetComplexGradientSpanCount,
                         m_currentResourceLimits.maxComplexGradientSpans))
    {
        assert(!m_gradSpanData);
        m_impl->resizeGradSpanBuffer(targetComplexGradientSpanCount * sizeof(GradientSpan));
        LOG_CHANGED_SIZE("maxComplexGradientSpans",
                         m_currentResourceLimits.maxComplexGradientSpans,
                         targetComplexGradientSpanCount,
                         m_gradSpanBuffer->totalSizeInBytes());
        m_currentResourceLimits.maxComplexGradientSpans = targetComplexGradientSpanCount;
    }
    COUNT_RESOURCE_SIZE(m_gradSpanBuffer->totalSizeInBytes());

    // Instance buffer ring for rendering path tessellation vertices.
    constexpr size_t kMinTessellationSpans = kMinTessTextureHeight * kTessTextureWidth / 4;
    const size_t maxTessellationSpans = kMaxTessTextureHeight * kTessTextureWidth / 8; // ~100MiB
    size_t targetTessellationSpanCount =
        std::clamp(targets.maxTessellationSpans, kMinTessellationSpans, maxTessellationSpans);
    if (shouldReallocate(targetTessellationSpanCount, m_currentResourceLimits.maxTessellationSpans))
    {
        assert(!m_tessSpanData);
        m_impl->resizeTessVertexSpanBuffer(targetTessellationSpanCount * sizeof(TessVertexSpan));
        LOG_CHANGED_SIZE("maxTessellationSpans",
                         m_currentResourceLimits.maxTessellationSpans,
                         targetTessellationSpanCount,
                         m_tessSpanBuffer->totalSizeInBytes());
        m_currentResourceLimits.maxTessellationSpans = targetTessellationSpanCount;
    }
    COUNT_RESOURCE_SIZE(m_tessSpanBuffer->totalSizeInBytes());

    // Instance buffer ring for literal triangles fed directly by the CPU.
    constexpr size_t kMinTriangleVertexCount = 3072 * 3; // 324 KiB
    // Triangle vertices don't have a maximum limit; we let the other components be the limiting
    // factor and allocate whatever buffer size we need at flush time.
    size_t targetTriangleVertexCount =
        std::max(targets.triangleVertexBufferCount, kMinTriangleVertexCount);
    if (shouldReallocate(targetTriangleVertexCount,
                         m_currentResourceLimits.triangleVertexBufferCount))
    {
        assert(!m_triangleVertexData);
        m_impl->resizeTriangleVertexBuffer(targetTriangleVertexCount * sizeof(TriangleVertex));
        LOG_CHANGED_SIZE("triangleVertexBufferCount",
                         m_currentResourceLimits.triangleVertexBufferCount,
                         targetTriangleVertexCount,
                         m_triangleBuffer->totalSizeInBytes());
        m_currentResourceLimits.triangleVertexBufferCount = targetTriangleVertexCount;
    }
    COUNT_RESOURCE_SIZE(m_triangleBuffer->totalSizeInBytes());

    // Gradient color ramp texture.
    size_t targetGradTextureHeight =
        std::clamp(targets.gradientTextureHeight, kMinGradTextureHeight, kMaxGradTextureHeight);
    if (shouldReallocate(targetGradTextureHeight, m_currentResourceLimits.gradientTextureHeight))
    {
        m_impl->resizeGradientTexture(targetGradTextureHeight);
        LOG_CHANGED_SIZE("gradientTextureHeight",
                         m_currentResourceLimits.gradientTextureHeight,
                         targetGradTextureHeight,
                         targetGradTextureHeight * kGradTextureWidth * 4 * sizeof(uint8_t));
        m_currentResourceLimits.gradientTextureHeight = targetGradTextureHeight;
    }
    COUNT_RESOURCE_SIZE((m_currentResourceLimits.gradientTextureHeight) * kGradTextureWidth * 4 *
                        sizeof(uint8_t));

    // Texture that path tessellation data is rendered into.
    size_t targetTessTextureHeight =
        std::clamp(targets.tessellationTextureHeight, kMinTessTextureHeight, kMaxTessTextureHeight);
    if (shouldReallocate(targetTessTextureHeight,
                         m_currentResourceLimits.tessellationTextureHeight))
    {
        m_impl->resizeTessellationTexture(targetTessTextureHeight);
        LOG_CHANGED_SIZE("tessellationTextureHeight",
                         m_currentResourceLimits.tessellationTextureHeight,
                         targetTessTextureHeight,
                         targetTessTextureHeight * kTessTextureWidth * 4 * sizeof(uint32_t));
        m_currentResourceLimits.tessellationTextureHeight = targetTessTextureHeight;
    }
    COUNT_RESOURCE_SIZE(targetTessTextureHeight * kTessTextureWidth * 4 * sizeof(uint32_t));
}

void PLSRenderContext::beginFrame(FrameDescriptor&& frameDescriptor)
{
    assert(!m_didBeginFrame);
    // Auto-grow GPU allocations to service the maximum recent usage. If the recent usage is larger
    // than the current allocation, scale it by an additional kGPUResourcePadding since we have to
    // reallocate anyway.
    // Also don't preemptively grow the resources we allocate a flush time, since we can just
    // allocate the right sizes once we know exactly how big they need to be.
    growExceededGPUResources(m_maxRecentResourceUsage.resetFlushTimeLimits(), kGPUResourcePadding);
    m_frameDescriptor = std::move(frameDescriptor);
    m_isFirstFlushOfFrame = true;
    m_impl->prepareToMapBuffers();
    RIVE_DEBUG_CODE(m_didBeginFrame = true);
}

uint32_t PLSRenderContext::generateClipID()
{
    assert(m_didBeginFrame);
    if (m_lastGeneratedClipID < m_maxPathID) // maxClipID == maxPathID.
    {
        ++m_lastGeneratedClipID;
        assert(m_clipContentID != m_lastGeneratedClipID); // Totally unexpected, but just in case.
        return m_lastGeneratedClipID;
    }
    return 0; // There are no available clip IDs. The caller should flush and try again.
}

bool PLSRenderContext::reservePathData(size_t pathCount,
                                       size_t contourCount,
                                       size_t curveCount,
                                       const TessVertexCounter& tessVertexCounter)
{
    assert(m_didBeginFrame);
    assert(m_tessVertexCount == m_expectedTessVertexCountAtNextReserve);

    // +1 for the padding vertex at the end of the tessellation data.
    size_t maxTessVertexCountWithInternalPadding =
        tessVertexCounter.totalVertexCountIncludingReflectionsAndPadding() + 1;
    // Line breaks potentially introduce a new span. Count the maximum number of line breaks we
    // might encounter. Since line breaks may also occur from the reflection, just find a simple
    // upper bound.
    size_t maxSpanBreakCount = (1 + maxTessVertexCountWithInternalPadding / kTessTextureWidth) * 2;
    // +pathCount for a span of padding vertices at the beginning of each path.
    // +1 for the padding vertex at the end of the entire tessellation texture (in case this happens
    // to be the final batch of paths in the flush).
    size_t maxPaddingVertexSpans = pathCount + 1;
    size_t maxTessellationSpans = maxPaddingVertexSpans + curveCount + maxSpanBreakCount;

    // Guard against the case where a single draw overwhelms our GPU resources. Since nothing has
    // been mapped yet on the first draw, we have a unique opportunity at this time to grow our
    // resources if needed.
    if (m_currentPathID == 0)
    {
        GPUResourceLimits newLimits{};
        bool needsRealloc = false;
        if (newLimits.maxPathID < pathCount)
        {
            newLimits.maxPathID = pathCount;
            needsRealloc = true;
        }
        if (newLimits.maxContourID < contourCount)
        {
            newLimits.maxContourID = contourCount;
            needsRealloc = true;
        }
        if (newLimits.maxTessellationSpans < maxTessellationSpans)
        {
            newLimits.maxTessellationSpans = maxTessellationSpans;
            needsRealloc = true;
        }
        assert(!m_pathData);
        assert(!m_contourData);
        assert(!m_tessSpanData);
        if (needsRealloc)
        {
            // The very first draw of the flush overwhelmed our GPU resources. Since we haven't
            // mapped any buffers yet, grow these buffers to double the size that overwhelmed them.
            growExceededGPUResources(newLimits, kGPUResourceIntermediateGrowthFactor);
        }
    }

    if (!m_pathData)
    {
        assert(!m_contourData);
        assert(!m_tessSpanData);
        m_pathData.reset(m_impl->mapPathTexture(), m_currentResourceLimits.maxPathID);
        m_contourData.reset(m_impl->mapContourTexture(), m_currentResourceLimits.maxContourID);
        m_tessSpanData.reset(m_impl->mapTessVertexSpanBuffer(),
                             m_currentResourceLimits.maxTessellationSpans);
    }

    // Does the path fit in our current buffers?
    if (m_currentPathID + pathCount <= m_currentResourceLimits.maxPathID &&
        m_currentContourID + contourCount <= m_currentResourceLimits.maxContourID &&
        m_tessSpanData.hasRoomFor(maxTessellationSpans) &&
        m_tessVertexCount + maxTessVertexCountWithInternalPadding <= kMaxTessellationVertexCount)
    {
        assert(m_pathData.hasRoomFor(pathCount));
        assert(m_contourData.hasRoomFor(contourCount));
        RIVE_DEBUG_CODE(m_expectedTessVertexCountAtNextReserve =
                            m_tessVertexCount +
                            tessVertexCounter.totalVertexCountIncludingReflectionsAndPadding());
        assert(m_expectedTessVertexCountAtNextReserve <= kMaxTessellationVertexCount);
        return true;
    }

    return false;
}

bool PLSRenderContext::pushPaint(const PLSPaint* paint, PaintData* data)
{
    assert(m_didBeginFrame);
    if (const PLSGradient* gradient = paint->getGradient())
    {
        if (!pushGradient(gradient, data))
        {
            return false;
        }
    }
    else
    {
        data->setColor(paint->getColor());
    }
    return true;
}

bool PLSRenderContext::pushGradient(const PLSGradient* gradient, PaintData* paintData)
{
    const ColorInt* colors = gradient->colors();
    const float* stops = gradient->stops();
    size_t stopCount = gradient->count();

    uint32_t row, left, right;
    if (stopCount == 2 && stops[0] == 0)
    {
        // This is a simple gradient that can be implemented by a two-texel color ramp.
        assert(stops[1] == 1); // PLSFactory transforms gradients so that the final stop == 1.
        uint64_t simpleKey;
        static_assert(sizeof(simpleKey) == sizeof(ColorInt) * 2);
        RIVE_INLINE_MEMCPY(&simpleKey, gradient->colors(), sizeof(ColorInt) * 2);
        uint32_t rampTexelsIdx;
        auto iter = m_simpleGradients.find(simpleKey);
        if (iter != m_simpleGradients.end())
        {
            rampTexelsIdx = iter->second; // This gradient is already in the texture.
        }
        else
        {
            if (m_simpleGradients.size() >= m_currentResourceLimits.maxSimpleGradients)
            {
                // We ran out of texels in the section for simple color ramps. The caller needs to
                // flush and try again.
                return false;
            }
            rampTexelsIdx = m_simpleGradients.size() * 2;
            if (!m_simpleColorRampsData)
            {
                m_simpleColorRampsData.reset(m_impl->mapSimpleColorRampsBuffer(),
                                             m_currentResourceLimits.maxSimpleGradients);
            }
            m_simpleColorRampsData.set_back(colors);
            m_simpleGradients.insert({simpleKey, rampTexelsIdx});
        }
        row = rampTexelsIdx / kGradTextureWidth;
        left = rampTexelsIdx % kGradTextureWidth;
        right = left + 2;
    }
    else
    {
        // This is a complex gradient. Render it to an entire row of the gradient texture.
        left = 0;
        right = kGradTextureWidth;
        GradientContentKey key(ref_rcp(gradient));
        auto iter = m_complexGradients.find(key);
        if (iter != m_complexGradients.end())
        {
            row = iter->second; // This gradient is already in the texture.
        }
        else
        {
            if (m_complexGradients.size() >= kMaxComplexGradients)
            {
                // We ran out of the maximum supported number of complex gradients. The caller needs
                // to issue an intermediate flush.
                return false;
            }

            // Guard against the case where a gradient draw overwhelms gradient span buffer. When
            // the gradient span buffer hasn't been mapped yet, we have a unique opportunity to grow
            // it if needed.
            size_t spanCount = stopCount + 1;
            if (!m_gradSpanData)
            {
                if (spanCount > m_currentResourceLimits.maxComplexGradientSpans)
                {
                    // The very first gradient draw of the flush overwhelmed our gradient span
                    // buffer. Since we haven't mapped the buffer yet, grow it to double the size
                    // that overwhelmed it.
                    GPUResourceLimits newLimits{};
                    newLimits.maxComplexGradientSpans = spanCount;
                    growExceededGPUResources(newLimits, kGPUResourceIntermediateGrowthFactor);
                }
                m_gradSpanData.reset(m_impl->mapGradSpanBuffer(),
                                     m_currentResourceLimits.maxComplexGradientSpans);
            }

            if (!m_gradSpanData.hasRoomFor(spanCount))
            {
                // We ran out of instances for rendering complex color ramps. The caller needs to
                // flush and try again.
                return false;
            }

            // Push "GradientSpan" instances that will render each section of the color ramp.
            ColorInt lastColor = colors[0];
            uint32_t lastXFixed = 0;
            // The viewport will start at m_reservedSimpleGradientRowCount when rendering
            // color ramps.
            uint32_t y = static_cast<uint32_t>(m_complexGradients.size());
            // "stop * w + .5" converts a stop position to an x-coordinate in the gradient texture.
            // Stops should be aligned (ideally) on pixel centers to prevent bleed.
            // Render half-pixel-wide caps at the beginning and end to ensure the boundary pixels
            // get filled.
            float w = kGradTextureWidth - 1.f;
            for (size_t i = 0; i < stopCount; ++i)
            {
                float x = stops[i] * w + .5f;
                uint32_t xFixed = static_cast<uint32_t>(x * (65536.f / kGradTextureWidth));
                assert(lastXFixed <= xFixed && xFixed < 65536);
                m_gradSpanData.set_back(lastXFixed, xFixed, y, lastColor, colors[i]);
                lastColor = colors[i];
                lastXFixed = xFixed;
            }
            m_gradSpanData.set_back(lastXFixed, 65535u, y, lastColor, lastColor);

            row = m_reservedSimpleGradientRowCount + m_complexGradients.size();
            m_complexGradients.emplace(std::move(key), row);
        }
    }
    paintData->setGradient(row, left, right, gradient->coeffs());
    return true;
}

void PLSRenderContext::pushPath(PatchType patchType,
                                const Mat2D& matrix,
                                float strokeRadius,
                                FillRule fillRule,
                                PaintType paintType,
                                uint32_t clipID,
                                PLSBlendMode blendMode,
                                const PaintData& paintData,
                                uint32_t tessVertexCount,
                                uint32_t paddingVertexCount)
{
    assert(m_didBeginFrame);
    assert(m_tessVertexCount == m_expectedTessVertexCountAtEndOfPath);
    assert(m_mirroredTessLocation == m_expectedMirroredTessLocationAtEndOfPath);

    m_currentPathIsStroked = strokeRadius != 0;
    m_currentPathNeedsMirroredContours = !m_currentPathIsStroked;
    m_pathData.set_back(matrix, strokeRadius, fillRule, paintType, clipID, blendMode, paintData);

    ++m_currentPathID;
    assert(0 < m_currentPathID && m_currentPathID <= m_maxPathID);
    assert(m_currentPathID == m_pathData.bytesWritten() / sizeof(PathData));

    auto drawType = patchType == PatchType::midpointFan ? DrawType::midpointFanPatches
                                                        : DrawType::outerCurvePatches;
    uint32_t baseVertexToDraw = m_tessVertexCount + paddingVertexCount;
    uint32_t patchSize = PatchSegmentSpan(drawType);
    uint32_t baseInstance = baseVertexToDraw / patchSize;
    // The caller is responsible to pad each path so it begins on a multiple of the patch size.
    // (See PLSRenderContext::PathPaddingCalculator.)
    assert(baseInstance * patchSize == baseVertexToDraw);
    pushDraw(drawType, baseInstance, fillRule, paintType, clipID, blendMode);
    assert(m_drawList.tail().baseVertexOrInstance + m_drawList.tail().vertexOrInstanceCount ==
           baseInstance);
    uint32_t vertexCountToDraw = tessVertexCount - paddingVertexCount;
    if (m_currentPathNeedsMirroredContours)
    {
        vertexCountToDraw *= 2;
    }
    uint32_t instanceCount = vertexCountToDraw / patchSize;
    // The caller is responsible to pad each contour so it ends on a multiple of the patch size.
    assert(instanceCount * patchSize == vertexCountToDraw);
    m_drawList.tail().vertexOrInstanceCount += instanceCount;

    // The first curve of the path will be pre-padded with 'paddingVertexCount' tessellation
    // vertices, colocated at T=0. The caller must use this argument align the beginning of the path
    // on a boundary of the patch size. (See PLSRenderContext::TessVertexCounter.)
    if (paddingVertexCount > 0)
    {
        pushPaddingVertices(paddingVertexCount);
    }

    size_t tessVertexCountWithoutPadding = tessVertexCount - paddingVertexCount;
    if (m_currentPathNeedsMirroredContours)
    {
        m_tessVertexCount = m_mirroredTessLocation =
            m_tessVertexCount + tessVertexCountWithoutPadding;
        RIVE_DEBUG_CODE(m_expectedMirroredTessLocationAtEndOfPath =
                            m_mirroredTessLocation - tessVertexCountWithoutPadding);
    }
    RIVE_DEBUG_CODE(m_expectedTessVertexCountAtEndOfPath =
                        m_tessVertexCount + tessVertexCountWithoutPadding);
    assert(m_expectedTessVertexCountAtEndOfPath <= m_expectedTessVertexCountAtNextReserve);
    assert(m_expectedTessVertexCountAtEndOfPath <= kMaxTessellationVertexCount);
}

void PLSRenderContext::pushContour(Vec2D midpoint, bool closed, uint32_t paddingVertexCount)
{
    assert(m_didBeginFrame);
    assert(m_pathData.bytesWritten() > 0);
    assert(m_currentPathIsStroked || closed);
    assert(m_currentPathID != 0); // pathID can't be zero.

    if (m_currentPathIsStroked)
    {
        midpoint.x = closed ? 1 : 0;
    }
    m_contourData.emplace_back(midpoint, m_currentPathID, static_cast<uint32_t>(m_tessVertexCount));
    ++m_currentContourID;
    assert(0 < m_currentContourID && m_currentContourID <= kMaxContourID);
    assert(m_currentContourID == m_contourData.bytesWritten() / sizeof(ContourData));

    // The first curve of the contour will be pre-padded with 'paddingVertexCount' tessellation
    // vertices, colocated at T=0. The caller must use this argument align the end of the contour on
    // a boundary of the patch size. (See pls::PaddingToAlignUp().)
    m_currentContourPaddingVertexCount = paddingVertexCount;
}

void PLSRenderContext::pushCubic(const Vec2D pts[4],
                                 Vec2D joinTangent,
                                 uint32_t additionalPLSFlags,
                                 uint32_t parametricSegmentCount,
                                 uint32_t polarSegmentCount,
                                 uint32_t joinSegmentCount)
{
    assert(m_didBeginFrame);
    assert(0 <= parametricSegmentCount && parametricSegmentCount <= kMaxParametricSegments);
    assert(0 <= polarSegmentCount && polarSegmentCount <= kMaxPolarSegments);
    assert(joinSegmentCount > 0);
    assert(m_currentContourID != 0); // contourID can't be zero.

    // Polar and parametric segments share the same beginning and ending vertices, so the merged
    // *vertex* count is equal to the sum of polar and parametric *segment* counts.
    uint32_t curveMergedVertexCount = parametricSegmentCount + polarSegmentCount;
    // -1 because the curve and join share an ending/beginning vertex.
    uint32_t totalVertexCount =
        m_currentContourPaddingVertexCount + curveMergedVertexCount + joinSegmentCount - 1;

    // Only the first curve of a contour gets padding vertices.
    m_currentContourPaddingVertexCount = 0;

    if (m_currentPathNeedsMirroredContours)
    {
        pushMirroredTessellationSpans(pts,
                                      joinTangent,
                                      totalVertexCount,
                                      parametricSegmentCount,
                                      polarSegmentCount,
                                      joinSegmentCount,
                                      m_currentContourID | additionalPLSFlags);
    }
    else
    {
        pushTessellationSpans(pts,
                              joinTangent,
                              totalVertexCount,
                              parametricSegmentCount,
                              polarSegmentCount,
                              joinSegmentCount,
                              m_currentContourID | additionalPLSFlags);
    }
}

void PLSRenderContext::pushPaddingVertices(uint32_t count)
{
    constexpr static Vec2D kEmptyCubic[4]{};
    // This is guaranteed to not collide with a neighboring contour ID.
    constexpr static uint32_t kInvalidContourID = 0;
    assert(m_tessVertexCount == m_expectedTessVertexCountAtEndOfPath);
    RIVE_DEBUG_CODE(m_expectedTessVertexCountAtEndOfPath = m_tessVertexCount + count;)
    assert(m_expectedTessVertexCountAtEndOfPath <= kMaxTessellationVertexCount);
    pushTessellationSpans(kEmptyCubic, {0, 0}, count, 0, 0, 1, kInvalidContourID);
    assert(m_tessVertexCount == m_expectedTessVertexCountAtEndOfPath);
}

RIVE_ALWAYS_INLINE void PLSRenderContext::pushTessellationSpans(const Vec2D pts[4],
                                                                Vec2D joinTangent,
                                                                uint32_t totalVertexCount,
                                                                uint32_t parametricSegmentCount,
                                                                uint32_t polarSegmentCount,
                                                                uint32_t joinSegmentCount,
                                                                uint32_t contourIDWithFlags)
{
    uint32_t y = m_tessVertexCount / kTessTextureWidth;
    int32_t x0 = m_tessVertexCount % kTessTextureWidth;
    int32_t x1 = x0 + totalVertexCount;
    for (;;)
    {
        m_tessSpanData.set_back(pts,
                                joinTangent,
                                static_cast<float>(y),
                                x0,
                                x1,
                                parametricSegmentCount,
                                polarSegmentCount,
                                joinSegmentCount,
                                contourIDWithFlags);
        if (x1 > static_cast<int32_t>(kTessTextureWidth))
        {
            // The span was too long to fit on the current line. Wrap and draw it again, this
            // time behind the left edge of the texture so we capture what got clipped off last
            // time.
            ++y;
            x0 -= kTessTextureWidth;
            x1 -= kTessTextureWidth;
            continue;
        }
        break;
    }
    assert(y == (m_tessVertexCount + totalVertexCount - 1) / kTessTextureWidth);

    m_tessVertexCount += totalVertexCount;
    assert(m_tessVertexCount <= m_expectedTessVertexCountAtEndOfPath);
}

RIVE_ALWAYS_INLINE void PLSRenderContext::pushMirroredTessellationSpans(
    const Vec2D pts[4],
    Vec2D joinTangent,
    uint32_t totalVertexCount,
    uint32_t parametricSegmentCount,
    uint32_t polarSegmentCount,
    uint32_t joinSegmentCount,
    uint32_t contourIDWithFlags)
{
    int32_t y = m_tessVertexCount / kTessTextureWidth;
    int32_t x0 = m_tessVertexCount % kTessTextureWidth;
    int32_t x1 = x0 + totalVertexCount;

    uint32_t reflectionY = (m_mirroredTessLocation - 1) / kTessTextureWidth;
    int32_t reflectionX0 = (m_mirroredTessLocation - 1) % kTessTextureWidth + 1;
    int32_t reflectionX1 = reflectionX0 - totalVertexCount;

    for (;;)
    {
        m_tessSpanData.set_back(pts,
                                joinTangent,
                                static_cast<float>(y),
                                x0,
                                x1,
                                static_cast<float>(reflectionY),
                                reflectionX0,
                                reflectionX1,
                                parametricSegmentCount,
                                polarSegmentCount,
                                joinSegmentCount,
                                contourIDWithFlags);
        if (x1 > static_cast<int32_t>(kTessTextureWidth) || reflectionX1 < 0)
        {
            // Either the span or its reflection was too long to fit on the current line. Wrap and
            // draw one both of them both again, this time behind the opposite edge of the texture
            // so we capture what got clipped off last time.
            ++y;
            x0 -= kTessTextureWidth;
            x1 -= kTessTextureWidth;

            --reflectionY;
            reflectionX0 += kTessTextureWidth;
            reflectionX1 += kTessTextureWidth;
            continue;
        }
        break;
    }

    m_tessVertexCount += totalVertexCount;
    assert(m_tessVertexCount <= m_expectedTessVertexCountAtEndOfPath);

    m_mirroredTessLocation -= totalVertexCount;
    assert(m_mirroredTessLocation >= m_expectedMirroredTessLocationAtEndOfPath);
}

void PLSRenderContext::pushInteriorTriangulation(GrInnerFanTriangulator* triangulator,
                                                 PaintType paintType,
                                                 uint32_t clipID,
                                                 PLSBlendMode blendMode)
{
    pushDraw(DrawType::interiorTriangulation,
             0,
             triangulator->fillRule(),
             paintType,
             clipID,
             blendMode);
    m_maxTriangleVertexCount += triangulator->maxVertexCount();
    triangulator->setPathID(m_currentPathID);
    m_drawList.tail().triangulator = triangulator;
}

void PLSRenderContext::pushDraw(DrawType drawType,
                                size_t baseVertex,
                                FillRule fillRule,
                                PaintType paintType,
                                uint32_t clipID,
                                PLSBlendMode blendMode)
{
    if (m_drawList.empty() || m_drawList.tail().drawType != drawType)
    {
        m_drawList.emplace_back(this, drawType, baseVertex);
    }
    ShaderFeatures* shaderFeatures = &m_drawList.tail().shaderFeatures;
    if (blendMode > PLSBlendMode::srcOver)
    {
        assert(paintType != PaintType::clipReplace);
        shaderFeatures->programFeatures.blendTier =
            std::max(shaderFeatures->programFeatures.blendTier, BlendTierForBlendMode(blendMode));
    }
    if (clipID != 0)
    {
        shaderFeatures->programFeatures.enablePathClipping = true;
    }
    if (fillRule == FillRule::evenOdd)
    {
        shaderFeatures->fragmentFeatures.enableEvenOdd = true;
    }
}

template <typename T> bool bits_equal(const T* a, const T* b)
{
    return memcmp(a, b, sizeof(T)) == 0;
}

void PLSRenderContext::flush(FlushType flushType)
{
    assert(m_didBeginFrame);
    if (flushType == FlushType::intermediate)
    {
        // We might not have pushed as many tessellation vertices as expected if we ran out of room
        // for the paint and had to flush.
        assert(m_tessVertexCount <= m_expectedTessVertexCountAtNextReserve);
    }
    else
    {
        assert(m_tessVertexCount == m_expectedTessVertexCountAtNextReserve);
    }
    assert(m_tessVertexCount == m_expectedTessVertexCountAtEndOfPath);
    assert(m_mirroredTessLocation == m_expectedMirroredTessLocationAtEndOfPath);

    // The final vertex of the final patch of each contour crosses over into the next contour. (This
    // is how we wrap around back to the beginning.) Therefore, the final contour of the flush needs
    // an out-of-contour vertex to cross into as well, so we emit a padding vertex here at the end.
    if (m_tessSpanData.bytesWritten() > 0)
    {
        pushPaddingVertices(1);
    }

    // Since we don't write these resources until flush time, we can wait to resize them until now,
    // when we know exactly how large they need to be.
    GPUResourceLimits newLimitsForFlushTimeResources{};
    bool needsFlushTimeRealloc = false;
    if (m_currentResourceLimits.triangleVertexBufferCount < m_maxTriangleVertexCount)
    {
        newLimitsForFlushTimeResources.triangleVertexBufferCount = m_maxTriangleVertexCount;
        needsFlushTimeRealloc = true;
    }
    size_t requiredGradTextureHeight = m_reservedSimpleGradientRowCount + m_complexGradients.size();
    if (m_currentResourceLimits.gradientTextureHeight < requiredGradTextureHeight)
    {
        newLimitsForFlushTimeResources.gradientTextureHeight = requiredGradTextureHeight;
        needsFlushTimeRealloc = true;
    }
    size_t requiredTessTextureHeight =
        resource_texture_height<kTessTextureWidth>(m_tessVertexCount);
    if (m_currentResourceLimits.tessellationTextureHeight < requiredTessTextureHeight)
    {
        newLimitsForFlushTimeResources.tessellationTextureHeight = requiredTessTextureHeight;
        needsFlushTimeRealloc = true;
    }
    if (needsFlushTimeRealloc)
    {
        growExceededGPUResources(newLimitsForFlushTimeResources, kGPUResourcePadding);
    }
    if (m_maxTriangleVertexCount > 0)
    {
        assert(!m_triangleVertexData);
        m_triangleVertexData.reset(m_impl->mapTriangleVertexBuffer(),
                                   m_currentResourceLimits.triangleVertexBufferCount);
        assert(m_triangleVertexData.hasRoomFor(m_maxTriangleVertexCount));
    }
    assert(m_complexGradients.size() <=
           m_currentResourceLimits.gradientTextureHeight - m_reservedSimpleGradientRowCount);
    assert(m_tessVertexCount <=
           m_currentResourceLimits.tessellationTextureHeight * kTessTextureWidth);

    // Finish calculating our DrawList.
    bool needsClipBuffer = false;
    RIVE_DEBUG_CODE(size_t drawIdx = 0;)
    size_t writtenTriangleVertexCount = 0;
    for (Draw& draw : m_drawList)
    {
        switch (draw.drawType)
        {
            case DrawType::midpointFanPatches:
            case DrawType::outerCurvePatches:
                break;
            case DrawType::interiorTriangulation:
            {
                size_t maxVertexCount = draw.triangulator->maxVertexCount();
                assert(writtenTriangleVertexCount + maxVertexCount <= m_maxTriangleVertexCount);
                size_t actualVertexCount = maxVertexCount;
                if (maxVertexCount > 0)
                {
                    actualVertexCount = draw.triangulator->polysToTriangles(&m_triangleVertexData);
                }
                assert(actualVertexCount <= maxVertexCount);
                draw.baseVertexOrInstance = writtenTriangleVertexCount;
                draw.vertexOrInstanceCount = actualVertexCount;
                writtenTriangleVertexCount += actualVertexCount;
                break;
            }
        }
        needsClipBuffer = needsClipBuffer || draw.shaderFeatures.programFeatures.enablePathClipping;
        RIVE_DEBUG_CODE(++drawIdx;)
    }
    assert(drawIdx == m_drawList.count());

    // Determine how much to draw.
    size_t simpleColorRampCount = m_simpleColorRampsData.bytesWritten() / sizeof(TwoTexelRamp);
    size_t gradSpanCount = m_gradSpanData.bytesWritten() / sizeof(GradientSpan);
    size_t tessVertexSpanCount = m_tessSpanData.bytesWritten() / sizeof(TessVertexSpan);
    size_t tessDataHeight = resource_texture_height<kTessTextureWidth>(m_tessVertexCount);

    // Unmap all non-empty buffers before flushing.
    if (m_pathData)
    {
        size_t texelsWritten = m_pathData.bytesWritten() / (sizeof(uint32_t) * 4);
        size_t widthWritten = std::min(texelsWritten, kPathTextureWidthInTexels);
        size_t heightWritten = resource_texture_height<kPathTextureWidthInTexels>(texelsWritten);
        m_impl->unmapPathTexture(widthWritten, heightWritten);
        m_pathData.reset();
    }
    if (m_contourData)
    {
        size_t texelsWritten = m_contourData.bytesWritten() / (sizeof(uint32_t) * 4);
        size_t widthWritten = std::min(texelsWritten, kContourTextureWidthInTexels);
        size_t heightWritten = resource_texture_height<kContourTextureWidthInTexels>(texelsWritten);
        m_impl->unmapContourTexture(widthWritten, heightWritten);
        m_contourData.reset();
    }
    if (m_simpleColorRampsData)
    {
        m_impl->unmapSimpleColorRampsBuffer(m_simpleColorRampsData.bytesWritten());
        m_simpleColorRampsData.reset();
    }
    if (m_gradSpanData)
    {
        m_impl->unmapGradSpanBuffer(m_gradSpanData.bytesWritten());
        m_gradSpanData.reset();
    }
    if (m_tessSpanData)
    {
        m_impl->unmapTessVertexSpanBuffer(m_tessSpanData.bytesWritten());
        m_tessSpanData.reset();
    }
    if (m_triangleVertexData)
    {
        m_impl->unmapTriangleVertexBuffer(m_triangleVertexData.bytesWritten());
        m_triangleVertexData.reset();
    }

    // Update the uniform buffer for drawing if needed.
    FlushUniforms uniformData(m_complexGradients.size(),
                              tessDataHeight,
                              m_frameDescriptor.renderTarget->width(),
                              m_frameDescriptor.renderTarget->height(),
                              m_currentResourceLimits.gradientTextureHeight,
                              m_platformFeatures);
    if (!bits_equal(&m_cachedUniformData, &uniformData))
    {
        m_impl->updateFlushUniforms(&uniformData);
        m_cachedUniformData = uniformData;
    }

    FlushDescriptor flushDesc;
    flushDesc.renderTarget = frameDescriptor().renderTarget.get();
    flushDesc.loadAction =
        m_isFirstFlushOfFrame ? frameDescriptor().loadAction : LoadAction::preserveRenderTarget;
    flushDesc.clearColor = frameDescriptor().clearColor;
    flushDesc.complexGradSpanCount = gradSpanCount;
    flushDesc.tessVertexSpanCount = tessVertexSpanCount;
    flushDesc.simpleGradTexelsWidth = std::min(simpleColorRampCount * 2, kGradTextureWidth);
    flushDesc.simpleGradTexelsHeight =
        resource_texture_height<kGradTextureWidthInSimpleRamps>(simpleColorRampCount);
    flushDesc.complexGradRowsTop = m_reservedSimpleGradientRowCount;
    flushDesc.complexGradRowsHeight = m_complexGradients.size();
    flushDesc.tessDataHeight = tessDataHeight;
    flushDesc.needsClipBuffer = needsClipBuffer;
    flushDesc.hasTriangleVertices = m_maxTriangleVertexCount > 0;
    flushDesc.wireframe = frameDescriptor().wireframe;
    flushDesc.drawList = &m_drawList;
    m_impl->flush(flushDesc);

    m_currentFrameResourceUsage.maxPathID += m_currentPathID;
    m_currentFrameResourceUsage.maxContourID += m_currentContourID;
    m_currentFrameResourceUsage.maxSimpleGradients += m_simpleGradients.size();
    m_currentFrameResourceUsage.maxComplexGradientSpans += gradSpanCount;
    m_currentFrameResourceUsage.maxTessellationSpans += tessVertexSpanCount;
    m_currentFrameResourceUsage.triangleVertexBufferCount += m_maxTriangleVertexCount;
    m_currentFrameResourceUsage.gradientTextureHeight +=
        resource_texture_height<kGradTextureWidthInSimpleRamps>(m_simpleGradients.size()) +
        m_complexGradients.size();
    m_currentFrameResourceUsage.tessellationTextureHeight +=
        resource_texture_height<kTessTextureWidth>(m_tessVertexCount);
    static_assert(sizeof(m_currentFrameResourceUsage) ==
                  sizeof(size_t) * 8); // Make sure we got every field.

    if (flushType == FlushType::intermediate)
    {
        // Intermediate flushes in a single frame are BAD. If the current frame's accumulative usage
        // (across all flushes) of any resource is larger than the current allocation, double it!
        // Also don't preemptively grow the resources we allocate a flush time, since we can just
        // allocate the right sizes once we know exactly how big they need to be.
        growExceededGPUResources(m_currentFrameResourceUsage.resetFlushTimeLimits(),
                                 kGPUResourceIntermediateGrowthFactor);
    }
    else
    {
        assert(flushType == FlushType::endOfFrame);
        m_frameDescriptor = FrameDescriptor{};
        m_maxRecentResourceUsage.accumulateMax(m_currentFrameResourceUsage);
        m_currentFrameResourceUsage = GPUResourceLimits{};
        RIVE_DEBUG_CODE(m_didBeginFrame = false;)
    }

    m_lastGeneratedClipID = 0;
    m_clipContentID = 0;

    m_currentPathID = 0;
    m_currentContourID = 0;

    m_simpleGradients.clear();
    m_complexGradients.clear();

    m_tessVertexCount = 0;
    m_mirroredTessLocation = 0;
    RIVE_DEBUG_CODE(m_expectedTessVertexCountAtNextReserve = 0);
    RIVE_DEBUG_CODE(m_expectedTessVertexCountAtEndOfPath = 0);
    RIVE_DEBUG_CODE(m_expectedMirroredTessLocationAtEndOfPath = 0);

    m_maxTriangleVertexCount = 0;

    m_isFirstFlushOfFrame = false;

    m_drawList.reset();

    // Delete all objects that were allocted for this flush using the TrivialBlockAllocator.
    m_trivialPerFlushAllocator.reset();

    if (flushType == FlushType::intermediate)
    {
        // The frame isn't complete yet. The caller will begin preparing a new flush immediately
        // after this method returns, so lock buffers for the next flush now.
        m_impl->prepareToMapBuffers();
    }
}
} // namespace rive::pls
