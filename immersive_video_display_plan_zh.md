# 沉浸式视频显示推进方案

## 当前状态

当前头显侧的编码视频显示路径是：

- PC 侧产出单路编码视频；
- 头显侧使用 `MediaCodec + Android surface swapchain` 解码；
- OpenXR 侧把解码后的 swapchain 作为一个 `XrCompositionLayerQuad` 提交；
- `quad` 的位置和尺寸固定在用户前方，因此体验更接近“看一块悬浮大屏”，而不是“进入场景”。

这条路径的关键落点在：

- `D:\videotest\android-native\app\src\main\cpp\xr_pose_runtime.cpp`

## 为什么第一步不直接做 true stereo

从当前实现出发，真正的双目沉浸式显示当然是目标方向，但直接上真双目会同时引入三类变化：

1. PC 侧要产出左右眼各自的视频内容；
2. 协议要能表达左右眼或成对帧关系；
3. 头显侧要管理双路解码和双眼显示。

这里最大的结构约束是：当前编码视频使用的是 **直接解码到 Android surface swapchain** 的路径。  
这意味着：

- 如果采用 **SBS（side-by-side）单流双目**，头显侧需要把解码后的一张大图再拆成左右眼；
- 但当前解码输出不是一个我们已经在 OpenGL 中采样的普通纹理，而是直接喂给 OpenXR layer 的 surface swapchain；
- 因此 **SBS 在当前架构下并不是最顺手的下一步**。

相对地，如果未来做 true stereo，更自然的方向是：

- 左眼一路编码视频；
- 右眼一路编码视频；
- 头显侧双路 decoder；
- 每只眼分别进入 projection 可用的 GL-backed swapchain；
- 最终通过 `XrCompositionLayerProjection` 的两个 `ProjectionView` 提交。

也就是说，**当前目标仍然更适合未来走“dual-stream stereo projection”，而不是“single-stream SBS projection”**。但经过当前设备验证，不能再默认把 Android surface swapchain 直接作为 projection view 的输入；后续需要把解码结果送入 projection 可见的普通 GL swapchain。

## 第一阶段：projection_mono

为了尽快把“前方大屏”升级为“更沉浸的全视场显示”，第一阶段先做：

- 保持 **单路编码视频** 不变；
- 保持 **单路 MediaCodec 解码** 不变；
- 仅把头显侧显示方式从 `quad` 升级为 `projection_mono`。

### 含义

`projection_mono` 的本质是：

- 仍然只有一张解码后的视频图像；
- 但不再把它贴到前方一块固定矩形上；
- 而是把同一张解码图像作为两个 `ProjectionView` 的内容提交给 OpenXR；
- 从显示感受上，更接近“画面充满视场”，为后续真双目投影路径打基础。

### 工程收益

- **发送侧不需要改**；
- **协议不需要先改**；
- **解码器不需要先改成双路**；
- 可以先验证：projection layer 这条更沉浸的显示链路是否稳定。

## 阶段性验证结论：2026-04-12

本轮在 PICO 设备上完成了关键 A/B：

- `XrCompositionLayerQuad + Android surface swapchain`：可见；
- `XrCompositionLayerProjection + Android surface swapchain`：黑屏；
- `XrCompositionLayerProjection + 普通 GL swapchain`：可见。

因此，projection layer 本身是可行的，但当前 `XR_KHR_android_surface_swapchain` 输出不能直接作为 projection view 的图像来源。详细过程记录在：

- `D:\videotest\immersive_projection_pathfinding_log_zh.md`

## 第二阶段：projection_stereo

在 `projection_mono` 的 projection 基础能力确认后，进入第二阶段：

- PC 侧输出左右眼视频；
- 头显侧维护左右眼 decoder；
- 每眼输出进入 projection 可见的 GL-backed swapchain；
- `XrCompositionLayerProjection` 的左右眼分别引用自己的 GL swapchain。

### 推荐方案

推荐优先做：

- **dual-stream stereo projection + GL-backed projection composition**

不优先做：

- **single-stream SBS**

原因是：双流能自然表达左右眼独立内容与帧对关系；而当前设备验证已经说明直接 `Android surface swapchain -> ProjectionView` 不可作为可靠假设，后续更应把每眼解码结果转入普通 GL swapchain 后再提交 projection。

## 本次落地内容

这次先把第一阶段做成可切换模式：

- 新增运行时配置 `display_mode`
  - `quad_mono`
  - `projection_mono`
