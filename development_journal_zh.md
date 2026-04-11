# 原生 PICO VR 互联开发日志（中文版）

> 更新时间：2026-04-10  
> 工作区：`D:\videotest`  
> 本文件是持续维护的开发日志，用于记录问题、修复方式、阶段性结论和技术 tradeoff。

---

## 1. 项目目标

目标是构建一个“类 PICO VR 互联”的原生系统原型，遵循以下分阶段架构：

- **PC 端**：Windows Native
- **头显端**：Android Native + OpenXR，目标设备为 **PICO 4 Ultra**
- **Unity**：后置接入，等原生链路稳定后再作为内容源接入

近期闭环目标是：

1. 头显上运行原生应用
2. 头显把姿态上行到 PC
3. PC 把视频下行到头显
4. 头显解码并显示
5. 用户转头时，PC 侧内容随姿态变化并重新下发

---

## 2. 当前架构快照

### PC 侧

- pose 接收：UDP
- 视频下发：
  - 原始 RGBA bring-up 路径
  - NVENC H.264 主路径
- 渲染 / 编码基础：
  - D3D11
  - NVIDIA Video Codec SDK sample wrapper

### PICO 侧

- NativeActivity
- OpenXR
- EGL / OpenGL ES
- UDP pose sender
- UDP encoded video receiver
- MediaCodec H.264 decoder
- OpenXR quad layer 显示
- Android surface swapchain 作为解码输出目标

---

## 3. 已经达成的关键里程碑

### 里程碑 A：实机 Pose 上行成功

在真实 PICO 4 Ultra 上验证通过：

- OpenXR session 可启动
- 头部姿态可采样
- 姿态包可通过 UDP 到达 Windows
- Windows 端可以持续打印实时 `pos / quat`

### 里程碑 B：原始视频下行成功

在真实设备上验证通过：

- PC 发送低分辨率原始 RGBA 测试图像
- 头显接收并显示测试画面

### 里程碑 C：H.264 编码视频下行成功

在真实设备上验证通过：

- Windows 侧用 NVENC 输出 H.264 Annex B
- Android 端完成编码帧重组
- MediaCodec 成功配置并开始输出
- OpenXR quad layer 显示解码结果

这是原生编码链路第一次真正打通。

---

## 4. 开发过程中遇到的典型问题

## 4.1 App 长时间“加载中”，无法进入可见 XR 场景

### 现象

- App 启动了，但用户看到的是一直在加载
- OpenXR runtime 没有进入真正可见的运行状态

### 根因

- App 没有足够早地提交一个稳定可见的 composition layer
- 对 runtime 来说，它更像一个“还没真正开始显示内容”的应用

### 解决方式

- 增加最小 quad swapchain
- 增加最小可见 layer 提交路径
- 每帧都保证有可提交的显示层

### Tradeoff

- 提前写了一些显示脚手架代码
- 但这是让 runtime 把应用当成真正 XR 场景而不是“加载页”的必要条件

---

## 4.2 Android surface swapchain 相关路径不稳定

### 现象

- surface swapchain 早期接入时不稳定，甚至可能崩溃

### 根因

- 对 Java `Surface` 和 JNI 引用关系的处理过度复杂
- 早期版本中的引用生命周期管理不适合当前 runtime 行为

### 解决方式

- 简化 surface 生命周期管理
- 直接从 `Surface` 获取 `ANativeWindow`
- 将其作为 MediaCodec 输出目标

### Tradeoff

- 当前采用了较保守、较简单的所有权模型
- 后续如果 surface 生命周期更复杂，还需要再细化

---

## 4.3 收到编码帧，但头显没有真正显示出视频

### 现象

- 日志显示编码帧已经到达
- 但头显中看不到有效视频输出

### 根因

- 后台解码 pump 没有得到足够执行机会
- NativeActivity 事件轮询在某些状态下过于阻塞
- 导致网络线程收到的编码帧不能稳定推进到 MediaCodec

### 解决方式

- 增加 `TickBackgroundWork()`
- 在 session 未完全 running 之前也持续 pump 解码链路
- 降低主循环阻塞程度

### Tradeoff

- 主循环更积极，不再是纯事件驱动
- 但对 bring-up 阶段来说，这个取舍是正确的

---

## 4.4 很难判断 MediaCodec 到底卡在哪一段

### 现象

- 初期很难区分问题是在：
  - 包接收
  - decoder configure
  - input queue
  - output render
  - OpenXR 显示

### 根因

- 缺少足够细的日志

### 解决方式

- 增加解码路径日志：
  - 收到 codec config
  - 配置 decoder
  - queue input
  - output format change
  - first output frame
  - 切换到 Android surface-backed quad layer

