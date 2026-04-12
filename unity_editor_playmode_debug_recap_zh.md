# Unity Editor Play 双向链路问题排查与解决复盘

## 1. 背景与目标

当前项目的目标是让链路在 **Unity Editor 的 Play 模式** 下稳定工作，而不是依赖打包后的 `Unity.exe`。目标闭环如下：

1. PC 端 Unity Editor 将场景画面编码并发送到 VR 头显。
2. VR 头显将实时 pose 回传到 PC。
3. Unity 场景中的相机根据回传 pose 实时更新。
4. 这样可以在 Editor 中直接调试渲染、传输、姿态驱动逻辑，提升开发效率。

在本轮排查开始时，已经具备这些前置条件：

- 视频下行链路已打通过。
- pose 上行链路已打通过。
- Windows 桌面抓取和 D3D11 直接渲染模拟 pose 的样例可运行。
- Unity 雏形框架已接好。

但是在 **Unity Editor 点击 Play 后**，出现了典型异常：

- Unity Editor/Game 画面无反应。
- VR 端黑屏。
- 调试面板参数不更新。
- 看起来进程还活着，但双向链路没有真正进入稳定工作状态。

---

## 2. 初始现象拆解

这次问题一开始很容易让人误判成“Unity 卡死”或者“编码器没出帧”，但实际现象更复杂：

- 有时 Unity 进程仍然存活，只是链路没有真正建立。
- 有时 VR 端直接黑屏。
- 有时 Unity 端日志里能看到发送行为，但 pose 一直是默认值。
- 网络环境在排查过程中还发生过变化：
  - 先使用手机热点。
  - 后来切换到新的 Wi‑Fi。

这意味着问题不一定只有一个，而可能是：

1. Unity 渲染/纹理格式问题；
2. NVENC 首帧/关键帧问题；
3. Unity/Windows 防火墙问题；
4. 运行时缓存地址失效；
5. 头显 OpenXR 生命周期未进入可发送 pose 的状态；
6. `auto` 目标学习逻辑没有等到第一包 pose。

因此这次排查的原则不是猜，而是 **每一步都通过日志、配置文件、端口状态来确认事实**。

---

## 3. 第一阶段：先排 Unity 端是否真的能送出视频

### 3.1 纹理格式兼容问题

首先确认 Unity 送进原生插件的纹理格式。最终在日志中确认到 Unity 侧源纹理是：

- `DXGI_FORMAT_B8G8R8A8_TYPELESS`

而原来的 NVENC ARGB 路径对格式的接受比较严格，这会导致：

- 即使 Unity 画面有在渲染，原生插件也可能拒绝复制纹理；
- 结果就是 VR 端收不到有效视频帧。

因此做了以下调整：

- `D:\videotest\pc-unity-app\Assets\Scripts\Streaming\UnitySenderController.cs:260`
  - 优先申请 `RenderTextureFormat.BGRA32`
  - 指定 `RenderTextureReadWrite.Linear`
  - 固定 `antiAliasing = 1`
- `D:\videotest\windows-native\src\unity_sender_plugin.cpp:31`
  - 增加 DXGI 格式日志
  - 接受 `B8G8R8A8_UNORM / TYPELESS / UNORM_SRGB`
  - 拷贝时强制转成 `DXGI_FORMAT_B8G8R8A8_UNORM`

### 3.2 首帧/关键帧问题

即便纹理能复制，VR 端也可能因为没有等到正确的第一帧而黑屏。为此还修正了首个非空访问单元强制 IDR 的逻辑：

- `D:\videotest\windows-native\src\video_sender_core.cpp:285`

核心思路是：

- 不再单纯依赖 `frame_index == 1`
- 而是改成“**只要还没真正发送过非空视频访问单元，就强制 IDR**”

这一步是有价值的，因为它排除了“视频编码路径自身就不稳定”的基础问题。

### 3.3 阶段性结论

做到这里之后，已经能确认：

- Unity Editor 可以真正发送视频帧。
- 头显端可以真正解码、并出现 `Rendered output frame`。

