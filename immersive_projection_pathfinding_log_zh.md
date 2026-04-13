# 沉浸投影显示路径探索记录

## 背景

目标是把头显侧视频显示从固定 `XrCompositionLayerQuad` 逐步推进到 `XrCompositionLayerProjection`，最终服务于 true stereo projection：

- 左眼画面进入左眼 `ProjectionView`；
- 右眼画面进入右眼 `ProjectionView`；
- OpenXR compositor 直接接收左右眼对应画面。

本轮探索的第一阶段不是直接做真双目，而是先验证 `projection_mono`：仍然使用单路 Unity Editor 串流、单路 H.264、单路 MediaCodec 解码，但把显示层从 quad 改成 projection。

## 环境与设备

- 工作目录：`D:\videotest`
- 分支：`codex/immersive-display-presenter`
- 头显 ADB 序列号：`PA9410MGK6200301G`
- 头显型号日志：`PICO 4 Ultra Enterprise`
- adb 路径：`D:\platform-tools-latest-windows\platform-tools\adb.exe`
- 本地构建 APK：`D:\videotest\android-native\app\build\outputs\apk\debug\app-debug.apk`

## 关键排查过程

### 1. 先排除“没装新 APK”

一开始用户执行 `start_unity_editor_projection_mono.ps1` 后没有看到视觉变化。检查设备包信息发现：

- 设备侧 `lastUpdateTime=2025-08-05 21:11:30`
- 新 APK 还没有安装到头显
- 设备侧 `last_successful_runtime_config.txt` 也没有 `display_mode`

因此先重新构建并安装 APK，再启动 profile。

验证结果：

- `adb install -r D:\videotest\android-native\app\build\outputs\apk\debug\app-debug.apk` 成功；
- profile 启动后设备配置写入 `display_mode=projection_mono`；
- 日志确认运行时读取到 `Decoded video display mode: projection_mono`。

### 2. Unity Play 后出现黑屏，先拆链路

用户点击 Unity Editor `Play` 后，VR 端黑屏。没有直接猜测 projection 代码错误，而是先抓日志确认链路分段状态。

日志显示：

- Unity 端确实在发送编码帧；
- 头显端持续收到 `Encoded video frame complete`；
- `AMediaCodec` 已经持续输出 `Rendered output frame`；
- pose 也在持续发送；
- 也出现过 `Decoded video is now submitted as projection_mono...`。

这说明黑屏不是“Unity 没发帧”、不是“UDP 没收到”、也不是“MediaCodec 没出帧”，问题被缩小到 OpenXR projection 提交或 projection 源图像这一段。

### 3. 与 `quad_mono` 做 A/B

在不改变 Unity Play 状态的情况下，切回：

- `D:\videotest\tools\start_unity_editor_direct.ps1`

然后重新抓系统截图。

结果：

- `quad_mono` 下画面可见；
- 系统截图 `D:\videotest\tmp_headset_screencap_quad.png` 能看到 Unity 场景；
- `projection_mono` 下系统截图 `D:\videotest\tmp_headset_screencap.png` 为纯黑。

因此可以确认：同一套发送和解码链路在 quad layer 中可见，但在 projection layer 中不可见。

### 4. 排除 `XR_FB_composition_layer_image_layout`

曾怀疑垂直翻转扩展挂接方式可能影响 projection。修正并随后临时禁用 projection 路径上的 `XrCompositionLayerImageLayoutFB` 后重新构建、安装、验证。

结果：

- 去掉该扩展后 `projection_mono` 仍然纯黑；
- 因此它不是本轮黑屏的根因。

### 5. 关键诊断：把 projection 源换成普通 GL swapchain

为了区分“projection layer 本身不可用”与“Android surface swapchain 不适合作为 projection 源”，做了一个临时诊断版：

- `projection_mono` 不再直接引用 MediaCodec 的 `android_surface_swapchain_`；
- 改成先用已有 `quad_swapchain_` 渲染测试画面；
- 再把这个普通 GL swapchain 作为 `XrCompositionLayerProjection` 的两个 `ProjectionView` 内容提交。

用户在头显里确认：

- 能看到测试画面。

