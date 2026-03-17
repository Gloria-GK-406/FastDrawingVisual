#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3DBatchDrawTypes.h"

namespace fdv::d3d11::draw {

HRESULT DrawTriangleBatch(TriangleBatchDrawContext& context,
                          const TriangleVertexData& vertexData,
                          TriangleBatchDrawStats* stats = nullptr);

HRESULT DrawShapeInstanceBatch(InstanceBatchDrawContext& context,
                               const ShapeInstanceData& instanceData,
                               InstanceBatchDrawStats* stats = nullptr);

HRESULT DrawRectInstanceBatch(InstanceBatchDrawContext& context,
                              const RectInstanceData& instanceData,
                              InstanceBatchDrawStats* stats = nullptr);

HRESULT DrawEllipseInstanceBatch(InstanceBatchDrawContext& context,
                                 const EllipseInstanceData& instanceData,
                                 InstanceBatchDrawStats* stats = nullptr);

} // namespace fdv::d3d11::draw
