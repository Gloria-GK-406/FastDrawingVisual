#include "BridgeRendererInternal.h"
#include "BridgeCommandProtocol.g.h"

#if !defined(YieldProcessor)
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64) || defined(_M_AMD64))
#include <intrin.h>
#define YieldProcessor() _mm_pause()
#elif defined(_WIN32)
#define YieldProcessor() SwitchToThread()
#else
#define YieldProcessor() ((void)0)
#endif
#endif

static constexpr DWORD kLegacyFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
static constexpr DWORD kSdfFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;
static constexpr float kSdfAaWidthPx = 1.0f;

static inline D3DCOLOR ToD3DColor(const fdv::protocol::ColorArgb8 &color) {
  return D3DCOLOR_ARGB(color.a, color.r, color.g, color.b);
}

static inline float Maxf(float a, float b) { return a > b ? a : b; }
static inline float Minf(float a, float b) { return a < b ? a : b; }

struct Vertex2D {
  float x, y, z, rhw;
  D3DCOLOR color;
};

struct VertexSdf {
  float x, y, z, rhw;
  float u, v;
};

static void SetLegacyPipeline(IDirect3DDevice9 *dev) {
  dev->SetPixelShader(nullptr);
  dev->SetFVF(kLegacyFvf);
}

static bool SetSdfPipeline(IDirect3DDevice9 *dev, IDirect3DPixelShader9 *shader) {
  if (!dev || !shader)
    return false;

  dev->SetPixelShader(shader);
  dev->SetFVF(kSdfFvf);
  return true;
}

void SetupRenderState(IDirect3DDevice9 *dev) {
  dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
  dev->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, FALSE);
  dev->SetTexture(0, nullptr);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

  SetLegacyPipeline(dev);
}

static void WriteColorConstant(D3DCOLOR color, float out[4]) {
  out[0] = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
  out[1] = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
  out[2] = static_cast<float>(color & 0xFF) / 255.0f;
  out[3] = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
}

static void DrawFilledRect(IDirect3DDevice9 *dev, float x, float y, float w,
                           float h, D3DCOLOR color) {
  SetLegacyPipeline(dev);
  Vertex2D verts[6] = {
      {x, y, 0.5f, 1.0f, color},         {x + w, y, 0.5f, 1.0f, color},
      {x, y + h, 0.5f, 1.0f, color},     {x + w, y, 0.5f, 1.0f, color},
      {x + w, y + h, 0.5f, 1.0f, color}, {x, y + h, 0.5f, 1.0f, color},
  };

  dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, verts, sizeof(Vertex2D));
}

static void DrawStrokeRect(IDirect3DDevice9 *dev, float x, float y, float w,
                           float h, float t, D3DCOLOR color) {
  DrawFilledRect(dev, x, y, w, t, color);
  DrawFilledRect(dev, x, y + h - t, w, t, color);
  DrawFilledRect(dev, x, y + t, t, h - 2 * t, color);
  DrawFilledRect(dev, x + w - t, y + t, t, h - 2 * t, color);
}

static bool DrawSdfQuad(IDirect3DDevice9 *dev, float left, float top,
                        float right, float bottom) {
  VertexSdf verts[6] = {
      {left, top, 0.5f, 1.0f, left, top},
      {right, top, 0.5f, 1.0f, right, top},
      {left, bottom, 0.5f, 1.0f, left, bottom},
      {right, top, 0.5f, 1.0f, right, top},
      {right, bottom, 0.5f, 1.0f, right, bottom},
      {left, bottom, 0.5f, 1.0f, left, bottom},
  };

  return SUCCEEDED(
      dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, verts, sizeof(VertexSdf)));
}

static bool DrawSdfEllipse(BridgeRenderer *s, float cx, float cy, float rx,
                           float ry, float thickness, bool stroke,
                           D3DCOLOR color) {
  if (!s || !s->device || !s->sdfEllipseShader)
    return false;

  IDirect3DDevice9 *dev = s->device;
  if (!SetSdfPipeline(dev, s->sdfEllipseShader))
    return false;

  float halfStroke = stroke ? Maxf(thickness * 0.5f, 0.0f) : 0.0f;
  float safeRx = Maxf(rx, 0.0001f);
  float safeRy = Maxf(ry, 0.0001f);
  float padding = halfStroke + kSdfAaWidthPx + 1.0f;

  float color4[4] = {};
  float params[4] = {halfStroke, kSdfAaWidthPx, 0.0f, 0.0f};
  float data0[4] = {cx, cy, safeRx, safeRy};
  WriteColorConstant(color, color4);

  dev->SetPixelShaderConstantF(0, color4, 1);
  dev->SetPixelShaderConstantF(1, params, 1);
  dev->SetPixelShaderConstantF(2, data0, 1);

  return DrawSdfQuad(dev, cx - safeRx - padding, cy - safeRy - padding,
                     cx + safeRx + padding, cy + safeRy + padding);
}