这一步非常关键，因为它证明：

- `XrCompositionLayerProjection` 路径本身在这台 PICO 设备上是可用的；
- 当前黑屏不是 projection layer 的基础能力问题；
- 黑屏集中在“把 `XR_KHR_android_surface_swapchain` 创建出来的 Android surface swapchain 直接作为 projection view 的 subImage”这一组合上。

## 阶段性结论

当前最可靠的判断是：

- `XrCompositionLayerProjection + 普通 GL swapchain`：可见；
- `XrCompositionLayerQuad + Android surface swapchain`：可见；
- `XrCompositionLayerProjection + Android surface swapchain`：在当前 PICO 运行时上表现为黑屏。

因此，原先设想的“每只眼各自一个 Android surface swapchain，然后直接挂到 `ProjectionView`”不能再被当作已验证可行路径。

## 对后续 true stereo 路线的影响

true stereo projection 的目标仍然成立，但头显侧图像来源需要调整：

- 仍然推荐优先做 dual-stream，而不是单流 SBS；
- 但每眼解码结果不应再默认直接作为 Android surface swapchain 提交给 projection；
- 下一步更合理的方向是建立 **GL-backed projection composition**：
  - MediaCodec 输出到可被 GL 采样或导入的对象；
  - 头显侧把每眼画面写入 OpenXR 普通 GL swapchain；
  - projection layer 的左右眼 `ProjectionView` 引用这些普通 GL swapchain。

需要继续验证的候选技术包括：

- `MediaCodec -> SurfaceTexture / GL_TEXTURE_EXTERNAL_OES -> GL blit -> XrSwapchain`
- `MediaCodec -> ImageReader / AHardwareBuffer / EGLImage -> GL blit -> XrSwapchain`
- 后续双流时每眼维护独立 decoder、独立 GL projection swapchain、独立 `ProjectionView`

## 下一步开发记录

在本轮验证结论之后，先铺设不会被显示路径阻塞的协议基础：

- 扩展 `shared-protocol/video_protocol.h`；
- 增加 `view_id`、`view_count`、`frame_pair_id`；
- 预留 `layout`、`fov`、`projection` 字段；
- 让当前 mono sender 默认发送 `view_id=0`、`view_count=1`、`layout=mono`；
- 头显接收端保留这些元数据，为后续 dual-stream stereo projection 做准备。

### 2026-04-12 继续开发记录

在用户确认“可以看到测试画面”之后，本轮判断不再停留在猜测层面，而是把结论固化为后续路线约束：

- 可继续投入 `XrCompositionLayerProjection`，因为普通 GL swapchain 已经在 projection layer 中可见；
- 不再把 `Android surface swapchain -> ProjectionView` 当作可靠假设；
- 先完成 stereo 元数据协议铺垫，避免后续双流开发时再拆基础包格式；
- 本轮已验证 Android `assembleDebug` 与 Windows `cmake --build .\build --config Debug` 均可通过，说明协议字段扩展没有破坏当前 mono sender/receiver 编译。

同时需要记录一个容易误判的状态差异：本地源码中的临时开关 `kProjectionMonoDiagnosticUseQuadSwapchain` 已恢复为 `false`，避免把测试画面当作正式 `projection_mono` 行为；但本轮构建后的 APK 没有再次安装到头显，因此头显上当前仍可能保留“可见测试画面”的诊断安装状态。后续如果安装当前新 APK，`projection_mono` 不再显示诊断测试画面，而会进入新接入的 `SurfaceTexture -> GL swapchain -> ProjectionView` 路径；它仍需要实机验证，若黑屏也应优先按新 GL 路径排查，而不是再回到“Unity 没发帧”的猜测。

### GL-backed projection 开发推进

基于设备 A/B 的结论，后续不再优先走 `XR_KHR_android_surface_swapchain` 直接 projection。当前已开始接入第一版 **SurfaceTexture -> GL swapchain -> projection** 基础：

