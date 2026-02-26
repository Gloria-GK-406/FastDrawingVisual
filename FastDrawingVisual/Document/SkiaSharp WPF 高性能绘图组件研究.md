# **基于SkiaSharp、D3DImage与WriteableBitmap的.NET 6 WPF高性能绘图组件架构研究**

## **现代WPF图形架构的演进与挑战**

Windows Presentation Foundation (WPF) 的核心渲染引擎（milcore）自设计之初便依赖于保留模式（Retained-mode）图形系统，该系统通过构建复杂的视觉树并将指令序列化后交由DirectX 9进行硬件加速 1。这种架构在处理标准业务界面时表现出极高的便利性与数据绑定优势。然而，在面对需要极高刷新率、海量几何图元渲染（例如实时动态图表、百万级节点渲染或复杂GIS地图系统）的场景时，原生的 DrawingVisual 和 Shape 对象的性能瓶颈便会彻底暴露。当图元数量达到数十万级别时，UI线程的CPU占用率将迅速饱和，导致严重的帧率下降与界面冻结 3。

为了突破WPF底层组合引擎的限制，业界广泛采用立即模式（Immediate-mode）渲染策略。SkiaSharp 作为Google Skia图形库的跨平台.NET封装，凭借其强大的2D矢量与光栅化渲染能力，成为了替代WPF原生绘图API的首选方案 5。在最新的.NET 6 环境下，如何将SkiaSharp的高性能绘制能力与WPF的生命周期完美融合，同时满足异步多线程渲染、GPU硬件加速、以及在旧版操作系统（如不支持D3D11的Windows 7）上的无缝CPU降级，是一项极具挑战性的架构工程。

本研究报告深入剖析并提出了一种基于 FrameworkElement 封装的自定义高性能绘图组件的完整实现架构。该架构严格满足以下五大核心需求：

1. 提供与原生 DrawingVisual 高度一致的API体验，并原生支持在 Task.Run 线程池中进行 OpenRender 与绘制，实现真正的渲染与UI分离。  
2. 具备智能降级机制，在Windows 7等不支持Direct3D 11的环境或远程桌面（RDP）环境中，自动切回纯CPU绘制的 WriteableBitmap 模式。  
3. 架构全面适配并优化于.NET 6 WPF 运行环境。  
4. 组件内部严格封装视觉宿主，对外仅暴露单一的 FrameworkElement，内部自动路由 D3DImage 或 WriteableBitmap。  
5. 在GPU渲染模式下，强制实施三重缓冲（Triple Buffering）机制，彻底消除UI线程与后台渲染线程之间的锁竞争与帧同步阻塞。

## **API设计：基于多线程环境的 DrawingVisual 仿真**

原生WPF的 DrawingVisual 提供了一种极其优雅的绘图模式：开发者通过调用 RenderOpen() 获取一个 DrawingContext，在上下文中发出所有绘图指令，最终在释放上下文时将指令提交给渲染树 7。然而，WPF的强制线程亲和性（Thread Affinity）要求所有对 DrawingContext 的操作必须在创建该对象的UI调度器（Dispatcher）线程上执行 8。为了在后台 Task.Run 中执行密集型绘图操作，必须对SkiaSharp的上下文进行深度抽象。

### **线程安全的独立上下文策略**

SkiaSharp 的核心对象 SKSurface 和 SKCanvas 默认并非线程安全，多个线程不可同时操作同一个画布实例 9。为了在保持 DrawingVisual 风格API的同时实现线程安全，组件需要实现一个自定义的 ISkiaDrawingContext 接口。

当业务代码在 Task.Run 中调用类似 skiaElement.RenderOpen() 的方法时，架构内部并非返回WPF的 DrawingContext，而是执行以下操作：

1. **缓冲池检出**：系统从底层的三重缓冲池中检出一个当前处于“可用（Available）”状态的非托管内存块或独立GPU纹理。  
2. **上下文封装**：针对该独立的内存块或纹理，实时实例化一个关联的 SKSurface 和 SKCanvas，并将其包装在 ISkiaDrawingContext 中返回 10。  
3. **离屏异步绘制**：由于该 SKSurface 在当前帧的生命周期内被当前 Task.Run 线程独占，开发者可以安全地调用所有 SkiaSharp 的高消耗绘制API（如抗锯齿路径填充、复杂文本排版等），而无需担心任何多线程锁竞争 9。

### **延迟执行与上下文提交**

符合 IDisposable 模式的上下文设计是实现帧提交的关键。当业务代码完成绘制并退出 using 代码块（即触发 Dispose）时，ISkiaDrawingContext 会触发内部的提交流程：

