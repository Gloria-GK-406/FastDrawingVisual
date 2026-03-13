#include "D3DBatchDraw.h"

namespace fdv::d3d9::draw {

void SetupRenderState(IDirect3DDevice9* device) {
  if (device == nullptr) {
    return;
  }

  device->SetRenderState(D3DRS_ZENABLE, FALSE);
  device->SetRenderState(D3DRS_LIGHTING, FALSE);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
  device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
  device->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, FALSE);
  device->SetTexture(0, nullptr);
  device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DISABLE);
  device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

HRESULT DrawTriangleBatch(const TriangleBatchDrawContext& context,
                          const TriangleVertexData& vertexData) {
  if (vertexData.vertices == nullptr || vertexData.vertexCount <= 0) {
    return S_OK;
  }

  if (context.device == nullptr || context.vertexDeclaration == nullptr ||
      context.vertexShader == nullptr || context.pixelShader == nullptr) {
    return E_POINTER;
  }

  if ((vertexData.vertexCount % 3) != 0) {
    return E_INVALIDARG;
  }

  context.device->SetVertexDeclaration(context.vertexDeclaration);
  context.device->SetVertexShader(context.vertexShader);
  context.device->SetPixelShader(context.pixelShader);

  const UINT primitiveCount = static_cast<UINT>(vertexData.vertexCount / 3);
  const HRESULT hr = context.device->DrawPrimitiveUP(
      D3DPT_TRIANGLELIST, primitiveCount, vertexData.vertices,
      sizeof(batch::TriangleVertex));
  return FAILED(hr) ? hr : S_OK;
}

HRESULT DrawTextBatch(const TextBatchDrawContext& context,
                      const DrawTextData& textData) {
  static_cast<void>(context);
  static_cast<void>(textData);
  return S_OK;
}

} // namespace fdv::d3d9::draw
