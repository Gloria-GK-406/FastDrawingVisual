#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3DBatchDrawTypes.h"
#include "TextFormatCacheStore.h"

namespace fdv::d3d11::draw {

HRESULT DrawTriangleBatch(TriangleBatchDrawContext& context,
                          const TriangleVertexData& vertexData);

HRESULT DrawTextBatch(const TextBatchDrawContext& context,
                      TextFormatCacheStore& textFormatCache,
                      const DrawTextData& textData);

} // namespace fdv::d3d11::draw