* 调用 SKCanvas.Flush()，强制将所有缓存的绘制指令推送至硬件管线或完成CPU光栅化 11。  
* 将当前缓冲区的状态原子性地标记为“就绪（Ready）”。  
* 向WPF的UI线程 Dispatcher 异步发送一个极轻量级的帧更新信号，通知前端视觉树获取最新帧 12。

这种设计使得耗时极长的绘图指令（可能长达数十毫秒）完全在后台线程池中并行消化，UI线程仅在毫秒级的缓冲交换时被唤醒，从而确保了即使在繁重的绘图任务下，WPF应用程序的交互响应率依然能够维持在最高水平。

## **组件封装：基于 FrameworkElement 的视觉树管理**

为了向消费端隐藏底层的DirectX互操作、内存拷贝与三重缓冲状态机，所有复杂逻辑均需内聚于一个继承自 System.Windows.FrameworkElement 的自定义控件中 13。

### **宿主隔离与生命周期编排**

FrameworkElement 提供了参与WPF布局与事件系统的最基础能力，但其自身并不具备渲染像素的能力。因此，在组件的内部逻辑中，需要动态维护一个 System.Windows.Controls.Image 控件作为实际的像素宿主 14。

| 方法/事件 | 架构响应逻辑 | 机制与目的 |
| :---- | :---- | :---- |
| VisualChildrenCount | 返回固定值 1 | 告知WPF逻辑树该组件拥有唯一的内部子元素（即Image宿主）15。 |
| GetVisualChild(index) | 返回内部的 Image 实例 | 将Image挂载至视觉树，使其接受WPF组合引擎的最终渲染 15。 |
| MeasureOverride | 返回可用空间大小 | 确保绘图组件能够正确响应网格与容器的动态缩放请求 16。 |
| ArrangeOverride | 将Image撑满整个控件区域 | 维持后台渲染分辨率与前端呈现物理尺寸的1:1映射。 |
| Loaded 事件 | 初始化图形硬件设备 | 在组件确实被挂载到视觉树后，才开始分配昂贵的D3D11上下文或非托管内存 17。 |
| Unloaded 事件 | 深度清理所有非托管资源 | 释放COM指针、销毁DXGI句柄、清空 GRContext，防止严重的内存泄漏 17。 |

通过在 SizeChanged 事件中侦听物理尺寸的变更，组件可以精确地销毁并重建底层的三重缓冲池。这一重建过程必须是线程安全的，需在短暂停顿后台 Task.Run 的前提下，重新申请对应新分辨率的GPU纹理或内存块，再恢复渲染泵 19。

## **GPU加速管道：D3D11与D3D9的极致互操作**

在支持硬件加速的现代Windows系统上，为了将SkiaSharp的GPU输出桥接至WPF，必须跨越一条历史遗留的API鸿沟。WPF的 D3DImage 类专门用于将原生DirectX表面接入WPF的保留模式管道，但它存在一个严苛的限制：它仅接受 Direct3D 9 (IDirect3DSurface9) 的指针 20。而SkiaSharp在Windows上依赖于更高性能的 OpenGL (通过ANGLE转换) 或 Direct3D 11/12 甚至 Vulkan 后端进行硬件级光栅化 21。

### **DXGI 跨设备表面共享机制**

架构的突破点在于利用 Windows Display Driver Model (WDDM) 提供的 DXGI 表面共享机制（DXGI Surface Sharing）。该机制允许 Direct3D 11 创建一个特殊的纹理，并将其内存句柄暴露给 Direct3D 9Ex 设备，从而实现跨API的“零拷贝（Zero-Copy）”内存共享 23。

在.NET 6 环境中，实施此硬件加速管道的步骤极其精密：

1. **设备并发初始化**：通过互操作库（如 Silk.NET 或 SharpDX 的分叉版本），同时创建一个 D3D11Device 和一个 Direct3D9Ex 设备。为了防止WPF渲染线程与SkiaSharp后台线程发生冲突，D3D9Ex 设备在创建时必须严格显式传递 D3DCREATE\_MULTITHREADED 和 D3DCREATE\_FPU\_PRESERVE 标志 25。缺少 FPU 保留标志会导致WPF的布局系统引发难以排查的浮点数运算异常。  
2. **分配D3D11纹理资源**：在D3D11设备上，为三重缓冲池分配三个 Texture2D 对象。分配时，关键配置包含 D3D11\_RESOURCE\_MISC\_SHARED（指示DXGI开放共享）和 D3D11\_BIND\_RENDER\_TARGET。像素格式必须锁定为 DXGI\_FORMAT\_B8G8R8A8\_UNORM，因为这是WPF唯一能够无缝映射的格式 20。  
3. **句柄提取与D3D9映射**：通过查询 D3D11 纹理的 IDXGIResource 接口，提取出 SharedHandle（一个非托管 IntPtr）。随后，利用该句柄在 D3D9 设备上调用 CreateTexture，生成对应的 IDirect3DTexture9，并提取出 0 级的 IDirect3DSurface9 25。此时，D3D11与D3D9已经物理指向了显存中的同一块区域。  
4. **SkiaSharp后端绑定**：将 D3D11 的 Texture2D 封装为 SkiaSharp 的 GRBackendRenderTarget。借此，Skia的 GRContext 获得了向该纹理直接发射GPU指令的能力 29。

