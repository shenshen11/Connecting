# windows-native

该目录存放 Windows 原生模块。

## 当前阶段

第一阶段当前包含：

- 使用 Winsock 监听 UDP
- 接收来自头显端的 pose 包
- 校验头
- 解析并打印
- mock pose sender（用于无设备时在 PC 本机验证协议链路）
- mock video sender（用于快速原型阶段向头显下发测试视频帧）

## 未来扩展

- D3D11 纹理桥接
- NVENC 编码
- 视频发送
- pose 缓冲与统计
