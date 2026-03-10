# 渲染架构重构计划书

## 1. 背景与问题

当前渲染架构以“最终产品形态”作为主要分层轴，典型表现为：

- `FastDrawingVisual.DCompD3D11`
- `FastDrawingVisual.NativeD3D9`

这种分法在早期验证阶段是可行的，但随着分层绘制、命令 batch 化、能力探测、呈现方式扩展等需求增加，已经暴露出明显问题：

- 命令录制逻辑在 D3D9 和 D3D11 两条路径中高度重复。
- latest-wins 提交流程在两条路径中高度重复。
- 呈现宿主方式与 GPU 后端实现被绑定在同一个 renderer 类型中。
- “D3D9/D3D11 差异”和“D3DImage/DComp 差异”这两个不同维度被混合在一起。
- 后续若支持 `DComp + D3D9`、`D3DImage + D3D11` 等组合，当前结构会导致代码继续横向复制。

结论：

- 当前主要问题不是某个实现细节错误，而是架构切分轴不合理。
- 需要把“命令层、调度层、呈现层、硬件执行层”明确拆开。

## 2. 重构目标

本次重构的目标不是简单重命名或搬文件，而是建立可组合、可协商、可渐进迁移的渲染架构。

目标如下：

- 将命令录制和 frame 提交流程收敛为共享组件。
- 将呈现宿主能力从具体 D3D 后端中分离出来。
- 将 D3D9 与 D3D11 的差异收敛到“硬件执行层”。
- 用能力接口协商替代巨型统一接口。
- 让渲染组合由“能力匹配”决定，而不是由硬编码类型名决定。
- 消除 `DCompD3D11`、`NativeD3D9` 这种复合命名成为核心架构实体的现状。

非目标：

- 本计划不要求一次性重写全部 native 代码。
- 本计划不要求立即支持所有理论组合。
- 本计划不要求 WPF fallback 路径参与硬件能力协商。

## 3. 目标分层

重构后建议形成以下四层结构。

### 3.1 命令层

职责：

- 提供统一的 `IDrawingContext` 基础 API。
- 提供可选的 `ILayeredDrawingContextContainer` 能力。
- 负责命令录制、layer bucket、batch barrier 语义。
- 负责 frame packet 构建。

这一层不应关心：

- 使用 D3D9 还是 D3D11。
- 最终呈现在 `D3DImage`、`HWND` 还是 `DComp`。

建议产物：

- `LayeredCommandRecordingContext`
- `LayerCommandWriter`
- `LayeredFramePacket`

### 3.2 调度层

职责：

- latest-wins 提交模型。
- draw delegate 执行线程调度。
- pending frame 替换策略。
- 设备可用性检查后的触发与丢帧策略。

这一层本质上是：

- “何时执行录制”
- “何时提交 frame”

而不是：

- “如何绘制到 D3D9”
- “如何挂到 DComp”

建议产物：

- `RenderSubmissionScheduler`
- `LatestWinsRenderWorker`

### 3.3 呈现层

职责：

- 将硬件层生成的呈现资源挂接到宿主环境。
- 处理 `D3DImage`、`HWND`、`DComp` 等不同呈现方式。
- 处理 front buffer、host handle、sprite rect、swap chain 绑定等宿主相关行为。

这一层应只关心：

- 宿主需要什么类型的呈现资源。
- 如何把后端资源接到宿主上。

建议产物：

- `D3DImagePresenter`
- `DCompPresenter`
- 未来可选的 `HwndPresenter`

### 3.4 硬件执行层

职责：

- 接收命令 frame。
- 在 GPU 侧解析并执行命令。
- 维护 D3D9 或 D3D11 设备资源。
- 提供呈现层所需的底层资源能力。

这一层是唯一真正应该保留 D3D9 / D3D11 差异的地方。

建议产物：

- `D3D9Backend`
- `D3D11Backend`

## 4. 新的核心设计原则

### 4.1 不再以复合 Renderer 类型作为核心架构实体

重构后的核心组合不再是：

- `DCompD3D11Renderer`
- `NativeD3D9Renderer`

而应是：

- 一个命令录制与提交链路
- 一个 presenter
- 一个 backend

最终 renderer 只是组合产物，而不是架构边界本身。