这种复杂的管道建立后，当 Task.Run 中的 Skia 指令完成绘制并执行 Flush 后，硬件会确保显存中的数据更新。随后，UI线程只需将关联的 D3D9 表面传递给 D3DImage.SetBackBuffer，WPF 的合成引擎（milcore）就能直接从显存中提取画面并进行屏幕垂直同步，整个过程不仅没有内存到显存的拷贝，甚至避免了显存内部的数据转移，达到了极限的吞吐量 24。

## **应对极致并发：GPU模式下的三重缓冲设计**

在传统的双缓冲（Double Buffering）模型中，如果后台绘制线程的帧生成速度快于显示器的刷新率（例如WPF绑定的60Hz），后台线程在提交新帧时，必须等待WPF完成上一帧的合成并释放后备缓冲区的锁。这种锁等待会导致后台计算资源的严重浪费。反之，如果取消锁限制强行覆写，则会导致严重的画面撕裂（Tearing）30。

为了满足需求5的要求，本架构在GPU与CPU模式下全面引入了无锁的三重缓冲（Triple Buffering）状态机。三重缓冲允许系统在同一时刻维持三个独立的帧状态，彻底解耦了生产者（SkiaSharp后台线程）与消费者（WPF UI线程）。

### **缓冲区状态转换模型**

三重缓冲池（包含三个对应的 D3D11-D3D9 共享表面对，或三块非托管内存）在任意时刻，必然分别处于以下三种离散状态之一：

| 状态名称 | 所属线程域 | 物理含义与访问权限 |
| :---- | :---- | :---- |
| **呈现中 (Presenting)** | UI线程 (Dispatcher) | 该缓冲区目前正通过 D3DImage.SetBackBuffer 被WPF的组合引擎锁定并在屏幕上显示。后台线程严禁对此缓冲区进行任何写操作，否则会触发COM层级的访问违例或引发严重的画面撕裂 14。 |
| **就绪 (Ready)** | 中立 / 队列 | 这是一个已经由后台线程完整绘制完毕并Flush到GPU的帧。它悬停在队列中，等待WPF的 CompositionTarget.Rendering 事件在下一个垂直同步周期将其拾取为“呈现中”状态。 |
| **可用 (Available)** | 后台线程 (Task.Run) | 此缓冲区已被WPF释放。当业务发起新的 OpenRender 请求时，系统会将此缓冲区提供给 Skia 的 SKCanvas 进行下一帧的绘制指令积累。 |

### **无锁原子交换机制**

为了实现极低延迟，架构摒弃了高开销的 lock (object) 互斥锁，转而采用 System.Threading.Interlocked 提供的原子交换操作来管理这三个缓冲区的索引指针。

**生产者流程 (后台 Task.Run)：**

1. 获取当前标记为 Available 的缓冲区索引进行渲染。  
2. 渲染完成且上下文 Dispose 后，执行原子交换：Interlocked.Exchange(ref ReadyIndex, AvailableIndex)。这一步将刚刚画好的帧推入就绪槽。  
3. 如果此时UI线程尚未消耗之前的旧 Ready 帧，原子交换会将旧的 Ready 帧退回给生产者作为新的 Available 帧。这意味着在超高帧率场景下，中间帧被智能地“丢弃（Drop）”，避免了背压（Backpressure）导致的工作队列堆积 30。  
4. 发送轻量级的信号触发UI线程更新。

**消费者流程 (前端 UI线程)：**

1. 收到更新信号后，在UI调度器中执行原子交换：Interlocked.Exchange(ref PresentingIndex, ReadyIndex)。这使得最新画好的帧被切入显示槽，同时正在显示的旧帧被退回给队列。  
2. 调用WPF的严格渲染管线：

C\#

d3dImage.Lock();  
d3dImage.SetBackBuffer(D3DResourceType.IDirect3DSurface9, pointerArray\[PresentingIndex\], true);  
d3dImage.AddDirtyRect(new Int32Rect(0, 0, width, height));  
d3dImage.Unlock();