### Tradeoff

- 日志噪声变多
- 但 native bring-up 阶段“信息充分”远比“日志漂亮”重要

---

## 4.5 晚加入 / 恢复时的 decoder bootstrap 与稳定播放冲突

### 现象

- 如果头显启动较晚，可能会错过最开始的 codec config
- 简单粗暴地周期重发 codec config 会提高恢复率
- 但这又会影响稳定播放

### 根因

- 晚加入恢复能力和稳定持续播放之间存在天然张力

### 阶段性解决方式

- sender 支持周期 keyframe
- 一度也支持周期性 codec config 重发

### 副作用

- 头显可能偶发闪帧
- 某一帧中小方块消失但背景还在

### 当前结论

- 周期性 standalone codec config 不是稳态下的理想方案
- 更合理的是：
  - 保留周期 IDR
  - 在需要时由 control channel 显式请求 codec config

---

## 4.6 “adb 已启动应用”不等于“头显中真正看到应用”

### 现象

- `adb shell am start` 返回成功
- 但实际头显中不一定已经真正前台显示

### 根因

- XR 生命周期与普通 Android Activity 启动状态并不完全等价

### 解决方式

- 联调时同时检查：
  - Android 进程状态
  - OpenXR session state
  - Activity 前台焦点
  - 实际头显显示效果

### Tradeoff

- 某些问题无法仅靠命令行判断
- XR 联调仍需要人眼确认某些显示层面的行为

---

## 5. 为什么我们刻意延后 Unity

Unity 不是被放弃，而是被刻意后置。

原因是我们一开始更需要回答这些问题：

- 原生 OpenXR 是否稳定
- 姿态与视频能否在 native 层打通
- MediaCodec 与显示层怎样协作
- Windows 端如何掌握真正的低层编码路径

如果一开始就把这些问题交给引擎，很多关键风险会被框架封装住，不利于系统级定位。

这就是为什么当前路线是：

- **先原生**
- **后 Unity**

---

## 6. 当前已知限制

1. 传输仍然是原型级 UDP
2. 控制通道还很小
3. pose 与 video 的严格时序同步还没做完整
4. Unity 仍未正式接入
5. 联调仍受网络与系统环境影响

---

## 7. 当前对系统状态的判断

现在的项目已经具备很强的工程价值，因为它已经不是“空谈架构”，而是一套已经能在真实头显和 Windows 之间完成闭环的系统原型。

它已经包含：

- 真实头显
- 真实 OpenXR session
- 真实姿态上行
- 真实编码下行
- 真实硬件解码
- 真实 XR 显示
- 真实交互闭环

---

## 8. 推荐的后续方向

### 短期

1. 继续把控制通道做扎实
2. 提升恢复与联调稳定性
3. 完善配置和诊断能力

### 中期

1. Unity 作为新的内容源接入
2. 不替换现有 native 传输栈
3. 通过共享发送核心挂接 Unity 纹理

### 长期

1. 评估 WebRTC
2. 更完整的时间同步与低延迟策略
3. 更强的产品级交互体验

---

## 9. 本日志的更新规则

今后遇到以下情况时，应继续更新本文件：

- 发现重大 bug
- 完成关键修复
- 技术路线发生变化
- 接受一个重要 tradeoff
- 达成新里程碑
- 替换或升级某个子系统

推荐更新格式：

```md
## Update YYYY-MM-DD

### What changed
- ...

### Problem
- ...

### Fix
- ...

### Tradeoff / impact
- ...
```

---

## Update 2026-04-06（播放稳定性与 config 重发策略）

### What changed

- 将 standalone codec config 改为仅在启动时发送一次
- 保留周期 keyframe
- 尝试让接收端从关键帧中 bootstrap decoder

### Problem

- 周期重发 standalone codec config 导致播放过程中出现闪帧

### Fix

- 去掉稳态下的周期 codec config 重发
- 保留 startup config 与周期 keyframe

### Verification

- 在“先启动头显 App，再启动 sender”的流程中，decoder 仍可成功启动并显示

### Tradeoff / impact

- 稳态播放更平滑
- 但晚加入恢复能力暂时下降

---

## Update 2026-04-06（桌面捕获原型）

### What changed

- 新增 Windows 端桌面捕获 sender：
  - `desktop_capture_nvenc_sender.cpp`

### Problem

- 之前只有程序化测试画面，不足以验证更真实的 PC 内容来源

### Fix

- 引入 DXGI Desktop Duplication
- 将桌面捕获结果送入 NVENC 并下发

### Verification

- 本地 Windows 冒烟测试通过
- 头显端编码视频接收与解码通过