也就是说，**视频下行链路最终是可用的**。

---

## 4. 第二阶段：发现 pose 一直没有进 Unity

在 Unity 端日志里，后续观察到一个非常关键的信号：

- `pose_packets=0`
- `pose=default`

这说明：

- Unity 原生插件已经在运行；
- 视频发送线程也在跑；
- 但是 **pose 上行包根本没有进入 Unity 监听端口**。

这个阶段最重要的收获是：  
问题不再是“Unity 全部挂死”，而是“**视频和 pose 两条链路状态不一致**”。

---

## 5. 第三阶段：查到运行时缓存地址已经失效

### 5.1 Unity 端缓存地址过期

随后检查 Unity 的持久化配置文件：

- `C:\Users\Lenovo\AppData\LocalLow\DefaultCompany\pc-unity-app\VideoTestUnitySender\last_successful_endpoint.json`

里面保存的是旧热点地址：

- `172.20.10.3`

但当前 PC 实际 Wi‑Fi 地址已经变成：

- `10.51.42.140`

这意味着 Unity 虽然场景默认配置是 `targetHost: auto`，但在运行时 **优先加载了旧缓存**，导致它还在对旧设备发视频。

### 5.2 头显端缓存地址同样过期

接着检查头显端保存的运行时配置：

- `files/last_successful_runtime_config.txt`

里面同样是旧热点地址：

- `target_host=172.20.10.2`

而当前正确的 PC 地址应该是：

- `10.51.42.140`

这说明两端都存在“**换网后仍沿用旧缓存**”的问题。

### 5.3 这是这轮排查里最关键的事实之一

如果这一点不查清楚，很容易误以为是：

- Unity Editor 不兼容；
- OpenXR 运行时异常；
- 编码器不稳定；
- 甚至怀疑是插件线程死锁。

但实际上，这里已经出现了一个非常现实的工程问题：

> 网络拓扑发生变化后，运行时缓存没有失效，导致两端都在和历史地址通信。

---

## 6. 第四阶段：一度走入的弯路

这次排查里，真正的“弯路”不是完全错误，而是 **先修了真实问题，但它们并不是最后一个阻塞点**。

### 6.1 弯路一：一度把主要注意力放在纹理/编码路径上

这条线并不是白做：

- 我们确实修掉了 Unity -> NVENC 的格式兼容问题；
- 也修掉了首个有效访问单元强制关键帧的问题。

但这些修完后，系统仍可能表现为“VR 黑屏 / pose 不动”，因为后面还有缓存地址问题和 OpenXR 生命周期问题。

### 6.2 弯路二：网络切换后，问题表象被重置了

在切换新 Wi‑Fi 后，旧日志和新现场混在一起，容易造成错觉：

- 之前的头显日志可能显示新地址；
- 但新启动的一次 Play，又可能重新落回旧持久化配置；
- 这会让人误以为“刚刚明明改对了，为什么又不行”。

实际上是因为：

- 头显这边的 `Intent` 覆盖只是“本次启动生效”；
- 如果持久化配置没同步更新，下一次重启 app 仍可能回到旧地址。

### 6.3 弯路三：一度看到头显进程活着，但 XR 会话其实没进入可发姿态状态

有一次确认到：

- 头显 app 在前台；
- `OpenXR runtime initialized` 已完成；
- 但是日志只停在 `Session state changed -> 1`

对照 OpenXR 状态枚举后确认：

- `1 = XR_SESSION_STATE_IDLE`

也就是说：

- app 活着，不代表 pose 一定在发；
- 必须等它进入 `READY / SYNCHRONIZED / VISIBLE / FOCUSED` 才能真正 `RunFrame()`，也才能 `SendHeadPose()`。

这是另一个很重要的经验：

> 不要把“前台进程存活”误认为“XR 渲染循环已正常运行”。

---

## 7. 第五阶段：真正打通 Editor Play 的关键动作

最终问题真正闭环，依赖的是下面这组组合动作：

### 7.1 清掉 Unity 旧缓存，回到 `auto`

删除：