通过三重缓冲，后台 Task.Run 中的业务数据处理和 SkiaSharp 的图元栅格化可以以 100% 的CPU/GPU利用率狂奔（例如达到 300+ FPS），而 UI 线程依然能以平滑稳定的 60 FPS 提取最新鲜的画面进行合成呈现，完美平衡了计算吞吐量与视觉流畅度。

## **智能硬件降级：Windows 7 与纯CPU回退机制**

架构的第二项核心需求是必须在不支持 D3D11 的环境下（如陈旧的 Windows 7 机器、未安装显卡驱动的服务器、或部分远程桌面 RDP 协议下）正常运行。这种环境约束使得上述的 DXGI 表面共享机制彻底失效 25。

为了实现高可靠性，FrameworkElement 在初始化时必须实现一套严密的硬件能力探测与降级策略。

### **硬件能力动态探测**

WPF 暴露了 RenderCapability.Tier API 供开发者查询当前的硬件加速层级。探测逻辑如下：

1. 检查 RenderCapability.Tier \>\> 16。若结果为 0，表明系统 DirectX 版本低于 9.0，或当前正处于软件渲染强制模式（常出现于RDP会话中），立即触发 CPU 回退 1。  
2. 尝试利用.NET 6 互操作初始化带有 D3D\_DRIVER\_TYPE\_HARDWARE 标志的 D3D11 设备 36。如果抛出异常或返回非 S\_OK，说明显卡不支持所需的特征级别（Feature Level），触发 CPU 回退。  
3. 动态监测 D3DImage.IsFrontBufferAvailableChanged 事件。如果设备在运行中途报告丢失且无法恢复，立即销毁GPU管线，热切换至 CPU 回退模式 14。

### **基于 WriteableBitmap 的内存映射渲染**

在触发 CPU 降级后，组件内部的 Image 宿主将其 Source 从 D3DImage 无缝切换为 WriteableBitmap 38。虽然 WriteableBitmap 依赖 CPU 执行像素填充，且受到内存带宽与填充率（Fill-rate）的物理限制 2，但只要设计得当，其在 Task.Run 多线程环境下的性能表现依然远胜原生的 DrawingVisual。

然而，WriteableBitmap 带来了新的多线程困境：其 Lock()、写像素和 AddDirtyRect() 操作受到WPF线程亲和性的严格保护，必须在 UI 线程执行 39。若后台 Task.Run 试图直接获取 WriteableBitmap.BackBuffer 的指针进行 Skia 绘制，则必须长时间锁定 UI 线程，这不仅破坏了异步渲染的设计初衷，更会导致严重的界面卡顿。

为此，**三重缓冲机制在系统内存（System RAM）中被完美复刻**：

1. **非托管内存分配**：组件不直接使用 WriteableBitmap 的后台缓冲，而是通过 Marshal.AllocHGlobal(width \* height \* 4\) 分配三个独立的、由组件自行管理的非托管内存块。  
2. **并行CPU光栅化**：在 Task.Run 中，使用 SKSurface.Create 直接将 Skia 的渲染后端绑定到当前 Available 状态的非托管内存块上。在这个阶段，SkiaSharp 将全面利用 CPU 的 SIMD 指令集（如 AVX2、SSE4.1）执行高效的多线程光栅化 10。整个过程独立于 WPF 存在，对 UI 线程的阻塞时间为零。  
3. **极速同步拷贝**：后台绘制完成并执行原子指针交换后，UI 线程拾取 Ready 状态的内存块。随后，UI线程锁定 WriteableBitmap，并通过底层的高速内存拷贝技术（如 Buffer.MemoryCopy 或原生 API 的 CopyMemory），将整个非托管内存块的字节序列瞬间倾倒至 WriteableBitmap.BackBuffer 中 39。

在 4K 分辨率下，执行一次纯粹的 RAM 到 RAM 内存块拷贝操作通常仅需不到 1-2 毫秒。这使得 UI 线程的锁定时长被压缩到了极限，从而在纯 CPU 渲染环境中，依然保障了系统输入响应的最高流畅度。

### **像素格式的对齐优化**

为了使得上述的“极速同步拷贝”成立，源数据格式必须与目标数据格式保持字节级的一致。WPF的 WriteableBitmap 默认且最高效的像素格式为 PixelFormats.Bgra32 或 PixelFormats.Pbgra32（预乘 Alpha 格式）40。

因此，当在后台创建针对 CPU 的 SKImageInfo 时，必须极其精确地配置色彩空间：

C\#

var info \= new SKImageInfo(  
    width: pixelWidth,  
    height: pixelHeight,  
    colorType: SKColorType.Bgra8888, // 严格对应 WPF 的 Bgra32  
    alphaType: SKAlphaType.Premul    // 严格对应 WPF 的 Pbgra32  
);