- `projection_stereo`（这是 2026-04-12 的阶段状态；自 2026-04-13 起，Android 侧已开始按左右眼双 decoder / 双 swapchain 真正提交 stereo projection）
- 头显侧新增 `projection_mono` 显示路径
- 启动脚本支持把 `display_mode` 同步到头显 app
- 新增 profile：
  - `unity_editor_projection_mono`
- 新增 stereo 视频元数据基础：
  - `view_id`
  - `view_count`
  - `frame_pair_id`
  - `layout`
  - 预留 FOV/projection 字段

当前 mono 链路默认发送 `layout=mono`、`view_id=0`、`view_count=1`，头显接收端会保留并记录这些元数据。这样后续切入 dual-stream stereo 时，可以优先复用已验证的包格式扩展，而不是同时改显示路径和网络协议。

在显示路径上，已经开始把 `projection_mono` 从“直接引用 Android surface swapchain”转向 **GL-backed projection composition**：

- MediaCodec 输出到 `SurfaceTexture`；
- 头显侧用 `GL_TEXTURE_EXTERNAL_OES` 采样解码帧；
- 再把画面写入普通 OpenXR GL swapchain；
- 最后由 `XrCompositionLayerProjection` 引用这个 GL swapchain。

当前这一步已经通过 Android 编译，但还没有安装到头显做实机验证。

## 推荐验证顺序

### 验证 1：现有链路不回归

- 继续使用 `quad_mono`
- 确认当前 Unity Editor 直连链路工作正常

### 验证 2：切到 projection_mono

- 运行：
  - `D:\videotest\tools\start_unity_editor_projection_mono.ps1`
- 然后在 Unity Editor 点击 `Play`
- 观察：
  - 头显视频是否不再像固定前方大屏；
  - 视场填充是否更强；
  - 画面方向是否保持正确；
  - 头部旋转时整体视觉是否更自然。

### 验证 3：记录局限

即使 `projection_mono` 有明显进步，它仍然不是最终的 true stereo：

- 左右眼内容仍相同；
- 真实双目视差尚未建立；
- 场景深度感不会达到真正双目串流的程度。

但它能够尽早回答一个关键问题：

- **projection 显示路径本身是否值得继续投入。**

如果答案是肯定的，再进入 dual-stream stereo 的第二阶段。

## 2026-04-12：阶段 2 当前推进到哪里

当前已经不再停留在 `projection_mono` 的显示验证阶段，而是正式进入 **Phase 2 / Windows + Unity 最小双眼发送切面**：

- Unity 侧不再只保留单个 `RenderTexture` 输入能力；
- Windows 原生插件不再只假设“单纹理 + 单发送线程 + 单编码器”；
- 发送协议继续复用已落地的 stereo 元数据字段：`view_id`、`view_count`、`frame_pair_id`、`layout`。

### 本轮新增结论

在继续往 true stereo 方向推进前，又确认了一个必须先满足的约束：

- 当前 Android 接收端仍然是按 **全局 `frame_id`** 组包；
- 也就是说，后续左右眼双发送线程**不能各自从 `frame_id=1` 开始独立编号**；
- 否则左右眼包会在接收端组包阶段互相冲突，问题甚至会早于 decoder / projection_stereo 阶段暴露。

因此，当前 Phase 2 的最小可行切入点调整为：

- Unity 暴露左右眼两张 `RenderTexture`；
- 原生插件新增 `UnitySender_SetTextureForView(viewId, textureHandle)`；
- 插件内部为左右眼分别启动独立 NVENC sender；
- 但两路 sender **共享同一套 packet `frame_id` 分配器**；
- codec config / 视频帧都会携带对应 view 的 stereo metadata，便于后续 Android 侧按 view 路由到左右 decoder。

### 当前已完成

- `windows-native/include/unity_sender_plugin.h`
  - 新增 `UnitySender_SetTextureForView(...)`
  - 扩展 stats：`configured_view_count`、`latest_frame_pair_id`
- `windows-native/src/unity_sender_plugin.cpp`
  - 从单纹理/单发送线程改为双 view 纹理状态
  - 为每个 view 启动独立 sender thread
  - 引入共享 packet `frame_id` 计数器
  - codec config 也按 view 写入 stereo metadata
- `windows-native/include/video_sender_core.h`
  - sender 运行选项新增共享 packet id 支持
  - source 新增 codec-config stereo metadata 钩子
- `windows-native/src/video_sender_core.cpp`
  - 发送 codec config / access unit 时均支持共享 packet id
- `pc-unity-app/Assets/Scripts/Native/UnitySenderPluginBindings.cs`
  - 新增双眼纹理绑定入口