- 使用 `android.graphics.SurfaceTexture` + `ASurfaceTexture` 创建 MediaCodec 可写入的 `ANativeWindow`；
- 使用 `GL_TEXTURE_EXTERNAL_OES` 接收解码输出；
- 把外部纹理 blit 到普通 OpenXR GL swapchain；
- `projection_mono` / 当前预留的 `projection_stereo` 会优先尝试使用该 GL swapchain 构建 `XrCompositionLayerProjection`；
- Android `assembleDebug` 已通过，说明这一阶段先达到“可编译、可安装验证”的状态。

这一步仍然需要上机验证方向、时序和 SurfaceTexture 帧更新行为；在完成设备验证前，不把它当作已证明可见的最终方案。

### sender 重启后首帧被当旧帧的问题

在实机联调中又发现一个容易误导排查方向的问题：**头显 app 不重启、而 Windows sender 进程重启时，sender 的 `frame_id` 会从 `1` 重新开始。**

当前 Android 接收端的行为是：

- `udp_encoded_video_receiver.cpp` / `udp_video_receiver.cpp` 用 `IsNewerFrame(incoming, baseline)` 判断传入帧是否比最近完成帧更新；
- 判断逻辑本质上假设 `frame_id` 在单个流生命周期内持续单调递增；
- 因此如果上一次已经接收到例如 `frame_id=304`，而新的 sender 进程重启后先发 `frame_id=1` 的 codec config / keyframe，接收端会把它判定为旧帧并直接丢弃；
- 结果表现为：sender 侧日志显示已经重新发帧，但头显侧看起来“没有恢复画面”，除非重启头显 app 或等待不现实的 frame id 追平。

这个问题与 GL-backed projection 本身不同层：

- 它不是 `SurfaceTexture -> GL swapchain -> ProjectionView` 的可见性问题；
- 它是 **接收端没有把“frame_id 回退但时间戳前进”识别成 sender 重启 / 流切换**；
- 所以它会污染后续所有联调判断，必须先修。

修复方向：

- 在 Android 接收端增加 **stream discontinuity / sender restart** 识别；
- 当 `frame_id` 不再被视为 newer，但包时间戳明显前进时，将其视为新流开始；
- 清理旧 assembly / 缓存帧并接受新的低 frame id 流，而不是继续死守旧 baseline。

本轮已完成修复：

- `udp_encoded_video_receiver` 与 `udp_video_receiver` 都补充了“`frame_id` 回退但 `timestamp_us` 前进”的重启识别；
- 检测到这种情况时，会记录 `sender restart/discontinuity` 日志；
- 同时清理旧 assembly 和旧缓存帧，再接受新的低 `frame_id` 流。
- 为避免把同一帧的晚到重复 chunk 误判成重启，判定条件又进一步收紧为：
  - `incoming_frame_id < last_completed_frame_id`
  - 且 `incoming_timestamp_us > last_completed_timestamp_us`
  - 其中 `last_completed_timestamp_us` 取该帧所有 chunk 中最大的包时间戳，而不是首个 chunk 的时间戳

本轮复测结果：

- 安装最新 APK 后，不重启头显 app，仅连续重启 Windows sender；
- 头显日志成功出现  
  `Encoded video receiver detected sender restart/discontinuity ... incomingFrame=1 ... Resetting stale queued frames and accepting new stream.`
- 随后新的 `frame_id=1/2/...` 帧继续被接收端接受，不再像修复前那样被永久当作旧帧丢弃。

### 2026-04-12：继续推进 true stereo 前，先确认 Windows/Unity 发送端约束

在 `projection_mono` 的 GL-backed 路径确认可见之后，下一步并没有直接进入 Android 双 decoder，而是先回头检查 **PC / Unity / 原生插件** 当前能否承担 true stereo 的最小发送职责。

#### 代码核对结论

核对以下文件后，确认当前发送端仍是严格单路结构：

- `D:\videotest\pc-unity-app\Assets\Scripts\Streaming\UnitySenderController.cs`
- `D:\videotest\pc-unity-app\Assets\Scripts\Native\UnitySenderPluginBindings.cs`
- `D:\videotest\windows-native\include\unity_sender_plugin.h`
- `D:\videotest\windows-native\src\unity_sender_plugin.cpp`

确认到的事实是：