static bool DrawSdfLine(BridgeRenderer *s, float x0, float y0, float x1,
                        float y1, float thickness, D3DCOLOR color) {
  if (!s || !s->device || !s->sdfLineShader)
    return false;

  IDirect3DDevice9 *dev = s->device;
  if (!SetSdfPipeline(dev, s->sdfLineShader))
    return false;

  float halfStroke = Maxf(thickness * 0.5f, 0.0f);
  float padding = halfStroke + kSdfAaWidthPx + 1.0f;

  float color4[4] = {};
  float params[4] = {halfStroke, kSdfAaWidthPx, 0.0f, 0.0f};
  float data0[4] = {x0, y0, x1, y1};
  WriteColorConstant(color, color4);

  dev->SetPixelShaderConstantF(0, color4, 1);
  dev->SetPixelShaderConstantF(1, params, 1);
  dev->SetPixelShaderConstantF(2, data0, 1);

  float left = Minf(x0, x1) - padding;
  float top = Minf(y0, y1) - padding;
  float right = Maxf(x0, x1) + padding;
  float bottom = Maxf(y0, y1) + padding;
  return DrawSdfQuad(dev, left, top, right, bottom);
}

bool ExecuteCommands(BridgeRenderer *s, SurfaceSlot *slot,
                     const uint8_t *commandData, int commandBytes,
                     const uint8_t *blobData, int blobBytes) {
  IDirect3DDevice9 *dev = s->device;
  if (!dev || !slot || !slot->renderTarget)
    return false;

  HRESULT hr = dev->SetRenderTarget(0, slot->renderTarget);
  if (FAILED(hr))
    return false;

  hr = dev->BeginScene();
  if (FAILED(hr))
    return false;

  SetupRenderState(dev);

  bool commandOk = true;
  fdv::protocol::CommandReader reader(commandData, commandBytes, blobData,
                                      blobBytes);
  fdv::protocol::Command command{};
  while (reader.TryReadNext(command)) {
    switch (command.type) {
    case fdv::protocol::CommandType::Clear: {
      const auto &payload =
          std::get<fdv::protocol::ClearPayload>(command.payload);
      dev->Clear(0, nullptr, D3DCLEAR_TARGET, ToD3DColor(payload.color), 1.0f,
                 0);
      break;
    }

    case fdv::protocol::CommandType::FillRect: {
      const auto &payload =
          std::get<fdv::protocol::FillRectPayload>(command.payload);
      DrawFilledRect(dev, payload.x, payload.y, payload.width, payload.height,
                     ToD3DColor(payload.color));
      break;
    }

    case fdv::protocol::CommandType::StrokeRect: {
      const auto &payload =
          std::get<fdv::protocol::StrokeRectPayload>(command.payload);
      const float thickness = payload.thickness < 1.0f ? 1.0f : payload.thickness;
      DrawStrokeRect(dev, payload.x, payload.y, payload.width, payload.height,
                     thickness, ToD3DColor(payload.color));
      break;
    }

    case fdv::protocol::CommandType::FillEllipse: {
      const auto &payload =
          std::get<fdv::protocol::FillEllipsePayload>(command.payload);
      if (!DrawSdfEllipse(s, payload.centerX, payload.centerY, payload.radiusX,
                          payload.radiusY, 0.0f, false,
                          ToD3DColor(payload.color))) {
        commandOk = false;
        goto done;
      }
      break;
    }

    case fdv::protocol::CommandType::StrokeEllipse: {
      const auto &payload =
          std::get<fdv::protocol::StrokeEllipsePayload>(command.payload);
      if (!DrawSdfEllipse(s, payload.centerX, payload.centerY, payload.radiusX,
                          payload.radiusY, payload.thickness, true,
                          ToD3DColor(payload.color))) {
        commandOk = false;
        goto done;
      }
      break;
    }

    case fdv::protocol::CommandType::Line: {
      const auto &payload =
          std::get<fdv::protocol::LinePayload>(command.payload);
      if (!DrawSdfLine(s, payload.x0, payload.y0, payload.x1, payload.y1,
                       payload.thickness, ToD3DColor(payload.color))) {
        commandOk = false;
        goto done;
      }
      break;
    }

    case fdv::protocol::CommandType::DrawTextRun:
      // NativeD3D9 currently keeps text as a no-op path.
      break;
    }
  }

  if (reader.HasError()) {
    commandOk = false;
    goto done;
  }

done:
  hr = dev->EndScene();
  if (FAILED(hr))
    return false;
  if (!commandOk)
    return false;

  if (slot->renderDoneQuery) {
    hr = slot->renderDoneQuery->Issue(D3DISSUE_END);
    if (FAILED(hr))
      return false;

    while (true) {
      hr = slot->renderDoneQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH);
      if (hr == S_OK)
        break;
      if (hr == S_FALSE) {
        YieldProcessor();
        continue;
      }
      return false;
    }
  }

  return true;
}
