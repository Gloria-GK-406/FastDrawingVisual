#include "D3D11CompiledFrame.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace fdv::d3d11::compiled {
namespace {

double DurationMs(const std::chrono::steady_clock::time_point& start,
                  const std::chrono::steady_clock::time_point& end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

void AccumulateBatchCompileStats(batch::BatchCompileStats& destination,
                                 const batch::BatchCompileStats& source) {
  destination.commandCount += source.commandCount;
  destination.triangleVertexCount += source.triangleVertexCount;
  destination.shapeInstanceCount += source.shapeInstanceCount;
  destination.textItemCount += source.textItemCount;
  destination.textCharCount += source.textCharCount;
  destination.imageItemCount += source.imageItemCount;
  destination.imagePixelBytes += source.imagePixelBytes;
  destination.commandReadMs += source.commandReadMs;
  destination.commandBuildMs += source.commandBuildMs;
  destination.commands.clearCount += source.commands.clearCount;
  destination.commands.fillRectCount += source.commands.fillRectCount;
  destination.commands.strokeRectCount += source.commands.strokeRectCount;
  destination.commands.fillEllipseCount += source.commands.fillEllipseCount;
  destination.commands.strokeEllipseCount += source.commands.strokeEllipseCount;
  destination.commands.lineCount += source.commands.lineCount;
  destination.commands.drawTextRunCount += source.commands.drawTextRunCount;
  destination.commands.drawImageCount += source.commands.drawImageCount;
}

HRESULT CopyImageBatch(const std::vector<batch::ImageBatchItem>& sourceItems,
                       CompiledImageBatch& outBatch) {
  outBatch.pixelBlobs.clear();
  outBatch.items.clear();
  outBatch.pixelBlobs.reserve(sourceItems.size());
  outBatch.items.reserve(sourceItems.size());

  for (const auto& sourceItem : sourceItems) {
    if (sourceItem.pixels == nullptr || sourceItem.pixelBytes == 0) {
      batch::ImageBatchItem item = sourceItem;
      item.pixels = nullptr;
      outBatch.items.push_back(item);
      outBatch.pixelBlobs.emplace_back();
      continue;
    }

    outBatch.pixelBlobs.emplace_back(
        sourceItem.pixels,
        sourceItem.pixels + static_cast<std::size_t>(sourceItem.pixelBytes));

    const auto& pixels = outBatch.pixelBlobs.back();
    batch::ImageBatchItem item = sourceItem;
    item.pixels = pixels.empty() ? nullptr : pixels.data();
    outBatch.items.push_back(item);
  }

  return S_OK;
}

} // namespace

HRESULT CompileFrame(int width, int height, const LayeredFramePacket* framePacket,
                     CompiledFrame& outFrame, CompileStats& outStats) {
  outFrame = {};
  outStats = {};

  if (framePacket == nullptr || width <= 0 || height <= 0) {
    return E_INVALIDARG;
  }

  batch::BatchCompiler compiler;
  outFrame.width = width;
  outFrame.height = height;

  for (int layerIndex = 0; layerIndex < LayeredFramePacket::kMaxLayerCount;
       ++layerIndex) {
    const auto& layer = framePacket->layers[layerIndex];
    if (layer.commandData == nullptr || layer.commandBytes <= 0) {
      continue;
    }

    ++outStats.layerCount;
    CompiledLayer compiledLayer{};
    compiledLayer.layerIndex = layerIndex;

    compiler.Reset(width, height, layer.commandData, layer.commandBytes,
                   layer.blobData, layer.blobBytes);

    while (true) {
      batch::CompiledBatchView batchView{};
      const auto compileStart = std::chrono::steady_clock::now();
      const HRESULT batchHr = compiler.TryGetNextBatch(batchView);
      const auto compileEnd = std::chrono::steady_clock::now();
      outStats.compileMs += DurationMs(compileStart, compileEnd);
      AccumulateBatchCompileStats(outStats.aggregate, compiler.lastBatchStats());

      if (batchHr == S_FALSE) {
        break;
      }

      if (FAILED(batchHr)) {
        return batchHr;
      }

      CompiledOp op{};
      switch (batchView.kind) {
      case batch::BatchKind::Clear:
        op.kind = CompiledOpKind::Clear;
        op.clearColor[0] = batchView.clearColor[0];
        op.clearColor[1] = batchView.clearColor[1];
        op.clearColor[2] = batchView.clearColor[2];
        op.clearColor[3] = batchView.clearColor[3];
        ++outStats.clearBatchCount;
        compiledLayer.ops.push_back(op);
        break;

      case batch::BatchKind::Triangles:
        ++outStats.triangleBatchCount;
        outStats.maxTriangleBatchVertexCount =
            (std::max)(outStats.maxTriangleBatchVertexCount,
                       compiler.lastBatchStats().triangleVertexCount);
        return E_NOTIMPL;

      case batch::BatchKind::ShapeInstances: {
        op.kind = CompiledOpKind::ShapeInstances;
        op.payloadIndex =
            static_cast<std::uint32_t>(compiledLayer.shapeBatches.size());
        CompiledShapeBatch batch{};
        batch.items = compiler.GetShapeInstances();
        compiledLayer.shapeBatches.push_back(std::move(batch));
        ++outStats.shapeBatchCount;
        compiledLayer.ops.push_back(op);
        break;
      }

      case batch::BatchKind::Text: {
        op.kind = CompiledOpKind::Text;
        op.payloadIndex =
            static_cast<std::uint32_t>(compiledLayer.textBatches.size());
        CompiledTextBatch batch{};
        batch.items = compiler.GetTextItems();
        outStats.maxTextBatchItemCount =
            (std::max)(outStats.maxTextBatchItemCount,
                       static_cast<int>(batch.items.size()));
        compiledLayer.textBatches.push_back(std::move(batch));
        ++outStats.textBatchCount;
        compiledLayer.ops.push_back(op);
        break;
      }

      case batch::BatchKind::Image: {
        op.kind = CompiledOpKind::Image;
        op.payloadIndex =
            static_cast<std::uint32_t>(compiledLayer.imageBatches.size());
        CompiledImageBatch batch{};
        const HRESULT copyHr = CopyImageBatch(compiler.GetImageItems(), batch);
        if (FAILED(copyHr)) {
          return copyHr;
        }

        outStats.maxImageBatchItemCount =
            (std::max)(outStats.maxImageBatchItemCount,
                       static_cast<int>(batch.items.size()));
        compiledLayer.imageBatches.push_back(std::move(batch));
        ++outStats.imageBatchCount;
        compiledLayer.ops.push_back(op);
        break;
      }

      default:
        return E_INVALIDARG;
      }
    }

    if (!compiledLayer.ops.empty()) {
      outFrame.layers.push_back(std::move(compiledLayer));
    }
  }

  return S_OK;
}

} // namespace fdv::d3d11::compiled