如果此处配置为 Rgba8888 或忽略了预乘设定，SkiaSharp 或 WPF 在执行内存拷贝时，将会被迫触发极为昂贵的、逐像素的 CPU 格式转换管线，导致降级模式的性能直接崩溃 41。精准对齐结构后，字节级的搬运将使系统仅受限于主板内存的物理带宽边界。

## **生产级应用中的进阶工程考量**

将如此复杂的跨界架构推向.NET 6 WPF 生产环境，仍需克服设备丢失恢复与垃圾回收等深水区难题。

### **设备丢失（Device Lost）的防御与自愈**

在 Windows 生态中，触发 UAC 提权验证、用户切换会话（Fast User Switching）、系统休眠、乃至按下 Ctrl+Alt+Delete 锁定屏幕，都会导致 WDDM 瞬间撤销对显存的控制权。此时，绑定的 D3D9 表面将变为失效状态 14。

当遭遇此类中断时，D3DImage.IsFrontBufferAvailableChanged 事件将携带 false 值触发 14。若此时 Task.Run 中的渲染管线继续不顾一切地申请锁并尝试注入新帧，应用将直接抛出不可捕获的 COM 崩溃异常（如 TryLock Fails）42。

因此，整个架构在此处接入了高优先级的断路器（Circuit Breaker）机制：

1. **即时停机**：捕获事件后，立即切断后台 Task.Run 的提交流程。对 ISkiaDrawingContext 的所有操作均被安全地转入空跑（No-op）模式。  
2. **指针解绑**：必须强制执行 D3DImage.SetBackBuffer(D3DResourceType.IDirect3DSurface9, IntPtr.Zero)。这一步至关重要，它释放了 WPF 引擎内部对 DXGI 共享句柄的底层引用计数，防止显存死锁 18。  
3. **硬件重建**：当系统恢复（事件携带 true 再次触发）时，不能重用先前的 D3D11 设备。由于上下文上下文已受损，组件需立即销毁所有三个后备缓冲区的 SKSurface、释放 GRContext、抛弃 D3D11 与 D3D9 设备引用，并重新执行完整的硬件加速管道搭建流程，最终重新挂载至 D3DImage 18。

### **WDDM 内存竞争与资源缓存池配置**

在高度并发的环境下，如果开发者通过 ISkiaDrawingContext 大量绘制临时纹理或复杂的几何路径，SkiaSharp 内部的资源缓存（Resource Cache）可能会成为隐蔽的瓶颈。如果 Skia 的 GPU GRContext 缓存配额过小，引擎在渲染每一帧时都会被迫频繁创建与销毁缓存纹理，导致 SKCanvas.Flush() 操作耗时大幅飙升，甚至在某些极端数据量下阻塞后台渲染线程长达数秒 11。

在组件初始化 GRContext 时，应显式提升其 ResourceCacheLimit 属性，充分利用现代桌面显卡的庞大 VRAM，避免不必要的纹理颠簸，从而使 Flush 提交操作恢复到亚毫秒级的延迟。

### **委托回收与.NET 6 GC 钉选**

在构建 .NET 6 与底层非托管 C++（包括 Skia 的原生绑定与 DXGI API）的互操作桥梁时，WPF 开发极易陷入委托垃圾回收（Delegate GC）的陷阱。由于组件频繁在非托管层（如 IDXGISurface 接口）注册回调委托，如果这些委托仅存在于局部变量或未被有效引用，.NET 6 的现代高频垃圾回收器将无情地将其回收。随后，当 C++ 层尝试触发回调时，将直接引发 CallbackOnCollectedDelegate 崩溃异常，导致应用程序闪退且无任何堆栈跟踪 19。

为确保稳定性，封装该绘图组件的 FrameworkElement 类必须显式维护所有跨边界委托的强引用，将其声明为类级别的只读私有成员，确保其生命周期与组件自身的生命周期严格绑定，彻底根除跨边界通信的不可预测性。

## **结论**

在.NET 6 WPF 应用程序中实现突破原生限制的高性能 2D 绘图组件，需要精妙地跨越线程调度、多API硬件管线互操作以及底层内存管理的鸿沟。通过精心构建一个继承自 FrameworkElement 的混合渲染封装体，我们能够向业务层提供一套极度类似于原生 DrawingVisual 且全面支持在 Task.Run 异步线程中操作的 ISkiaDrawingContext 接口。

