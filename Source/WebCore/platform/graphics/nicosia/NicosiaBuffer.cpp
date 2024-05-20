/*
 * Copyright (C) 2017 Metrological Group B.V.
 * Copyright (C) 2017, 2024 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "NicosiaBuffer.h"

#include <wtf/FastMalloc.h>

#if USE(SKIA)
#include "FontRenderOptions.h"
#include "GLFence.h"
#include "PlatformDisplay.h"
#include <skia/core/SkCanvas.h>
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkStream.h>
#include <skia/gpu/GrBackendSurface.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <skia/gpu/ganesh/gl/GrGLDirectContext.h>

#if USE(LIBEPOXY)
#include <epoxy/gl.h>
#else
#include <GLES3/gl3.h>
#endif
#endif

#if USE(GBM)
#include "DRMDeviceManager.h"
#include "PlatformDisplay.h"

#include <gbm.h>
#include <drm_fourcc.h>
#include <wtf/SafeStrerror.h>
#include <wtf/unix/UnixFileDescriptor.h>
#endif

namespace Nicosia {

Lock Buffer::s_layersMemoryUsageLock;
double Buffer::s_currentLayersMemoryUsage = 0.0;
double Buffer::s_maxLayersMemoryUsage = 0.0;

void Buffer::resetMemoryUsage()
{
    Locker locker { s_layersMemoryUsageLock };
    s_maxLayersMemoryUsage = s_currentLayersMemoryUsage;
}

double Buffer::getMemoryUsage()
{
    // The memory usage is max of memory usage since last resetMemoryUsage or getMemoryUsage.
    Locker locker { s_layersMemoryUsageLock };
    const auto memoryUsage = s_maxLayersMemoryUsage;
    s_maxLayersMemoryUsage = s_currentLayersMemoryUsage;
    return memoryUsage;
}

Ref<Buffer> UnacceleratedBuffer::create(const WebCore::IntSize& size, Flags flags)
{
    return adoptRef(*new UnacceleratedBuffer(size, flags));
}

UnacceleratedBuffer::UnacceleratedBuffer(const WebCore::IntSize& size, Flags flags)
    : Buffer(flags)
    , m_size(size)
{
    const auto checkedArea = size.area() * 4;
    m_data = MallocPtr<unsigned char>::tryZeroedMalloc(checkedArea);

    {
        Locker locker { s_layersMemoryUsageLock };
        s_currentLayersMemoryUsage += checkedArea;
        s_maxLayersMemoryUsage = std::max(s_maxLayersMemoryUsage, s_currentLayersMemoryUsage);
    }

#if USE(SKIA)
    auto imageInfo = SkImageInfo::MakeN32Premul(size.width(), size.height(), SkColorSpace::MakeSRGB());
    // FIXME: ref buffer and unref on release proc?
    SkSurfaceProps properties = { 0, WebCore::FontRenderOptions::singleton().subpixelOrder() };
    m_surface = SkSurfaces::WrapPixels(imageInfo, m_data.get(), imageInfo.minRowBytes64(), &properties);
#endif
}

UnacceleratedBuffer::~UnacceleratedBuffer()
{
    const auto checkedArea = m_size.area().value() * 4;
    {
        Locker locker { s_layersMemoryUsageLock };
        s_currentLayersMemoryUsage -= checkedArea;
    }
}

void UnacceleratedBuffer::beginPainting()
{
    Locker locker { m_painting.lock };
    ASSERT(m_painting.state == PaintingState::Complete);
    m_painting.state = PaintingState::InProgress;
}

void UnacceleratedBuffer::completePainting()
{
    Locker locker { m_painting.lock };
    ASSERT(m_painting.state == PaintingState::InProgress);
    m_painting.state = PaintingState::Complete;
    m_painting.condition.notifyOne();
}

void UnacceleratedBuffer::waitUntilPaintingComplete()
{
    Locker locker { m_painting.lock };
    m_painting.condition.wait(m_painting.lock, [this] {
        return m_painting.state == PaintingState::Complete;
    });
}

#if USE(SKIA)
Ref<Buffer> AcceleratedBuffer::create(sk_sp<SkSurface>&& surface, Flags flags)
{
    return adoptRef(*new AcceleratedBuffer(WTFMove(surface), flags));
}

AcceleratedBuffer::AcceleratedBuffer(sk_sp<SkSurface>&& surface, Flags flags)
    : Buffer(flags)
{
    m_surface = WTFMove(surface);
}

AcceleratedBuffer::~AcceleratedBuffer() = default;

WebCore::IntSize AcceleratedBuffer::size() const
{
    return { m_surface->width(), m_surface->height() };
}

void AcceleratedBuffer::beginPainting()
{
    m_surface->getCanvas()->save();
    m_surface->getCanvas()->clear(SkColors::kTransparent);
}

void AcceleratedBuffer::completePainting()
{
    m_surface->getCanvas()->restore();

    auto* grContext = WebCore::PlatformDisplay::sharedDisplayForCompositing().skiaGrContext();
    if (WebCore::GLFence::isSupported()) {
        grContext->flushAndSubmit(m_surface.get(), GrSyncCpu::kNo);
        m_fence = WebCore::GLFence::create();
        if (!m_fence)
            grContext->submit(GrSyncCpu::kYes);
    } else
        grContext->flushAndSubmit(m_surface.get(), GrSyncCpu::kYes);

    auto texture = SkSurfaces::GetBackendTexture(m_surface.get(), SkSurface::BackendHandleAccess::kFlushRead);
    ASSERT(texture.isValid());
    GrGLTextureInfo textureInfo;
    bool retrievedTextureInfo = GrBackendTextures::GetGLTextureInfo(texture, &textureInfo);
    ASSERT_UNUSED(retrievedTextureInfo, retrievedTextureInfo);
    m_textureID = textureInfo.fID;
    RELEASE_ASSERT(m_textureID > 0);
}

void AcceleratedBuffer::waitUntilPaintingComplete()
{
    if (!m_fence)
        return;

    m_fence->wait(WebCore::GLFence::FlushCommands::No);
    m_fence = nullptr;
}
#endif

#if USE(GBM)
Ref<Buffer> GbmBuffer::create(const WebCore::IntSize& size, Flags flags)
{
    return adoptRef(*new GbmBuffer(size, flags));
}

GbmBuffer::GbmBuffer(const WebCore::IntSize& size, Flags flags)
    : Buffer(flags)
    , m_size(size)
    , m_preferredFormat(DRM_FORMAT_ARGB8888)
    , m_linearLayout(true)
    , m_modifier(DRM_FORMAT_MOD_INVALID)
{
    createGbmBuffer();

    const auto checkedArea = size.area() * 4;

    {
        Locker locker { s_layersMemoryUsageLock };
        s_currentLayersMemoryUsage += checkedArea;
        s_maxLayersMemoryUsage = std::max(s_maxLayersMemoryUsage, s_currentLayersMemoryUsage);
    }
}

void GbmBuffer::createGbmBuffer()
{
    auto* device = DRMDeviceManager::singleton().mainGBMDeviceNode(WebCore::DRMDeviceManager::NodeType::Render);
    if (!device) {
        WTFLogAlways("Failed to create GBM buffer of size %dx%d: no GBM device found", m_size.width(), m_size.height());
        return;
    }

    uint64_t* modifiers = NULL;
    int modifiersCount = 0;

    if (m_linearLayout) {
        modifiers = static_cast<uint64_t*>(malloc(sizeof *modifiers));
        if (modifiers) {
            *modifiers = DRM_FORMAT_MOD_LINEAR;
            modifiersCount = 1;
        }
    }

    m_modifier = DRM_FORMAT_MOD_INVALID;
    uint32_t gbm_flags = GBM_BO_USE_RENDERING;
    if (modifiersCount > 0) {
        m_bo = gbm_bo_create_with_modifiers2(device, m_size.width(), m_size.height(), m_preferredFormat, modifiers, modifiersCount, gbm_flags);
        if (m_bo)
            m_modifier = gbm_bo_get_modifier(m_bo);
    }

    if (!m_bo) {
        gbm_flags |= GBM_BO_USE_LINEAR;
        m_bo = gbm_bo_create(device, m_size.width(), m_size.height(), m_preferredFormat, gbm_flags);
    }

    if (!m_bo) {
        WTFLogAlways("Failed to create GBM buffer of size %dx%d: %s", m_size.width(), m_size.height(), safeStrerror(errno).data());
        return;
    }
}

GbmBuffer::~GbmBuffer()
{
    glDeleteTextures(1, &m_textureID);
    m_textureID = 0;

    if (m_bo != nullptr) {
        unmap();
        gbm_bo_destroy(m_bo);
    }

    const auto checkedArea = m_size.area().value() * 4;
    {
        Locker locker { s_layersMemoryUsageLock };
        s_currentLayersMemoryUsage -= checkedArea;
    }
}

unsigned char* GbmBuffer::data()
{
    map();
    return m_data;
}

void GbmBuffer::beginPainting()
{
    Locker locker { m_painting.lock };
    ASSERT(m_painting.state == PaintingState::Complete);
    if (m_data == nullptr)
        map();

    m_painting.state = PaintingState::InProgress;
}

void GbmBuffer::completePainting()
{
    Locker locker { m_painting.lock };
    ASSERT(m_painting.state == PaintingState::InProgress);
    m_painting.state = PaintingState::Complete;
    m_painting.condition.notifyOne();
}

void GbmBuffer::waitUntilPaintingComplete()
{
    Locker locker { m_painting.lock };
    m_painting.condition.wait(m_painting.lock, [this] {
        return m_painting.state == PaintingState::Complete;
    });
    unmap();
}

void GbmBuffer::map()
{
    if (!m_surface) {
        auto imageInfo = SkImageInfo::MakeN32Premul(m_size.width(), m_size.height(), SkColorSpace::MakeSRGB());
        // FIXME: ref buffer and unref on release proc?
        m_data = static_cast<unsigned char*>(gbm_bo_map(m_bo, 0, 0, gbm_bo_get_width(m_bo), gbm_bo_get_height(m_bo), GBM_BO_TRANSFER_READ_WRITE, &m_stride, &m_mapData));
        SkSurfaceProps properties = { 0, WebCore::FontRenderOptions::singleton().subpixelOrder() };
        m_surface = SkSurfaces::WrapPixels(imageInfo, m_data, m_stride, &properties);
    }
}

void GbmBuffer::unmap()
{
    if (m_mapData != nullptr) {
        gbm_bo_unmap(m_bo, m_mapData);
        m_mapData = nullptr;
        m_data = nullptr;
        m_surface.reset();
    }
}

void GbmBuffer::createTexture()
{
    if (!m_textureID) {
        Vector<UnixFileDescriptor> fds;
        Vector<uint32_t> offsets;
        Vector<uint32_t> strides;
        uint32_t format = gbm_bo_get_format(m_bo);
        int planeCount = gbm_bo_get_plane_count(m_bo);

        Vector<EGLAttrib> attributes = {
            EGL_WIDTH, static_cast<EGLAttrib>(gbm_bo_get_width(m_bo)),
            EGL_HEIGHT, static_cast<EGLAttrib>(gbm_bo_get_height(m_bo)),
            EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLAttrib>(format),
        };

#define ADD_PLANE_ATTRIBUTES(planeIndex) { \
        fds.append(UnixFileDescriptor { gbm_bo_get_fd_for_plane(m_bo, planeIndex), UnixFileDescriptor::Adopt }); \
        offsets.append(gbm_bo_get_offset(m_bo, planeIndex)); \
        strides.append(gbm_bo_get_stride_for_plane(m_bo, planeIndex)); \
        std::array<EGLAttrib, 6> planeAttributes { \
            EGL_DMA_BUF_PLANE##planeIndex##_FD_EXT, fds.last().value(), \
            EGL_DMA_BUF_PLANE##planeIndex##_OFFSET_EXT, static_cast<EGLAttrib>(offsets.last()), \
            EGL_DMA_BUF_PLANE##planeIndex##_PITCH_EXT, static_cast<EGLAttrib>(strides.last()) \
        }; \
        attributes.append(std::span<const EGLAttrib> { planeAttributes }); \
        if (m_modifier != DRM_FORMAT_MOD_INVALID) { \
            std::array<EGLAttrib, 4> modifierAttributes { \
                EGL_DMA_BUF_PLANE##planeIndex##_MODIFIER_HI_EXT, static_cast<EGLAttrib>(m_modifier >> 32), \
                EGL_DMA_BUF_PLANE##planeIndex##_MODIFIER_LO_EXT, static_cast<EGLAttrib>(m_modifier & 0xffffffff) \
            }; \
            attributes.append(std::span<const EGLAttrib> { modifierAttributes }); \
        } \
        }

        if (planeCount > 0)
            ADD_PLANE_ATTRIBUTES(0);
        if (planeCount > 1)
            ADD_PLANE_ATTRIBUTES(1);
        if (planeCount > 2)
            ADD_PLANE_ATTRIBUTES(2);
        if (planeCount > 3)
            ADD_PLANE_ATTRIBUTES(3);

#undef ADD_PLANE_ATTRIBS

        attributes.append(EGL_NONE);

        auto& display = WebCore::PlatformDisplay::sharedDisplayForCompositing();
        auto image = display.createEGLImage(EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);

        if (!image) {
            WTFLogAlways("Failed to create EGL image for DMABufs with size %dx%d", m_size.width(), m_size.height());
            return;
        }

        glGenTextures(1, &m_textureID);
        glBindTexture(GL_TEXTURE_2D, m_textureID);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    }
}
#endif

Buffer::Buffer(Flags flags)
    : m_flags(flags)
{
}

Buffer::~Buffer() = default;

} // namespace Nicosia