- `C:\Users\Lenovo\AppData\LocalLow\DefaultCompany\pc-unity-app\VideoTestUnitySender\last_successful_endpoint.json`

这样 Unity Scene 里的默认配置才会重新生效：

- `D:\videotest\pc-unity-app\Assets\Scenes\SampleScene.unity:429`
  - `targetHost: auto`

同时场景中本来就已经打开：

- `D:\videotest\pc-unity-app\Assets\Scenes\SampleScene.unity:437`
  - `applyPoseToCamera: 1`

### 7.2 显式把头显切到当前 PC IP

通过 adb 重启头显 app，并带上当前 PC 地址：

- `target_host = 10.51.42.140`
- `target_port = 25672`
- `encoded_video_port = 25674`

并且把头显保存的运行时配置也一并写成新地址，避免下一次重启又回到旧热点。

### 7.3 等头显 XR 会话真正进入可运行状态

最后一次成功链路建立时，头显日志明确出现：

- `Session state changed -> 2`
- `Session state changed -> 3`
- `Session state changed -> 4`
- `Session state changed -> 5`
- `Pose sent seq=...`
- `Rendered output frame #...`

这说明：

- pose 上行开始持续发包；
- 视频下行被正常解码和显示；
- XR 渲染循环已经真正运转起来。

### 7.4 Unity 端随之进入稳定工作态

Unity Editor 日志随后出现：

- `UnitySenderController started. source=scene_defaults target=10.51.77.47:25674 ...`
- `pose_packets=...`
- `pose=active`
- `saved last known good endpoint ... learnedTarget=10.51.77.47`

这表示：

- Unity 成功从第一包 pose 自动学到了头显地址；
- 之后视频发往正确头显；
- pose 也持续流入 Unity；
- Editor Play 模式下的双向链路正式恢复。

---

## 8. 最终验证结果

本轮排查后，已经确认：

1. Unity Editor Play 后，头显端可看到渲染画面；
2. 头显端持续回传 pose；
3. Unity 端持续收到 pose，`pose_packets` 持续增长；
4. `applyPoseToCamera` 已开启，Unity 相机可由 pose 驱动；
5. 视频和 pose 两条链路在 Editor 模式下同时工作。

也就是说，本轮目标已经达成。

### 8.1 后续补充：VR 端画面上下反转

在双向链路跑通后，又暴露出一个后续问题：

- VR 端可以看到 Unity 画面；
- pose 也已经正常回传；
- 但 **头显里看到的画面是上下反转的**。

这个现象说明：

- 编码和传输本身已经在工作；
- 问题更可能出在 **头显显示阶段对图像坐标原点的理解**，而不是 Unity 场景本身画错了。

#### 排查思路

这次没有直接去猜 Unity 侧要不要手工翻转 `RenderTexture`，而是先核对头显 OpenXR 运行时对 composition layer image layout 的支持情况。

在设备扩展列表中确认到了：

- `XR_FB_composition_layer_image_layout`

同时在 OpenXR 定义里确认：

- `XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB`

它的语义正是：

> 告诉运行时当前 composition layer 对应的 swapchain 图像应按“垂直翻转”的方式解释。

这比在 Unity、NVENC 或像素缓冲层手工倒置更合适，因为问题本质上发生在 **头显运行时如何解释该图像的坐标系**。

#### 最终修复

在 Android OpenXR 侧做了以下修改：

- `D:\videotest\android-native\app\src\main\cpp\xr_pose_runtime.cpp:220`
  - 如果设备支持，则把 `XR_FB_composition_layer_image_layout` 加入实例启用扩展列表
- `D:\videotest\android-native\app\src\main\cpp\xr_pose_runtime.cpp:756`
  - 为视频 `XrCompositionLayerQuad` 挂接 `XrCompositionLayerImageLayoutFB`
  - 设置 `XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB`
- `D:\videotest\android-native\app\src\main\cpp\xr_pose_runtime.h:100`
  - 增加运行时布尔开关，记录该扩展是否已启用

#### 验证方式

修复后做了这些确认：