- `pc-unity-app/Assets/Scripts/Streaming/UnitySenderController.cs`
  - 新增 `CaptureViewMode`
  - 支持 mono / stereo_projection 两种采集路径
  - stereo 模式下创建左右眼相机与左右眼 `RenderTexture`
  - overlay 可显示 `configuredViewCount` 与 `latestFramePairId`

### 当前未完成

这一步**还不是** true stereo projection 打通：

- Android 侧当时还没有开始做左右眼双 decoder / 双 GL projection swapchain / `projection_stereo` 真正提交；这一点已在 2026-04-13 进入首版实现。
- 当前只是把 **发送端最小切面** 改成了后续可继续扩展的形态；
- 真正的实机 true stereo 仍需下一阶段把 Android 侧 stereo demux 与双眼 projection submission 接起来。

### 当前验证状态

- Windows 原生构建通过：
  - `D:\videotest\windows-native`
  - `cmake --build .\build --config Debug`
- Unity C# 脚本编译通过：
  - `D:\videotest\pc-unity-app`
  - `MSBuild.exe .\Assembly-CSharp.csproj /t:Restore /p:Configuration=Debug`
  - `MSBuild.exe .\Assembly-CSharp.csproj /t:Build /p:Configuration=Debug`

### 一个需要特别注意的运行态细节

虽然本轮新的 `unity_sender_plugin.dll` 已经成功构建出来，但 Unity 当前正在占用：

- `D:\videotest\pc-unity-app\Assets\Plugins\x86_64\unity_sender_plugin.dll`

因此自动覆盖同步失败。也就是说：

- **源码和构建产物已经是新版；**
- **但如果 Unity Editor 进程未完全释放该 DLL，当前编辑器会继续使用旧插件。**

后续如果要实机验证这一版双眼 sender 切面，必须先让 Unity 释放该 DLL，再把新构建结果覆盖到 `Assets\Plugins\x86_64`。
## 2026-04-13：Phase 2 继续推进到 Android 侧 true stereo 最小闭环

本轮不是停留在 Windows / Unity 双眼发送切面，而是把 **Android 头显侧** 也往前接了一步，目标是让 `projection_stereo` 不再只是模式占位，而是具备最小可运行语义。

### 本轮已落地

- `xr_pose_runtime` 的 decoded-video GL 路径从“单路输出”扩成了“按 view 管理的多路输出”：
  - 每个 view 都有各自的 `MediaCodec -> SurfaceTexture -> external OES texture -> OpenXR GL swapchain`
  - 当前最大支持 2 路，对应 left / right eye
- `PumpEncodedVideoDecoder()` 不再把所有 encoded frame 都喂给同一个 decoder，而是按 `stereo.view_id` 路由到对应 decoder
- `projection_stereo` 提交路径已经改成真实的双 swapchain：
  - `projection_views[0].subImage.swapchain = left`
  - `projection_views[1].subImage.swapchain = right`
- 如果当前 sender 还没真正送来双眼流，头显侧不会直接黑屏：
  - 会记录“正在等待左右眼双路 decoded view”
  - 临时复用 primary view 走 mono projection，保证继续可见、便于联调
- `projection_mono` 保持原有 GL-backed projection 路径，不回退、不重写

### 同步补上的联调链路

之前虽然 Unity 端已经具备 `Mono / StereoProjection` 两种采集模式，但 **Editor profile 并不能自动切 capture mode**。这会导致一个容易误判的问题：

- 头显已经切到 `display_mode=projection_stereo`
- 但 Unity Editor 仍沿用 scene 默认的 mono capture
- 结果就是右眼永远收不到自己的 decoded stream，看起来像“projection_stereo 没生效”

因此本轮又补了 Editor 运行时配置同步：

- `tools\sync_editor_runtime.ps1` 现在除了写头显 runtime config，也会写 Unity 的本地运行时配置
- `display_mode=projection_stereo` 会映射成 Unity `captureViewMode=StereoProjection`
- `projection_mono / quad_mono` 会映射成 Unity `captureViewMode=Mono`
- 新增：
  - `tools\profiles\unity_editor_projection_stereo.json`
  - `tools\start_unity_editor_projection_stereo.ps1`

### 当前判断

到这里，路线已经从：

- `projection_mono` 验证 GL-backed projection 可见

推进到：

- Windows / Unity 双眼发送
- Android 按 `view_id` 双路解码
- OpenXR `projection_stereo` 左右眼分别绑定不同 swapchain

也就是说，**true stereo projection 的关键骨架已经首次在代码层接通**。  
下一步的重点不再是“能不能传双眼”，而是：

- 实机验证左右眼都能稳定出现
- 观察双眼到齐前后的切换日志与行为
- 再进入性能 / 帧节奏 / 卡顿与撕裂的专项优化