### 4.2 不设计一个巨型统一 D3D 接口

不建议设计一个“所有 backend 都必须实现”的臃肿接口。

原因：

- D3D9 与 D3D11 在设备资源、surface 类型、present 方式上存在天然差异。
- 强制统一会把最小公倍数做得过大，形成新的坏味道。

正确做法：

- 只保留很小的基础身份接口。
- 通过多个能力接口表达具体能力。
- 由组合器按能力接口进行协商。

### 4.3 通过能力协商决定组合，而不是通过类型硬编码决定组合

组合器不应该写死：

- `if DComp then must use D3D11`
- `if D3DImage then must use D3D9`

而应改为：

- presenter 声明自己需要哪些能力接口
- backend 声明自己提供哪些能力接口
- 组合器只做能力匹配

匹配成功则组合成立。
匹配失败则说明该组合当前不支持。

这属于正常 capability miss，不应视为异常设计。

## 5. 硬件层接口重设计

### 5.1 基础接口

建议保留一个非常小的基础接口，例如：

```csharp
public interface IRenderBackend : IDisposable
{
    bool Initialize(int width, int height);
    void Resize(int width, int height);
}
```

基础接口只保留生命周期和最基本控制能力。

### 5.2 命令消费能力接口

建议单独拆出命令消费能力：

```csharp
public interface ILayeredFrameSink
{
    void SubmitFrame(in LayeredFramePacket frame);
}
```

意义：

- 命令层和调度层只需要依赖它。
- 不需要知道 backend 是 D3D9 还是 D3D11。

### 5.3 呈现资源能力接口

不同 presenter 依赖不同类型的呈现资源，应拆成独立接口。

示意：

```csharp
public interface ID3D9SurfaceProvider
{
    IntPtr GetSurface9();
}

public interface IDXGISwapChainProvider
{
    IntPtr GetSwapChain();
}
```

未来若有共享纹理路径，也应独立接口表达，而不是塞入基础接口。

### 5.4 能力协商接口

建议提供统一能力查询入口，避免业务代码到处做裸类型转换。

示意：

```csharp
public interface ICapabilityProvider
{
    bool TryGetCapability<TCapability>(out TCapability? capability)
        where TCapability : class;
}
```

说明：

- 内部仍然可以通过接口实现来表达能力。
- 对外统一通过 `TryGetCapability` 协商，便于工厂、日志、调试和诊断。

## 6. 呈现层桥接设计

呈现层不应持有具体 backend 类型，而应只依赖自己需要的能力接口。

### 6.1 D3DImagePresenter

需要能力：

- `ID3D9SurfaceProvider`

职责：

- 绑定 `D3DImage`
- 处理 front buffer 可用性
- 触发 dirty rect
- 处理 back buffer 重新绑定

### 6.2 DCompPresenter

需要能力：

- `IDXGISwapChainProvider`

职责：

- 建立 desktop target
- 绑定 swap chain
- 更新 sprite rect
- 处理 host handle 生命周期

### 6.3 组合规则

presenter 组合 backend 时：

- 只尝试请求其所需能力接口
- 请求失败则该组合不可用
- 不以 backend 具体类名做判断

## 7. 合并 `DCompD3D11` 与 `NativeD3D9` 的方向

### 7.1 不是简单“删项目”，而是拆维度后重组

本计划中的“合并”并不是把两个项目粗暴并到一起，而是把它们内部混合的职责拆开后重组：

- 共享命令录制
- 共享 latest-wins 调度
- 独立 presenter
- 独立 backend

### 7.2 预期结果

最终不再存在以下核心架构概念：

- `DCompD3D11` 作为主要边界
- `NativeD3D9` 作为主要边界

取而代之的是：

- `Rendering.Command`
- `Rendering.Submission`
- `Rendering.Presentation`
- `Rendering.Backends.D3D9`
- `Rendering.Backends.D3D11`

### 7.3 产品层仍可保留预设组合名

虽然内部不再以复合 renderer 类型为主，但对外仍可以保留预设组合，例如：

- `Auto`
- `DCompPreferred`
- `D3DImagePreferred`

这些名称只是工厂策略，不再是内部主分层。

## 8. 推荐模块拓扑

建议未来模块边界类似如下。

### 8.1 共享核心

