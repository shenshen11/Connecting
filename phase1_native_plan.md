# 第一阶段原生开发计划（Unity 后置版）

## 1. 目标

第一阶段不直接接入 Unity，而是优先完成最关键、最不确定的原生链路验证：

1. 在 PICO 4 Ultra 上通过 Android Native + OpenXR 采集 HMD 姿态
2. 通过 UDP 将姿态发送到 Windows
3. 在 Windows 原生程序中接收、解析并打印/可视化姿态
4. 搭好后续视频编码、视频解码、Unity 接入所需的目录与协议基础

## 2. 本阶段交付物

- `shared-protocol/`：共享协议定义
- `windows-native/`：Windows 原生 pose receiver 控制台程序骨架
- `android-native/`：Android Native + OpenXR pose sender 项目骨架
- `docs` 级别说明文档：说明如何推进下一阶段

## 3. 任务顺序

### Step 1：共享协议

先统一定义：

- 包头
- pose 数据结构
- 时间戳语义
- 坐标系约定

### Step 2：Windows receiver

实现一个最小的 UDP 接收器：

- 监听固定端口
- 校验包头
- 解析 pose 包
- 打印位置、旋转、时间戳、序号

### Step 3：Android Native 骨架

建立一个 Android Native 项目骨架，包含：

- NativeActivity / native 主循环
- OpenXR 初始化占位
- pose sender 占位
- UDP 发送器

### Step 4：联调准备

待用户本地安装并确认以下依赖后进入下一阶段：

- Android SDK / NDK
- OpenXR loader / 头显平台 SDK
- PICO 设备调试环境

## 4. 本阶段不做

- Unity 场景
- 视频编码/解码实装
- 控制器输入
- 音频
- 重投影

## 5. 下一阶段计划

当 pose sender 和 receiver 跑通后，下一阶段做：

1. Windows 端测试图像编码发送
2. Android 端 MediaCodec 解码显示
3. 完成纯底层闭环
4. 最后再接入 Unity 作为真实内容源
