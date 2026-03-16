#pragma once

#include "../FastDrawingVisual.NativeProxy.Shared/BatchTypes.h"

namespace fdv::d3d11::batch {

using BatchKind = fdv::nativeproxy::shared::batch::BatchKind;
using ShapeInstanceType = fdv::nativeproxy::shared::batch::ShapeInstanceType;
using TriangleVertex = fdv::nativeproxy::shared::batch::TriangleVertex;
using ShapeInstance = fdv::nativeproxy::shared::batch::ShapeInstance;
using RectInstance = fdv::nativeproxy::shared::batch::RectInstance;
using EllipseInstance = fdv::nativeproxy::shared::batch::EllipseInstance;
using TextBatchItem = fdv::nativeproxy::shared::batch::TextBatchItem;
using BatchCommandStats = fdv::nativeproxy::shared::batch::BatchCommandStats;
using BatchCompileStats = fdv::nativeproxy::shared::batch::BatchCompileStats;
using CompiledBatchView = fdv::nativeproxy::shared::batch::CompiledBatchView;

} // namespace fdv::d3d11::batch