- `FastDrawingVisual.Contracts`
- `FastDrawingVisual.CommandRuntime`
- 可新增 `FastDrawingVisual.RenderingCore`

内容：

- `IDrawingContext`
- `ILayeredDrawingContextContainer`
- frame packet
- 调度组件
- 能力协商接口

### 8.2 呈现模块

- `FastDrawingVisual.Presentation.D3DImage`
- `FastDrawingVisual.Presentation.DComp`

### 8.3 硬件后端模块

- `FastDrawingVisual.Backends.D3D9`
- `FastDrawingVisual.Backends.D3D11`

### 8.4 现有项目演化建议

- `FastDrawingVisual.DCompD3D11` 中的 presenter 相关逻辑迁往 `Presentation.DComp`
- `FastDrawingVisual.DCompD3D11` 中的 D3D11 native 执行逻辑迁往 `Backends.D3D11`
- `FastDrawingVisual.NativeD3D9` 中的 `D3DImage` 绑定逻辑迁往 `Presentation.D3DImage`
- `FastDrawingVisual.NativeD3D9` 中的 D3D9 native 执行逻辑迁往 `Backends.D3D9`

## 9. 与分层绘制 / batch 化的关系

本次架构重构与 layered drawing 和 batch 提交是强相关的。

原因：

- layered drawing 属于命令层能力，不应分别实现在多个复合 renderer 中。
- batch barrier 规则属于命令层与硬件执行层之间的语义协作。
- 若不先回收架构边界，D3D9 与 D3D11 上的 layered/batch 改造会持续复制。

建议：

- 分层绘制能力定义放在共享命令层。
- frame packet 结构放在共享命令层。
- batch 策略的具体执行保留在 backend 中。

## 10. 迁移阶段建议

### Phase 1：命令层收敛

目标：

- 提取共享的 layered recording context
- 提取统一 frame packet 结构
- 从 D3D11 路径开始，去掉临时兼容包装

结果：

- D3D11 与 D3D9 使用相同的命令录制模型

### Phase 2：调度层收敛

目标：

- 抽出 latest-wins worker
- 抽出统一的 pending draw action / signal / worker loop 模型

结果：

- D3D11 与 D3D9 不再各自维护一套 draw worker 逻辑

### Phase 3：呈现层拆分

目标：

- 提取 `D3DImagePresenter`
- 提取 `DCompPresenter`

结果：

- 宿主相关逻辑不再混入 backend

### Phase 4：硬件层接口协商

目标：

- 为 D3D9 与 D3D11 backend 定义能力接口
- 建立组合器和 capability matching

结果：

- backend 与 presenter 可以通过能力协商装配

### Phase 5：清理复合 renderer 结构

目标：

- 逐步废弃 `DCompD3D11` / `NativeD3D9` 作为主要架构边界
- 保留兼容入口，但内部改为组合实现

结果：

- 对外 API 稳定
- 内部结构显著简化

## 11. 风险与约束

### 11.1 理论可组合不等于立即值得支持

例如：

- `D3DImage + D3D11`
- `DComp + D3D9`

从架构上应允许这种组合，但运行时是否值得支持，仍要看 interop 成本和平台约束。

### 11.2 能力协商必须清晰记录日志

若组合失败却没有明确日志，排查会非常困难。

要求：

- 每次能力匹配失败都应输出明确原因
- 记录缺失的是哪一项能力接口

### 11.3 不要在迁移期形成双重抽象

若新老架构长期并存且互相包装，会形成更重的维护负担。

要求：

- 每一阶段都要有明确的旧层清理目标
- 避免无限期保留兼容过渡对象

## 12. 建议结论

建议正式启动本次重构，并按以下原则推进：

- 先统一命令层，再统一调度层
- 呈现层与硬件层通过能力接口组合
- 不设计巨型统一 D3D 接口
- 使用能力协商而不是硬编码类型绑定
- 逐步消解 `DCompD3D11` 与 `NativeD3D9` 这种复合架构实体

最终目标是形成：

- 一个共享的命令录制与 frame 提交体系
- 多个 presenter
- 多个 backend
- 一个基于能力协商的组合装配机制

这将显著改善：

- 代码重复
- 架构清晰度
- 新组合扩展性
- layered drawing / batch 化后续演进效率