- Unity 只会向插件传一个纹理句柄；
- 原生插件只维护一个 source texture / copied texture；
- 插件只启动一个 NVENC sender thread；
- `RunNvencVideoSender(...)` 默认也只维护一套本地 packet `frame_id` 递增状态。

#### 新发现的关键约束：双 sender 不能各自独立编号

在真正动手改双 sender 之前，又回查了 Android 接收端：

- `D:\videotest\android-native\app\src\main\cpp\udp_encoded_video_receiver.cpp`
- `D:\videotest\android-native\app\src\main\cpp\udp_video_receiver.cpp`

结论非常关键：

- 当前接收端组包时仍然是按 **全局 `frame_id`** 判定“是否切换到新 frame assembly”；
- 它并不会先按 `stereo.view_id` 建立左右眼独立组包槽位；
- 所以如果左右眼 sender 都从 `frame_id=1`、`2`、`3`… 自己递增：
  - 左右眼数据会在接收端 assembly 阶段互相覆盖 / 冲突；
  - 问题会在 decoder 之前就发生，而不是等到 `projection_stereo` 阶段才暴露。

这个细节必须记录，因为它直接改变了 Phase 2 的切入方式：

- 不能只做“两个 sender thread”；
- 还必须让它们 **共享同一个 packet `frame_id` 命名空间**。

#### 因此本轮选定的 Phase 2 切法

本轮不是直接做完 true stereo，而是先把发送端切成后续可持续演进的形态：

- Unity 新增左右眼 `RenderTexture` 输入能力；
- 原生插件新增 `UnitySender_SetTextureForView(viewId, textureHandle)`；
- 插件内部按 view 维护左右眼 texture state；
- 左右眼各自跑独立 NVENC sender thread；
- 两个 sender thread 共享同一个 packet `frame_id` 计数器；
- codec config 包也带 view 级 stereo metadata，避免后续 Android 双 decoder 无法按 view 路由 SPS/PPS。

#### 本轮实际落地

已修改：

- `D:\videotest\windows-native\include\video_sender_core.h`
- `D:\videotest\windows-native\src\video_sender_core.cpp`
- `D:\videotest\windows-native\include\unity_sender_plugin.h`
- `D:\videotest\windows-native\src\unity_sender_plugin.cpp`
- `D:\videotest\pc-unity-app\Assets\Scripts\Native\UnitySenderPluginBindings.cs`
- `D:\videotest\pc-unity-app\Assets\Scripts\Streaming\UnitySenderController.cs`

具体效果：

- sender core 支持多 sender 共享 packet `frame_id`
- sender core 支持 codec config 写入 source 自定义 stereo metadata
- Unity 插件支持 view 0 / view 1 双纹理
- Unity controller 支持 `Mono` / `StereoProjection` 两种采集模式
- stereo 模式下可创建左右眼采集相机与左右眼 `RenderTexture`

#### 本轮验证与运行态阻塞

已验证：

- `D:\videotest\windows-native` 下 `cmake --build .\build --config Debug` 通过
- `D:\videotest\pc-unity-app` 下 `Assembly-CSharp.csproj` 的 restore + build 通过

但还记录到一个**非常容易误判**的运行态问题：

- 新版 `unity_sender_plugin.dll` 已在  
  `D:\videotest\windows-native\build\Debug\unity_sender_plugin.dll`  
  成功生成；
- 但尝试覆盖  
  `D:\videotest\pc-unity-app\Assets\Plugins\x86_64\unity_sender_plugin.dll`  
  时失败，原因是该 DLL **正被当前 Unity Editor 进程占用**。

这意味着：

- 本轮源码与构建产物已经是“双眼 sender 切面”新版本；
- 但如果 Unity Editor 不完全释放旧 DLL，Play 起来仍可能继续跑旧插件；
- 因此后续一旦进入这轮 sender 改动的实机验证，首先要确认 Unity 插件目录中的 DLL 是否已经真正更新，而不能只看 `windows-native\build\Debug` 下的时间戳。

### 2026-04-13：切换网络环境后“Play 但没有建立连接”的一次实证

本轮在准备继续联调时，又遇到一个**纯运行态、但非常容易误判成代码回归**的问题：用户更换了网络环境后，Unity `Play` 看起来没有建立连接。

