#pragma once

#include "../FastDrawingVisual.NativeProxy.Shared/BatchTypes.h"

namespace fdv::d3d11::batch {

using BatchKind = fdv::nativeproxy::shared::batch::BatchKind;
using TriangleVertex = fdv::nativeproxy::shared::batch::TriangleVertex;
using TextBatchItem = fdv::nativeproxy::shared::batch::TextBatchItem;
using BatchCommandStats = fdv::nativeproxy::shared::batch::BatchCommandStats;
using BatchCompileStats = fdv::nativeproxy::shared::batch::BatchCompileStats;
using CompiledBatchView = fdv::nativeproxy::shared::batch::CompiledBatchView;

} // namespace fdv::d3d11::batch