### Tradeoff / impact

- 最初版本采用 CPU readback 中转
- 路径简单但延迟和 CPU 代价更高

---

## Update 2026-04-06（桌面捕获优化）

### What changed

- 将桌面 sender 优化为 GPU 保持路径
- 使用同适配器 D3D11
- 增加 GPU 缩放和线性过滤
- 增加保持纵横比的适配

### Problem

- CPU 中转版本延迟更高、缩放质量较差

### Fix

- 捕获结果保持 GPU 常驻
- 用 D3D11 shader pass 做缩放
- 直接复制到 NVENC 输入纹理

### Tradeoff / impact

- 在当前机器上效果更好
- 但跨适配器场景下的兼容性不如 CPU 中转版宽容

---

## Update 2026-04-06（姿态驱动原生场景 sender）

### What changed

- 新增：
  - `pose_scene_nvenc_sender.exe`

### Why it matters

- 桌面流证明了传输和显示
- 姿态场景 sender 首次证明了“姿态驱动内容变化”的真正闭环

### 实现方式

- 在 Windows 原生侧用 D3D11 绘制程序化场景
- 用最新姿态驱动相机
- 用 NVENC 编码并下发

### Verification

- 本地 `mock_pose_sender` 联调通过
- 头显端视频链路继续可用

### Caveat

- 某些机器上新 exe 可能受 Windows 防火墙入站策略影响

---

## Update 2026-04-09（最小控制通道）

### What changed

- 新增共享控制协议：
  - `shared-protocol/control_protocol.h`
- Android 端可发送 control packet
- Windows sender 可接收并执行 control 请求

### 当前已实现消息

- `RequestKeyframe`
- `RequestCodecConfig`

### Why

- 之前系统只有 pose 上行和视频下行，没有 control plane
- 晚加入、恢复、bootstrap 都过于脆弱

### How

- Android decoder 检测恢复需求
- `XrPoseRuntime` 把控制请求通过现有 UDP 上行发给 PC
- Windows sender 收到后可：
  - 重发 codec config
  - 强制下一帧为 keyframe

### Tradeoff / impact

- 这是一个刻意保持最小化的 control plane
- 还没有：
  - ack
  - resend
  - bitrate 协商
  - sender 状态反馈

---

## Update 2026-04-09（共享发送核心重构）

### What changed

- 新增共享发送核心：
  - `windows-native/include/video_sender_core.h`
  - `windows-native/src/video_sender_core.cpp`
- `desktop_capture_nvenc_sender`
  与
  `pose_scene_nvenc_sender`
  现在都走共享编码发送循环

### Why

- 为 Unity 接入做准备
- 避免每个 sender 各自维护一整套编码、控制、发送逻辑

### 新结构

- 内容源：负责给出 D3D11 纹理
- 共享发送核心：负责 NVENC、SPS/PPS、IDR、分片发送

### Verification

- Windows 构建通过
- 两个 sender 本地冒烟测试通过

### 당시的联调阻塞

- 当时局域网下 PC 与头显并不真正可达
- App 也没有稳定进入前台活跃显示状态

### 结论

- 那一轮联调失败不能首先归咎于 sender 重构
- 真正阻塞主要来自网络与前台状态

---

## Update 2026-04-10（热点网络恢复联调）

### What changed

- 改用手机热点进行联调
- 当时观测地址：
  - PC：`172.20.10.2`
  - 头显：`172.20.10.3`
- Android 端改为使用新 PC 地址

### Problem

- 之前 `10.51.x.x` 网络表面看起来同网段，实际上不适合继续联调

### Fix

- 换到热点
- 重建有效联调环境

### Verification

- OpenXR session 可进入活动状态
- App 真正进入前台
- Pose 上行重新打通
- H.264 下行重新打通
- `pose_scene_nvenc_sender` 再次进入 `pose=active`

### Important nuance

- 即便 `头显 -> PC` 的 `ping` 结果不稳定，UDP pose 仍然成功上行

### 解释

- ICMP 不是当前项目最可靠的联调判断标准
- 协议级日志更重要

---

## Update 2026-04-10（目标地址可配置 + 记住上次成功地址）

### What changed

- Android 端不再要求每次改源码里的 PC IP
- 新增：
  - `runtime_config_store.h`
  - `runtime_config_store.cpp`
- 新增两类能力：
  - 启动参数覆盖
  - 记住上次成功连接配置

### 新的配置优先级

1. 默认值
2. 上次成功配置
3. `Intent` 启动参数覆盖

### 支持的覆盖项

- `target_host`
- `target_port`
- `video_port`
- `encoded_video_port`

### 为什么“成功”定义在第一帧渲染后