先核对到的事实是：

- 当前 PC IPv4 已变为 `172.20.10.2`
- 但头显保存的运行时配置仍然是 `target_host=10.51.42.140`
- 头显 logcat 里仍能看到它持续发送  
  `Pose sent seq=...`  
  说明头显 app 不是没工作，而是**在往旧地址发包**

这说明问题不在：

- `projection_mono` 的显示路径；
- Unity sender 新旧 DLL；
- MediaCodec / projection 提交。

真正的断点是：

- **换网后，头显运行时配置里的 `target_host` 没有同步刷新到当前 PC 的新 IP。**

本轮处理方式：

- 重新执行  
  `D:\videotest\tools\start_unity_editor_projection_mono.ps1`
- 脚本重新解析当前 PC IPv4，并删除 Unity 侧旧的 saved endpoint 缓存；
- 头显运行时配置成功刷新为：
  - `target_host=172.20.10.2`
  - `display_mode=projection_mono`

这条记录要特别保留，因为它意味着后续只要切换：

- Wi‑Fi
- 手机热点
- 不同局域网

都不能直接复用上一次的运行时配置；必须先重新执行对应 profile / sync 脚本，再做 Unity `Play` 联调，否则很容易误以为“代码没有连上”。
## 2026-04-13：继续推进 true stereo 时，发现“头显模式切了，但 Unity 仍是 mono capture”会造成误判

在把 Android 头显侧往 `projection_stereo` 推进时，需要额外记录一个很容易把排查方向带偏的细节：

- 头显 runtime config 的 `display_mode=projection_stereo`
- **并不等于** Unity Editor 自动进入 `StereoProjection` 采集模式

之前 Unity 端虽然已经支持：

- `CaptureViewMode.Mono`
- `CaptureViewMode.StereoProjection`

但 Editor profile 只负责同步头显运行时配置，并不会自动把 Unity 本地运行时配置切到 stereo。这样会出现一种非常像“Android stereo 提交失败”的现象：

- 左眼主路仍然能工作
- 右眼一直收不到自己的 encoded / decoded 流
- 头显端如果严格等待双眼，画面就可能表现成黑屏、停在等待态，或者长期只能看到单眼复用的结果

这个问题必须单独留痕，因为它不是 Android compositor 问题，也不是 packet stereo metadata 丢了，而是：

- **display_mode 和 Unity capture mode 之前没有打通**

### 因此本轮的处理

除了 Android 真正接上双路 decoder / 双 swapchain 之外，同时补了 Editor runtime sync：

- `tools\sync_editor_runtime.ps1` 现在会写 Unity 本地配置文件
- `projection_stereo -> captureViewMode=StereoProjection`
- `projection_mono / quad_mono -> captureViewMode=Mono`

并新增：

- `tools\profiles\unity_editor_projection_stereo.json`
- `tools\start_unity_editor_projection_stereo.ps1`

### Android 侧本轮实际改动

本轮 `xr_pose_runtime` 已从“单 decoder / 单 SurfaceTexture / 单 GL swapchain”改成：

- 按 view 管理的 decoded-video GL 输出
- `PumpEncodedVideoDecoder()` 按 `stereo.view_id` 路由到对应 decoder
- `projection_stereo` 提交时，左右眼绑定不同的 OpenXR swapchain

也就是说，本轮之后：

- `projection_stereo` 不再只是模式名占位
- 代码路径已经具备 true stereo projection 的最小闭环骨架

### 为何仍保留 mono fallback

即便 Android 已支持双路 stereo 提交，本轮仍保留了一个**刻意的过渡行为**：

- 如果当前还没同时收到左右眼 decoded frame
- 或其中一路暂时还没 ready
- 头显会先记录“waiting for both decoded views”
- 然后临时复用 primary view 继续走 mono projection

这样做的原因不是要回退设计，而是为了避免在联调阶段把“右眼尚未到齐”直接表现成黑屏，从而掩盖真正的问题来源。

因此，后续如果再看到：

- 头显模式已经是 `projection_stereo`
- 但视觉上仍像 mono