当运行于具备充分显卡支持的环境中时，通过 DXGI 表面共享机制，系统构建了 D3D11 至 D3D9Ex 的零拷贝内存桥梁，配合无锁的 GPU 三重缓冲状态机，达到了吞吐量最大化与延迟极小化的完美平衡，使后台图形管线摆脱了显示刷新率的桎梏。同时，针对缺乏 D3D11 支持的严苛环境或 RDP 远程桌面场景，组件智能地退回至由 WriteableBitmap 与系统级内存三重缓冲支撑的纯 CPU 栅格化模式，在最大限度缩短 UI 线程阻塞时间的同时，提供了坚如磐石的可用性保障。这种深度的融合架构，不仅展示了 WPF 底层扩展能力的极限，更为严苛条件下的数据可视化与高性能图形应用铺平了道路。

#### **引用的著作**

1. Optimizing Performance: Taking Advantage of Hardware \- WPF | Microsoft Learn, 访问时间为 二月 24, 2026， [https://learn.microsoft.com/en-us/dotnet/desktop/wpf/advanced/optimizing-performance-taking-advantage-of-hardware](https://learn.microsoft.com/en-us/dotnet/desktop/wpf/advanced/optimizing-performance-taking-advantage-of-hardware)  
2. Optimizing Performance: 2D Graphics and Imaging \- WPF | Microsoft Learn, 访问时间为 二月 24, 2026， [https://learn.microsoft.com/en-us/dotnet/desktop/wpf/advanced/optimizing-performance-2d-graphics-and-imaging](https://learn.microsoft.com/en-us/dotnet/desktop/wpf/advanced/optimizing-performance-2d-graphics-and-imaging)  
3. WPF Desktop Performance · Issue \#622 · mono/SkiaSharp \- GitHub, 访问时间为 二月 24, 2026， [https://github.com/mono/SkiaSharp/issues/622](https://github.com/mono/SkiaSharp/issues/622)  
4. How tf do you draw in WPF ???? : r/csharp \- Reddit, 访问时间为 二月 24, 2026， [https://www.reddit.com/r/csharp/comments/11vmynt/how\_tf\_do\_you\_draw\_in\_wpf/](https://www.reddit.com/r/csharp/comments/11vmynt/how_tf_do_you_draw_in_wpf/)  
5. SkiaSharp 2.88.6 \- NuGet, 访问时间为 二月 24, 2026， [https://www.nuget.org/packages/SkiaSharp/2.88.6](https://www.nuget.org/packages/SkiaSharp/2.88.6)  
6. Xamarin.Forms \+ SkiaSharp: Create Awesome Cross-Platform Animations in Your Mobile App \- Telerik.com, 访问时间为 二月 24, 2026， [https://www.telerik.com/blogs/xamarinforms-skiasharp-create-awesome-cross-platform-animations-in-your-mobile-app](https://www.telerik.com/blogs/xamarinforms-skiasharp-create-awesome-cross-platform-animations-in-your-mobile-app)  
7. WPF Graphics Rendering Overview \- Microsoft Learn, 访问时间为 二月 24, 2026， [https://learn.microsoft.com/en-us/dotnet/desktop/wpf/graphics-multimedia/wpf-graphics-rendering-overview](https://learn.microsoft.com/en-us/dotnet/desktop/wpf/graphics-multimedia/wpf-graphics-rendering-overview)  
8. Threading Model \- WPF \- Microsoft Learn, 访问时间为 二月 24, 2026， [https://learn.microsoft.com/en-us/dotnet/desktop/wpf/advanced/threading-model](https://learn.microsoft.com/en-us/dotnet/desktop/wpf/advanced/threading-model)  
9. SkBitmap surface and multiple threads. \- Google Groups, 访问时间为 二月 24, 2026， [https://groups.google.com/g/skia-discuss/c/yqF1rwi98pg](https://groups.google.com/g/skia-discuss/c/yqF1rwi98pg)  
10. SKCanvas Class (SkiaSharp) | Microsoft Learn, 访问时间为 二月 24, 2026， [https://learn.microsoft.com/en-us/dotnet/api/skiasharp.skcanvas?view=skiasharp-3.119](https://learn.microsoft.com/en-us/dotnet/api/skiasharp.skcanvas?view=skiasharp-3.119)  
11. \[QUESTION\] \[GPU\] \[Angle\] \[WPF\] Any safe way to FlushAsync() or Flush in other thread? · Issue \#1494 · mono/SkiaSharp \- GitHub, 访问时间为 二月 24, 2026， [https://github.com/mono/SkiaSharp/issues/1494](https://github.com/mono/SkiaSharp/issues/1494)  
12. Rendering video content in WPF using a custom EVR presenter and D3DImage, 访问时间为 二月 24, 2026， [https://dzimchuk.net/rendering-video-content-in-wpf-using-a-custom-evr-presenter-and-d3dimage/](https://dzimchuk.net/rendering-video-content-in-wpf-using-a-custom-evr-presenter-and-d3dimage/)  
13. FrameworkElement Class (System.Windows) | Microsoft Learn, 访问时间为 二月 24, 2026， [https://learn.microsoft.com/en-us/dotnet/api/system.windows.frameworkelement?view=windowsdesktop-10.0](https://learn.microsoft.com/en-us/dotnet/api/system.windows.frameworkelement?view=windowsdesktop-10.0)  
14. D3DImage Class (System.Windows.Interop) | Microsoft Learn, 访问时间为 二月 24, 2026， [https://learn.microsoft.com/en-us/dotnet/api/system.windows.interop.d3dimage?view=windowsdesktop-10.0](https://learn.microsoft.com/en-us/dotnet/api/system.windows.interop.d3dimage?view=windowsdesktop-10.0)  
15. How do I force a custom FrameworkElement to render changed content? \- Stack Overflow, 访问时间为 二月 24, 2026， [https://stackoverflow.com/questions/8000688/how-do-i-force-a-custom-frameworkelement-to-render-changed-content](https://stackoverflow.com/questions/8000688/how-do-i-force-a-custom-frameworkelement-to-render-changed-content)  
16. Custom FrameworkElement design \- wpf \- Stack Overflow, 访问时间为 二月 24, 2026， [https://stackoverflow.com/questions/27381422/custom-frameworkelement-design](https://stackoverflow.com/questions/27381422/custom-frameworkelement-design)  
17. Object lifetime events \- WPF | Microsoft Learn, 访问时间为 二月 24, 2026， [https://learn.microsoft.com/en-us/dotnet/desktop/wpf/events/object-lifetime-events](https://learn.microsoft.com/en-us/dotnet/desktop/wpf/events/object-lifetime-events)  
18. Performance Issues with SharpDX and D3DImage \- Stack Overflow, 访问时间为 二月 24, 2026， [https://stackoverflow.com/questions/13368940/performance-issues-with-sharpdx-and-d3dimage](https://stackoverflow.com/questions/13368940/performance-issues-with-sharpdx-and-d3dimage)  
19. GPU-accelerated WPF without WindowsFormsHost · Issue \#745 · mono/SkiaSharp \- GitHub, 访问时间为 二月 24, 2026， [https://github.com/mono/SkiaSharp/issues/745](https://github.com/mono/SkiaSharp/issues/745)  
20. Rendering on a WPF Control with DirectX 11 \- Stack Overflow, 访问时间为 二月 24, 2026， [https://stackoverflow.com/questions/11794516/rendering-on-a-wpf-control-with-directx-11](https://stackoverflow.com/questions/11794516/rendering-on-a-wpf-control-with-directx-11)  
21. SkiaSharp, DX11 and DirectComposition \- C\# \- Answer Overflow, 访问时间为 二月 24, 2026， [https://www.answeroverflow.com/m/1402766739963510906](https://www.answeroverflow.com/m/1402766739963510906)  
22. Interop with Direct3D 11 · AvaloniaUI Avalonia · Discussion \#5432 \- GitHub, 访问时间为 二月 24, 2026， [https://github.com/AvaloniaUI/Avalonia/discussions/5432](https://github.com/AvaloniaUI/Avalonia/discussions/5432)  
23. How to Convert DirectX Texture2D to a DXGI Surface for WPF Rendering?, 访问时间为 二月 24, 2026， [https://gamedev.stackexchange.com/questions/125448/how-to-convert-directx-texture2d-to-a-dxgi-surface-for-wpf-rendering](https://gamedev.stackexchange.com/questions/125448/how-to-convert-directx-texture2d-to-a-dxgi-surface-for-wpf-rendering)  
24. Shared textures support for the WPF control through D3DImage \- Coherent Labs, 访问时间为 二月 24, 2026， [https://coherent-labs.com/posts/shared-textures-support-for-the-wpf-control-through-d3dimage/](https://coherent-labs.com/posts/shared-textures-support-for-the-wpf-control-through-d3dimage/)  
25. Direct2d with WPF over RDP \- Stack Overflow, 访问时间为 二月 24, 2026， [https://stackoverflow.com/questions/56849171/direct2d-with-wpf-over-rdp](https://stackoverflow.com/questions/56849171/direct2d-with-wpf-over-rdp)  
26. ID3D11Device::OpenSharedResource (d3d11.h) \- Win32 apps | Microsoft Learn, 访问时间为 二月 24, 2026， [https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-opensharedresource](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-opensharedresource)  
27. WPF and DirectX 11 via D3DImage \- c++ \- Stack Overflow, 访问时间为 二月 24, 2026， [https://stackoverflow.com/questions/9095089/wpf-and-directx-11-via-d3dimage](https://stackoverflow.com/questions/9095089/wpf-and-directx-11-via-d3dimage)  
28. How can I convert Stream to D3DImage using SharpDX 4.2.0? \- Stack Overflow, 访问时间为 二月 24, 2026， [https://stackoverflow.com/questions/78824601/how-can-i-convert-stream-to-d3dimage-using-sharpdx-4-2-0](https://stackoverflow.com/questions/78824601/how-can-i-convert-stream-to-d3dimage-using-sharpdx-4-2-0)  
29. SkiaSharp Namespace | Microsoft Learn, 访问时间为 二月 24, 2026， [https://learn.microsoft.com/en-us/dotnet/api/skiasharp?view=skiasharp-3.119](https://learn.microsoft.com/en-us/dotnet/api/skiasharp?view=skiasharp-3.119)  
30. Does DirectX implement Triple Buffering? \- Game Development Stack Exchange, 访问时间为 二月 24, 2026， [https://gamedev.stackexchange.com/questions/58481/does-directx-implement-triple-buffering](https://gamedev.stackexchange.com/questions/58481/does-directx-implement-triple-buffering)  
31. Mogre with WPF GUI \- Ogre Addon Forums, 访问时间为 二月 24, 2026， [https://www.ogre3d.org/addonforums/8/t-14869.html](https://www.ogre3d.org/addonforums/8/t-14869.html)  
32. WPF and Direct3D9 Interoperation \- Microsoft Learn, 访问时间为 二月 24, 2026， [https://learn.microsoft.com/en-us/dotnet/desktop/wpf/advanced/wpf-and-direct3d9-interoperation](https://learn.microsoft.com/en-us/dotnet/desktop/wpf/advanced/wpf-and-direct3d9-interoperation)  
33. How to determine the system DirectX is 11 or 11.1? \- Stack Overflow, 访问时间为 二月 24, 2026， [https://stackoverflow.com/questions/29826036/how-to-determine-the-system-directx-is-11-or-11-1](https://stackoverflow.com/questions/29826036/how-to-determine-the-system-directx-is-11-or-11-1)  
34. D3DImage and remote desktop \- Stack Overflow, 访问时间为 二月 24, 2026， [https://stackoverflow.com/questions/61122452/d3dimage-and-remote-desktop](https://stackoverflow.com/questions/61122452/d3dimage-and-remote-desktop)  
35. How do you determine if WPF is using Hardware or Software Rendering? \- Stack Overflow, 访问时间为 二月 24, 2026， [https://stackoverflow.com/questions/149763/how-do-you-determine-if-wpf-is-using-hardware-or-software-rendering](https://stackoverflow.com/questions/149763/how-do-you-determine-if-wpf-is-using-hardware-or-software-rendering)  
36. D3D11CreateDevice+Hardware crashes and fallback doesn't work · Issue \#16062 · AvaloniaUI/Avalonia \- GitHub, 访问时间为 二月 24, 2026， [https://github.com/AvaloniaUI/Avalonia/issues/16062](https://github.com/AvaloniaUI/Avalonia/issues/16062)  
37. D3D11\_CREATE\_DEVICE\_FLAG (d3d11.h) \- Win32 apps | Microsoft Learn, 访问时间为 二月 24, 2026， [https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11\_create\_device\_flag](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_create_device_flag)  
38. High performance graphics using the WPF Visual layer \- Stack Overflow, 访问时间为 二月 24, 2026， [https://stackoverflow.com/questions/8713864/high-performance-graphics-using-the-wpf-visual-layer](https://stackoverflow.com/questions/8713864/high-performance-graphics-using-the-wpf-visual-layer)  
39. GPU-accelerated WPF without WindowsFormsHost · Issue \#745 · mono/SkiaSharp \- GitHub, 访问时间为 二月 24, 2026， [https://github.com/mono/SkiaSharp/issues/745?timeline\_page=1](https://github.com/mono/SkiaSharp/issues/745?timeline_page=1)  
40. WPF DrawingVisual on a background thread? \- Stack Overflow, 访问时间为 二月 24, 2026， [https://stackoverflow.com/questions/6084585/wpf-drawingvisual-on-a-background-thread](https://stackoverflow.com/questions/6084585/wpf-drawingvisual-on-a-background-thread)  
41. Drawing with SkiaSharp \- SWHarden.com, 访问时间为 二月 24, 2026， [https://swharden.com/csdv/skiasharp/skiasharp/](https://swharden.com/csdv/skiasharp/skiasharp/)  
42. D3DImage TryLock Fails \- Stack Overflow, 访问时间为 二月 24, 2026， [https://stackoverflow.com/questions/21834735/d3dimage-trylock-fails](https://stackoverflow.com/questions/21834735/d3dimage-trylock-fails)