- 如果只在应用启动时保存配置，很可能把一次失败连接也记下来
- 当前做法是：
  - 收到流
  - 解码成功
  - 第一帧真正渲染
  - 再保存为“last known good”

### Verification

- `Intent` 覆盖验证通过，日志显示 `source=defaults+intent`
- 首帧渲染后成功写入持久化文件
- 无参数重启后，日志显示 `source=defaults+persisted`

### Tradeoff / impact

- 这非常适合开发联调：
  - 不需要因 IP 变化而重编 APK
  - 行为仍然可预测、可追踪
- 但它还不是自动发现
- 如果网络变化且还没有新的成功会话，persisted 地址可能会过时

### Next likely refinement

- 在此基础上增加 UDP 自动发现
- 即使加入自动发现，也保留“上次成功配置”作为回退路径

---

## Update 2026-04-11（Unity 原生插件骨架）

### What changed

- 新增 Unity 面向的 Windows 原生插件目标：
  - `windows-native/src/unity_sender_plugin.cpp`
  - `windows-native/include/unity_sender_plugin.h`
- 新增 Unity 集成辅助目录：
  - `unity-integration/README.md`
  - `unity-integration/UnitySenderPluginBindings.cs`
- CMake 现在会自动探测本机 Unity PluginAPI 头文件
- 已成功构建：
  - `windows-native/build/Debug/unity_sender_plugin.dll`

### 为什么这一步重要

- 这意味着 Unity 接入已经从“计划”进入“可落地 DLL 边界”
- 我们不再只是在文档里说“以后 Unity 会接进来”
- 现在已经有一个明确的 native plugin 入口可以给 Unity 调用

### 当前插件做了什么

- 接入了 Unity 原生插件生命周期：
  - `UnityPluginLoad`
  - `UnityPluginUnload`
- 接入了 Unity render-thread callback：
  - `UnitySender_GetRenderEventFunc`
  - `UnitySender_GetCopyTextureEventId`
- 可以接收 Unity 传入的 native texture pointer
- 在 Unity 渲染线程上，把 `RenderTexture` 复制到插件自有 D3D11 纹理
- 单独运行网络线程接收：
  - pose
  - control
- 单独运行 sender 线程，复用现有 shared NVENC sender core

### 实现前明确查证过的约束

- Unity 官方文档确认：
  - D3D11 下 `Texture.GetNativeTexturePtr()` 返回 `ID3D11Resource*`
  - `GetNativeTexturePtr()` 会导致 render-thread 同步，因此应该缓存并仅在纹理变化时重新获取
  - `GL.IssuePluginEvent(...)` 会在 Unity 的 render thread 上回调 native 函数
- Unity 官方 NativeRenderingPlugin sample 说明了：
  - `IUnityGraphics` 的 device event 处理方式
  - render-thread callback 才是正确的原生渲染接入路径
- Microsoft D3D11 文档确认：
  - `ID3D11Device` 是线程安全的
  - `ID3D11DeviceContext` 不是线程安全的
  - `CopyResource` 要求资源类型和尺寸一致

### 由此得出的设计取舍

- 插件不会在 Unity 的 render callback 里直接做 NVENC
- 而是拆成三段：
  1. render-thread copy
  2. network receive
  3. sender encode/send
- 对共享的 D3D11 immediate context 使用了互斥同步
- 当前第一版明确要求：
  - Windows
  - Unity 渲染后端 = D3D11
  - streaming 期间 `RenderTexture` 尺寸固定

### Verification

- `unity_sender_plugin.dll` 构建成功
- 使用 `LoadLibrary/GetProcAddress` 验证了这些导出函数都存在：
  - `UnityPluginLoad`
  - `UnityPluginUnload`
  - `UnitySender_Configure`
  - `UnitySender_SetTexture`
  - `UnitySender_Start`
  - `UnitySender_Stop`
  - `UnitySender_GetLatestPose`
  - `UnitySender_GetStats`
  - `UnitySender_GetCopyTextureEventId`
  - `UnitySender_GetRenderEventFunc`

### 当前限制

- 仓库里还没有真正初始化 Unity 工程
- 当前插件只考虑 D3D11
- Unity 侧姿态坐标映射还没有最终敲定
- streaming 期间不支持动态 resize source texture

### Next likely refinement

1. 建一个最小 Unity Windows 工程
2. 接一张固定尺寸 RenderTexture
3. 接 `GetNativeTexturePtr()` + `GL.IssuePluginEvent(...)`
4. 从 C# 轮询 `UnitySender_GetLatestPose(...)`
5. 验证第一版 Unity 场景是否能通过现有头显链路显示
