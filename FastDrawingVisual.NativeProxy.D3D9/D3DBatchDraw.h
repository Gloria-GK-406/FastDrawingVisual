#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3DBatchDrawTypes.h"

namespace fdv::d3d9::draw {

HRESULT DrawTriangleBatch(const TriangleBatchDrawContext& context,
                          const TriangleVertexData& vertexData);

HRESULT DrawShapeBatch(const InstanceBatchDrawContext& context,
                       const ShapeInstanceData& instanceData);

HRESULT DrawRectBatch(const InstanceBatchDrawContext& context,
                      const RectInstanceData& instanceData);

HRESULT DrawEllipseBatch(const InstanceBatchDrawContext& context,
                         const EllipseInstanceData& instanceData);

HRESULT DrawTextBatch(const TextBatchDrawContext& context,
                      const DrawTextData& textData);

void SetupRenderState(IDirect3DDevice9* device);

} // namespace fdv::d3d9::draw