优先检查顺序应改成：

1. Unity runtime config 是否真的切到了 `StereoProjection`
2. Unity overlay 里的 `Configured views` 是否已经是 `2`
3. 头显 log 是否已经出现左右眼 view 的 decoded-ready / stereo-active 日志
4. 最后才继续怀疑 Android projection submission 本身

### 2026-04-13：`projection_stereo` 运行中出现“动头严重撕裂”，定位到双眼 `frame_pair_id` 漂移

这一次不是先凭体感猜网络或 compositor，而是先同时核对了**设备运行态**、**头显日志**和 **Windows sender 代码路径**。

先确认到的事实：
- 头显当前运行时配置已经是 `display_mode=projection_stereo`
- Android 侧没有再出现前一轮已经修掉的 `sender restart/discontinuity` 误判
- 头显仍在持续收到左右眼编码帧，说明链路不是“完全没连上”

但抓取头显 logcat 后，出现了一个更关键的现象：
- 相邻完成帧里，`view=0/2` 与 `view=1/2` 的 `pair=` 经常不同
- 例如同一段日志里出现：
  - `view=1/2 pair=333933`
  - 紧接着 `view=0/2 pair=333952`
- 后续还有大量 `334585/334586`、`335364/335365`、`335825/335827` 这类左右眼 pair 不一致的样本
- 也偶尔会短暂对齐，例如 `pair=334509`、`pair=334931`、`pair=335006`

这说明当前问题的本质不是经典意义上的屏幕 tearing，而是：
- **左右眼拿到的不是同一个立体帧对**
- 头动时双眼时间基线错开，就会主观表现为非常严重的“撕裂 / 拉扯 / 抖动”

继续回看 Windows sender 代码，根因也能对上：
- `windows-native/src/unity_sender_plugin.cpp` 里的 render thread 每次 `OnRenderEvent` 都会把当前左右纹理 copy 到 sender 纹理，并立刻把 `copied_frame_pair_id_` 自增
- 两个 sender thread 随后分别调用 `AcquireCopiedTexture(view_id, ...)`
- 旧逻辑只要求“某个 view 还没拿过当前 pair”即可返回，并**没有要求左右眼先共同消费完当前 pair，再发布下一个 pair**
- 结果就是：左眼线程可能拿到 pair N，右眼线程稍晚一点就拿到 pair N+1，于是立体对失配

因此把这次问题定性为：
- `projection_stereo` 已经真正走到了“双 decoder / 双 swapchain / 双 ProjectionView”路径
- 当前主阻塞点不再是 Android compositor 提交是否可行
- 而是 **Windows sender 端缺少 stereo pair 发布/消费门控，导致左右眼 frame pair 漂移**

本轮已落地的修复：
- 在 `unity_sender_plugin.cpp` 中增加每个 view 的 `last_consumed_copied_pair_id_`
- `OnRenderEvent` 在发布新 pair 之前，会先检查当前 pair 是否已被所有 active views 消费
- `AcquireCopiedTexture(...)` 在返回纹理时，会记录当前 view 已消费该 pair
- 这样 sender 会退化成“宁可等慢眼，不让左右眼各自前冲”的保守策略，优先保证立体同步

本轮本地验证结果：
- `D:\videotest\windows-native` 下执行 `cmake --build .\build --config Debug` 已通过
- 新 DLL 已生成：`D:\videotest\windows-native\build\Debug\unity_sender_plugin.dll`
- 但覆盖到 `D:\videotest\pc-unity-app\Assets\Plugins\x86_64\unity_sender_plugin.dll` 时失败，原因是 **Unity Editor 当前仍占用旧 DLL**

因此这轮修复的真实状态应记录为：
- **代码已修、构建已过**
- **尚未完成“替换 Unity 实际加载 DLL 后的实机复测”**
- 后续复测前，必须先退出 Play 或关闭 Editor，确保 Unity 释放旧插件文件，再同步 DLL 验证左右眼 `pair=` 是否稳定一致
- 后续复测前，必须先退出 Play 或关闭 Editor，确保 Unity 释放旧插件文件，再同步 DLL 验证左右眼 pair= 是否稳定一致
