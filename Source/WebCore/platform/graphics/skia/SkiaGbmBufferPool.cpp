/*
 * Copyright (C) 2024 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "SkiaGbmBufferPool.h"

#if USE(COORDINATED_GRAPHICS) && USE(SKIA) && USE(GBM)
#include "FontRenderOptions.h"
#include "GLContext.h"
#include "PlatformDisplay.h"

namespace WebCore {

SkiaGbmBufferPool::SkiaGbmBufferPool()
    : m_releaseUnusedBuffersTimer(RunLoop::main(), this, &SkiaGbmBufferPool::releaseUnusedBuffersTimerFired)
{
}

SkiaGbmBufferPool::~SkiaGbmBufferPool()
{
    if (m_buffers.isEmpty())
        return;

    //if (!PlatformDisplay::sharedDisplayForCompositing().skiaGLContext()->makeContextCurrent())
    //    return;

    m_buffers.clear();
}

RefPtr<Nicosia::Buffer> SkiaGbmBufferPool::acquireBuffer(const IntSize& size, bool supportsAlpha)
{
    Entry* selectedEntry = std::find_if(m_buffers.begin(), m_buffers.end(), [&](Entry& entry) {
        return entry.m_buffer->refCount() == 1 && entry.m_buffer->size() == size && entry.m_buffer->supportsAlpha() == supportsAlpha;
    });

    if (selectedEntry == m_buffers.end()) {
        auto buffer = createGbmBuffer(size, supportsAlpha);
        if (!buffer)
            return nullptr;

        m_buffers.append(Entry(Ref { *buffer }));
        selectedEntry = &m_buffers.last();
    }

    scheduleReleaseUnusedBuffers();
    selectedEntry->markIsInUse();
    return selectedEntry->m_buffer.ptr();
}

RefPtr<Nicosia::Buffer> SkiaGbmBufferPool::createGbmBuffer(const IntSize& size, bool supportsAlpha)
{
    //auto* grContext = PlatformDisplay::sharedDisplayForCompositing().skiaGrContext();
    //RELEASE_ASSERT(grContext);
    return Nicosia::GbmBuffer::create(size, supportsAlpha ? Nicosia::Buffer::SupportsAlpha : Nicosia::Buffer::NoFlags);
}

void SkiaGbmBufferPool::scheduleReleaseUnusedBuffers()
{
    if (m_releaseUnusedBuffersTimer.isActive())
        return;

    static const Seconds releaseUnusedBuffersTimerInterval { 500_ms };
    m_releaseUnusedBuffersTimer.startOneShot(releaseUnusedBuffersTimerInterval);
}

void SkiaGbmBufferPool::releaseUnusedBuffersTimerFired()
{
    if (m_buffers.isEmpty())
        return;

    // Delete entries, which have been unused in releaseUnusedSecondsTolerance.
    static const Seconds releaseUnusedSecondsTolerance { 5_s };
    MonotonicTime minUsedTime = MonotonicTime::now() - releaseUnusedSecondsTolerance;

    //if (PlatformDisplay::sharedDisplayForCompositing().skiaGLContext()->makeContextCurrent()) {
        m_buffers.removeAllMatching([&minUsedTime](const Entry& entry) {
            return entry.canBeReleased(minUsedTime);
        });
    //}

    if (!m_buffers.isEmpty())
        scheduleReleaseUnusedBuffers();
}

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS) && USE(SKIA) && USE(GBM)
