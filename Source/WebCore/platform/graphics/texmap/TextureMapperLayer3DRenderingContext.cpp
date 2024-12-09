/*
 * Copyright (C) 2024 Jani Hautakangas <jani@kodegood.com>
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
#include "TextureMapperLayer3DRenderingContext.h"

#include "ClipPath.h"
#include "FloatPlane3D.h"
#include "TextureMapperGPUBuffer.h"
#include "TextureMapperLayer.h"
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TextureMapperLayer3DRenderingContext);

void TextureMapperLayer3DRenderingContext::paint(TextureMapper& textureMapper, const Vector<TextureMapperLayer*>& layers,
    const std::function<void(TextureMapperLayer*, const ClipPath&)>& paintLayerFunction)
{
    if (layers.isEmpty())
        return;

    // Compute bounding volumes. These are distinct areas that can be rendered independently.
    // Bounding volumes help restrict BSP cutting planes to their respective areas, preventing
    // unnecessary splitting of layers that are spatially distant but would otherwise intersect the cutting plane.
    Vector<BoundingVolume> boundingVolumes;
    for (auto* layer : layers) {
        TextureMapperLayerPolygon layerPolygon({ layer->effectiveLayerRect(), layer->toSurfaceTransform() }, layer, false);
        auto layerBoundingBox = computeBoundingBoxForLayer(layerPolygon);

        bool addedToExistingBoundingVolume = false;

        for (auto& boundingVolume : boundingVolumes) {
            if (boundingVolume.boundingBox.intersects(layerBoundingBox)) {
                boundingVolume.boundingBox.extend(layerBoundingBox);
                boundingVolume.layerList.append(layerPolygon);
                addedToExistingBoundingVolume = true;
                break;
            }
        }

        if (!addedToExistingBoundingVolume) {
            BoundingVolume newBoundingVolume;
            newBoundingVolume.boundingBox = layerBoundingBox;
            newBoundingVolume.layerList.append(layerPolygon);
            boundingVolumes.append(WTFMove(newBoundingVolume));
        }
    }

    depthSortBoundingVolumes(boundingVolumes);

    // Build tree and collect clip data for all bounding volumes
    Vector<float> clipVertices;
    for (auto& boundingVolume : boundingVolumes) {
        boundingVolume.bspRoot = makeUnique<TextureMapperLayerNode>(boundingVolume.layerList.takeFirst());
        buildTree(*boundingVolume.bspRoot, boundingVolume.layerList);

        traverseTree(*boundingVolume.bspRoot, [&clipVertices](TextureMapperLayerNode& node) {
            for (auto& polygon : node.polygons) {
                auto toLayerTransform = polygon.layer->toSurfaceTransform().inverse();
                if (polygon.isSplitted && toLayerTransform) {
                    polygon.clipVertexBufferOffset = clipVertices.size();

                    unsigned numVertices = polygon.geometry.numberOfVertices();
                    for (unsigned i = 0; i < numVertices; i++) {
                        auto v = toLayerTransform->mapPoint(polygon.geometry.vertexAt(i));
                        clipVertices.append(v.x());
                        clipVertices.append(v.y());
                    }
                }
            }
        });
    }

    unsigned clipBufferSize = clipVertices.size() * sizeof(float);
    auto clipBuffer = textureMapper.acquireBufferFromPool(clipBufferSize, TextureMapperGPUBuffer::Type::Vertex);
    clipBuffer->updateData(clipVertices.data(), 0, clipBufferSize);

    // Paint
    for (auto& boundingVolume : boundingVolumes) {
        if (boundingVolume.layerList.size() > 1)
            continue;
        traverseTree(*boundingVolume.bspRoot, [&clipVertices, &clipBuffer, &paintLayerFunction](TextureMapperLayerNode& node) {
            for (auto& polygon : node.polygons) {
                unsigned numberOfClipVertices = polygon.isSplitted ? polygon.geometry.numberOfVertices() : 0;

                Vector<FloatPoint> points;
                if (numberOfClipVertices > 0) {
                    points.reserveCapacity(numberOfClipVertices);
                    auto xy = clipVertices.subvector(polygon.clipVertexBufferOffset, numberOfClipVertices * 2);
                    for (size_t i = 0; i < xy.size(); i += 2)
                        points.append(FloatPoint(xy.at(i), xy.at(i + 1)));
                }

                ClipPath clipPath(WTFMove(points), clipBuffer->bufferID(), polygon.clipVertexBufferOffset * sizeof(float));

                paintLayerFunction(polygon.layer, clipPath);
            }
        });
    }
}

TextureMapperLayer3DRenderingContext::AxisAlignedBoundingBox
TextureMapperLayer3DRenderingContext::computeBoundingBoxForLayer(const TextureMapperLayerPolygon& layerPolygon)
{
    FloatPoint3D minCorner =  layerPolygon.geometry.vertexAt(0);
    FloatPoint3D maxCorner = layerPolygon.geometry.vertexAt(0);

    unsigned numVertices = layerPolygon.geometry.numberOfVertices();
    for (unsigned i = 1; i < numVertices; i++) {
        auto point = layerPolygon.geometry.vertexAt(i);
        minCorner.setX(std::min(minCorner.x(), point.x()));
        minCorner.setY(std::min(minCorner.y(), point.y()));
        minCorner.setZ(std::min(minCorner.z(), point.z()));

        maxCorner.setX(std::max(maxCorner.x(), point.x()));
        maxCorner.setY(std::max(maxCorner.y(), point.y()));
        maxCorner.setZ(std::max(maxCorner.z(), point.z()));
    }

    return { minCorner, maxCorner };
}

void TextureMapperLayer3DRenderingContext::depthSortBoundingVolumes(Vector<BoundingVolume>& boundingVolumes)
{
    static auto computeCenter = [](const AxisAlignedBoundingBox& aabb) {
        return FloatPoint3D(
            (aabb.minCorner.x() + aabb.maxCorner.x()) / 2,
            (aabb.minCorner.y() + aabb.maxCorner.y()) / 2,
            (aabb.minCorner.z() + aabb.maxCorner.z()) / 2
        );
    };

    std::sort(boundingVolumes.begin(), boundingVolumes.end(), [](const BoundingVolume& a, const BoundingVolume& b) {
        FloatPoint3D centerA = computeCenter(a.boundingBox);
        FloatPoint3D centerB = computeCenter(b.boundingBox);

        return centerA.z() < centerB.z();
    });
}

// Build BSP tree for rendering polygons with painter's algorithm.
// For more information:
// https://en.wikipedia.org/wiki/Binary_space_partitioning
void TextureMapperLayer3DRenderingContext::buildTree(TextureMapperLayerNode& root, Deque<TextureMapperLayerPolygon>& polygons)
{
    if (polygons.isEmpty())
        return;

    auto& rootGeometry = root.firstPolygon().geometry;
    FloatPlane3D rootPlane(rootGeometry.normal(), rootGeometry.vertexAt(0));

    Deque<TextureMapperLayerPolygon> backList, frontList;
    for (auto& polygon : polygons) {
        switch (classifyPolygon(polygon, rootPlane)) {
        case PolygonPosition::InFront:
            frontList.append(WTFMove(polygon));
            break;
        case PolygonPosition::Behind:
            backList.append(WTFMove(polygon));
            break;
        case PolygonPosition::Coplanar:
            root.polygons.append(WTFMove(polygon));
            break;
        case PolygonPosition::Intersecting:
            auto [backGeometry, frontGeometry] = polygon.geometry.split(rootPlane);
            if (backGeometry.numberOfVertices() > 2)
                backList.append({ backGeometry, polygon.layer, true });
            if (frontGeometry.numberOfVertices() > 2)
                frontList.append({ frontGeometry, polygon.layer, true });
            break;
        }
    }

    if (!frontList.isEmpty()) {
        root.frontNode = makeUnique<TextureMapperLayerNode>(frontList.takeFirst());
        buildTree(*root.frontNode, frontList);
    }

    if (!backList.isEmpty()) {
        root.backNode = makeUnique<TextureMapperLayerNode>(backList.takeFirst());
        buildTree(*root.backNode, backList);
    }
}

void TextureMapperLayer3DRenderingContext::traverseTree(TextureMapperLayerNode& node, const std::function<void(TextureMapperLayerNode&)>& processNode)
{
    auto& geometry = node.firstPolygon().geometry;
    FloatPlane3D plane(geometry.normal(), geometry.vertexAt(0));

    auto* frontNode = node.frontNode.get();
    auto* backNode = node.backNode.get();

    // if polygon is facing away from camera then swap nodes to reverse
    // the traversal order
    if (plane.normal().z() < 0)
        std::swap(frontNode, backNode);

    if (backNode)
        traverseTree(*backNode, processNode);

    processNode(node);

    if (frontNode)
        traverseTree(*frontNode, processNode);
}

TextureMapperLayer3DRenderingContext::PolygonPosition TextureMapperLayer3DRenderingContext::classifyPolygon(const TextureMapperLayerPolygon& polygon, const FloatPlane3D& plane)
{
    const float epsilon = 0.05f; // Tolerance for intersection check

    int inFrontCount = 0;
    int behindCount = 0;
    for (unsigned i = 0; i < polygon.geometry.numberOfVertices(); i++) {
        const auto& vertex = polygon.geometry.vertexAt(i);
        float distance = plane.distanceToPoint(vertex);

        if (distance > epsilon)
            inFrontCount++;
        else if (distance < -epsilon)
            behindCount++;
    }

    if (inFrontCount > 0 && behindCount > 0)
        return PolygonPosition::Intersecting;
    if (inFrontCount > 0)
        return PolygonPosition::InFront;
    if (behindCount > 0)
        return PolygonPosition::Behind;
    return PolygonPosition::Coplanar;
}

} // namespace WebCore