1. Android 工程重新 `assembleDebug` 编译通过；
2. 新 APK 安装到头显成功；
3. 启动日志中确认出现：
   - `Enabling OpenXR extension: XR_FB_composition_layer_image_layout`
4. 实机观察确认：
   - VR 端画面方向恢复正常。

#### 这一问题带来的经验

这次的关键经验是：

- 当链路已经打通，但显示方向异常时，不要立刻在 Unity 或编码层做像素翻转；
- 应优先确认 **目标显示运行时是否提供标准的 image layout / vertical flip 能力**；
- 如果运行时原生支持，应优先使用运行时标准扩展，而不是在上游链路增加额外转换。

---

## 9. 为避免重复踩坑做的工程化补充

为了避免以后每次换 Wi‑Fi / 热点后再手动清缓存、改地址、重启头显，新增了辅助脚本：

- `D:\videotest\tools\sync_editor_runtime.ps1:1`

这个脚本会自动完成：

1. 解析当前 PC IPv4；
2. 清理 Unity 保存的旧成功端点；
3. 写入头显新的持久化运行时配置；
4. 用正确的 `Intent extras` 重启头显 app。

建议今后流程变成：

1. 换网络后先执行：
   - `powershell -ExecutionPolicy Bypass -File D:\videotest\tools\sync_editor_runtime.ps1`
2. 然后再在 Unity Editor 中点击 `Play`

这样可以显著减少“明明代码没变，但因为缓存地址失效导致黑屏/无 pose”的排查成本。

---

## 10. 这次排查的核心经验总结

### 10.1 Editor Play 问题不能只盯一个层次

这次问题横跨了多个层：

- Unity 渲染纹理格式
- 原生插件纹理复制
- NVENC 首帧策略
- Windows 网络/防火墙
- Unity 持久化端点
- 头显持久化端点
- OpenXR 生命周期

任何单点视角都可能得出错误结论。

### 10.2 “日志里有发送”不代表双向链路已经闭环

这次最典型的例子就是：

- Unity 能发视频，不代表 pose 一定进来；
- 头显 app 在前台，不代表 XR 会话已经进入 `FOCUSED`；
- 头显能发 pose，不代表它发到了正确地址。

必须分别验证：

- 发往哪里；
- 谁在监听；
- 是否真正收到；
- 是否真正进入下一阶段状态。

### 10.3 换网后的缓存失效策略必须工程化

这次真正最“工程化”的问题，不是算法也不是渲染，而是：

> 运行时缓存没有随着网络环境变化而失效。

这个问题如果不正视，后面每次开发调试都会反复出现“偶发黑屏 / pose 不动 / 又好了 / 又坏了”的假象。

因此后续建议继续做两类增强：

1. **运行时自诊断**
   - 启动时把当前目标地址、最近 pose 发送端地址、当前网络 IP 更清晰地打印到面板/日志中。
2. **缓存失效策略**
   - 当发现本机 IP 网段变化，或连续若干秒没有收到 pose/控制包时，主动提示或自动回退到 `auto` 学习模式。

---

## 11. 本次实际改动文件

- `D:\videotest\pc-unity-app\Assets\Scripts\Streaming\UnitySenderController.cs:260`
- `D:\videotest\windows-native\src\unity_sender_plugin.cpp:31`
- `D:\videotest\windows-native\src\video_sender_core.cpp:285`
- `D:\videotest\android-native\app\src\main\cpp\xr_pose_runtime.cpp:220`
- `D:\videotest\android-native\app\src\main\cpp\xr_pose_runtime.h:100`
- `D:\videotest\tools\sync_editor_runtime.ps1:1`

---

## 12. 一句话结论

这次问题最终不是单一 bug，而是 **“Unity 纹理格式兼容 + NVENC 首关键帧策略 + 换网后双端缓存地址失效 + XR 生命周期状态判断 + 头显 composition layer 图像布局解释”** 共同叠加出来的结果。  
真正把 Editor Play 跑通的关键，是在逐层确认事实之后，把这几层问题分别拆开并逐个闭环。
