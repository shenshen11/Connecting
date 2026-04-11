# Unity Native Sender Plugin Integration Notes

> Workspace: `D:\videotest`  
> Status: first-pass native plugin integration scaffold  
> Target: Windows + Unity D3D11

This folder contains the first Unity-facing integration artifacts for the native sender plugin.

Current native DLL target:

- `D:\videotest\windows-native\build\Debug\unity_sender_plugin.dll`

Native plugin source:

- `D:\videotest\windows-native\src\unity_sender_plugin.cpp`
- `D:\videotest\windows-native\include\unity_sender_plugin.h`

---

## 1. What is verified from official docs

The current integration approach is based on the following verified constraints:

### Unity `Texture.GetNativeTexturePtr`

Unity documents that:

- on D3D11, `GetNativeTexturePtr` returns a pointer to `ID3D11Resource`
- calling `GetNativeTexturePtr` with multithreaded rendering enabled synchronizes with the render thread and is slow
- best practice is to fetch and cache the native texture pointer at initialization time, and refresh it only when the underlying texture changes

### Unity render-thread callback

Unity documents that:

- native rendering work must be queued onto Unity's render thread
- `GL.IssuePluginEvent(callback, eventId)` causes Unity to invoke the native callback on the render thread
- calling it from `OnPostRender` gives a callback right after that camera has finished rendering

### D3D11 threading

Microsoft documents that:

- `ID3D11Device` is thread-safe
- `ID3D11DeviceContext` is **not** thread-safe
- if multiple threads access one immediate context, the application must synchronize access

### D3D11 `CopyResource`

Microsoft documents that:

- source and destination must be different resources
- they must have the same type
- they must have identical dimensions
- they must have compatible formats

This is why the current plugin prototype requires the Unity `RenderTexture` size to match the encoder size.

---

## 2. Current plugin design

The current plugin intentionally separates responsibilities:

1. Unity render thread
   - copies the Unity `RenderTexture` into a plugin-owned D3D11 texture

2. Network thread
   - receives pose and control packets on the Windows side

3. Sender thread
   - uses the shared NVENC sender core
   - copies the plugin-owned texture into the encoder input
   - encodes and sends H.264

This avoids doing NVENC work directly inside Unity's render callback.

---

## 3. Current exported API

From `unity_sender_plugin.dll`:

- `UnitySender_Configure`
- `UnitySender_SetTexture`
- `UnitySender_Start`
- `UnitySender_Stop`
- `UnitySender_IsRunning`
- `UnitySender_GetLatestPose`
- `UnitySender_GetStats`
- `UnitySender_GetCopyTextureEventId`
- `UnitySender_GetRenderEventFunc`

The P/Invoke declarations are provided in:

- `D:\videotest\unity-integration\UnitySenderPluginBindings.cs`

---

## 4. Expected Unity-side startup sequence

For the first prototype integration:

1. Create a fixed-size `RenderTexture`
2. Make the capture camera render into that texture
3. Call `RenderTexture.Create()`
4. Call `GetNativeTexturePtr()` once after creation
5. Pass that pointer to `UnitySender_SetTexture(...)`
6. Call `UnitySender_Configure(...)`
7. Call `UnitySender_Start()`
8. After the camera renders, call `GL.IssuePluginEvent(...)`
9. Poll `UnitySender_GetLatestPose(...)` from C# and apply it to the Unity camera logic

---

## 5. Important current constraints

### 5.1 D3D11 only

This first-pass plugin currently assumes:

- Unity is running with **D3D11**
- Windows platform

If Unity switches to D3D12, Vulkan, or OpenGL, this plugin should currently refuse to run correctly.

### 5.2 Fixed `RenderTexture` size while streaming

The current plugin does **not** yet support resizing the source `RenderTexture` while the sender is running.

If the `RenderTexture` is recreated at a different size:

- stop the sender
- get the new native pointer
- set the new texture
- start the sender again

### 5.3 No built-in Unity-side pose mapping yet

The plugin returns the latest raw pose packet, but this repo does **not** yet contain a final Unity coordinate-conversion layer.

That mapping should be validated in the actual Unity scene, instead of being guessed in advance.

### 5.4 First integration should avoid SRP-specific assumptions

For the first Unity bring-up:

- prefer the simplest camera-driven setup
- verify the native plugin path first
- only after that decide whether to formalize the render callback path for URP/HDRP

---

## 6. Recommended first Unity test

The first Unity-side validation should be intentionally narrow:

1. One camera
2. One fixed-size `RenderTexture`
3. One simple scene with obvious motion parallax
4. Native plugin running in parallel
5. Pose polled from the native plugin
6. Camera updated in Unity
7. Render-thread event issued after the capture camera renders

If that works, the next steps are:

1. confirm pose-to-camera mapping
2. convert to stereo content
3. improve render-thread integration for the chosen Unity pipeline

---

## 7. Build note

The CMake target `unity_sender_plugin` currently auto-detects Unity PluginAPI headers from installed editors under:

- `C:/Program Files/Unity/Hub/Editor/*/Editor/Data/PluginAPI`

It also supports:

- `UNITY_PLUGIN_API_DIR` environment variable

If those headers are not found, the plugin target is skipped.
