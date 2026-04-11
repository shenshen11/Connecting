# Native PICO VR Interconnect Development Journal

> Last updated: 2026-04-10  
> Workspace: `D:\videotest`  
> This is a living document. Future development notes, issues, fixes, and tradeoffs should continue to be appended here.

## 1. Project Goal

Build a native prototype similar in spirit to “PICO 互联 / VR streaming” with the following staged architecture:

- **PC side**: Windows Native
- **Headset side**: Android Native + OpenXR on **PICO 4 Ultra**
- **Unity**: deferred until the native transport / rendering / tracking baseline is solid

The near-term closed loop target is:

1. Run native app on PICO headset
2. Stream video down from PC to headset
3. Send HMD pose up from headset to PC
4. Update PC-side output according to head pose

---

## 2. Current Architecture Snapshot

### PC side

- Pose receive: UDP
- Video send:
  - raw RGBA prototype over UDP for bring-up
  - NVENC H.264 over UDP for more realistic video path
- Rendering / encoding base:
  - D3D11
  - NVENC wrapper from NVIDIA Video Codec SDK samples

### PICO side

- NativeActivity
- OpenXR
- EGL / OpenGL ES
- UDP pose sender
- UDP video receiver
- MediaCodec H.264 decode
- OpenXR quad layer display
- Android surface swapchain path for decoded video output

---

## 3. Major Milestones Reached

### Milestone A: Real pose uplink works

Confirmed on real PICO 4 Ultra:

- OpenXR session starts
- Head pose is sampled
- UDP pose reaches Windows
- Windows receiver prints live `pos / quat`

### Milestone B: Raw video downlink works

Confirmed on real device:

- PC sends low-res raw RGBA test frames over UDP
- PICO receives and displays moving test pattern

### Milestone C: H.264 encoded video downlink works end-to-end

Confirmed on real device:

- Windows NVENC generates H.264 Annex B stream
- PICO UDP receiver reassembles encoded frames
- MediaCodec configures successfully
- MediaCodec renders decoded output
- OpenXR switches to Android surface swapchain-backed quad display
- In-headset visible result: gradient background + moving square

This is the first full native encoded-video closed loop milestone.

---

## 4. Problems Encountered, How They Were Solved, and Tradeoffs

## 4.1 App stayed in “loading” and did not enter a visible XR scene

### Symptom

- App launched on headset but appeared to keep loading
- OpenXR runtime did not transition into a useful visible app state

### Root cause

- The app was not submitting a visible composition layer early enough
- From the runtime’s perspective, the app had no meaningful visible XR content

### Solution

- Added a minimal OpenXR quad swapchain
- Added a minimal visible layer submission path
- Ensured each frame submits a renderable composition layer

### Tradeoff

- This added a small amount of rendering scaffolding earlier than strictly needed
- But it was necessary to make the runtime treat the app as an actual XR scene instead of a loading app

---

## 4.2 Android surface swapchain creation was unstable / previously crashed

### Symptom

- Surface swapchain path was initially unreliable
- There were crash risks around the Java `Surface` / JNI handling

### Root cause

- The early integration handled `Surface` references too aggressively
- Extra JNI ref operations around the returned surface object were likely incorrect for this runtime path

### Solution

- Simplified surface handling
- Removed problematic `NewGlobalRef / DeleteLocalRef` style handling around the returned surface
- Obtained `ANativeWindow` from the returned `Surface` and used that directly for MediaCodec output

### Tradeoff

- We currently rely on a simpler and narrower ownership model
- This is safer for bring-up, but we should revisit lifetime assumptions if the surface management becomes more dynamic later

---

## 4.3 Encoded H.264 frames were received, but no video appeared

### Symptom

- UDP logs confirmed encoded frames were arriving
- But nothing useful showed up in-headset for the encoded path

### Root cause

- The background decode pump was not getting enough runtime on the main loop
- The event loop could block too aggressively when the app was not yet resumed / focused
- Result: encoded frames were received by the networking thread but not consistently fed into MediaCodec

### Solution

- Added `TickBackgroundWork()`
- Ensured the runtime pumps the encoded decoder even before the XR session is fully running
- Reduced blocking behavior in the main loop
- Changed the NativeActivity event polling strategy to bounded polling instead of effectively starving background work

### Tradeoff

- The main loop is now slightly more active and less purely event-driven
- This is a conscious bring-up tradeoff in favor of forward progress and observability
- Later, if needed, we can tune power behavior after the pipeline is fully stabilized

---

## 4.4 Hard to tell whether MediaCodec had really started decoding

### Symptom

- It was initially unclear whether the failure was:
  - packet receive,
  - decoder configure,
  - input queueing,
  - output render,
  - or OpenXR display submission

### Root cause

- Not enough detailed decode-path logging

### Solution

- Added logging for:
  - receiving codec config
  - configure/start results
  - queue input buffer
  - output format changes
  - output render
  - first decoded frame availability
  - switch to Android surface-backed quad layer

### Tradeoff

- More logging increases noise
- But for native bring-up, this was the right tradeoff because it converted uncertainty into actionable evidence

---

## 4.5 Late join / decoder bootstrap vs stable playback

### Symptom

- If the headset app started late, it might miss the initial codec config frame
- A simple fix was to periodically resend codec config
- But this introduced visible playback disturbance

### Root cause

- There is a natural tension between:
  1. **easy decoder bootstrap / resync**
  2. **stable uninterrupted playback**

### Interim solution

- Sender was modified to:
  - force periodic keyframes
  - periodically resend codec config

This improved recoverability.

### Side effect / tradeoff

- Periodic codec config can cause temporary decode reconfiguration behavior
- In the headset this can show up as:
  - an occasional flash
  - a frame where the moving square disappears briefly while background remains

### Current conclusion

- This is acceptable for the bring-up prototype
- It is **not** the preferred steady-state strategy for the next stage

### Planned improvement

- Stop resending standalone codec config during normal playback
- Keep periodic IDR / keyframe support
- Prefer in-band SPS/PPS on keyframes and/or decoder self-bootstrap from keyframes

This should reduce or eliminate the visible “flash frame” while keeping reasonable recovery behavior.

---

## 4.6 Session state and “app started” are not the same as “app visible in headset”

### Symptom

- `adb shell am start` successfully starts the app process
- But that does not guarantee the user is actually seeing the app in the headset foreground

### Root cause

- On PICO / OpenXR, lifecycle and session visibility are not equivalent to plain Android process launch state

### Solution

- During bring-up, validate both:
  - Android process launch
  - OpenXR session state progression
  - actual in-headset foreground behavior

### Tradeoff

- Some tests cannot be considered complete from command line logs alone
- Human-in-the-loop device confirmation remains necessary for certain XR display-stage validations

---

## 5. Why We Deliberately Deferred Unity

Unity was intentionally postponed.

### Reason

At the beginning of the project we needed to answer lower-level questions first:

- Can PICO native OpenXR run correctly?
- Can pose be sampled and sent reliably?
- Can video be transported and shown without the engine?
- Can we debug timing, encoding, decoding, and display directly?

### Benefit

- Lower complexity during bring-up
- Full control over Windows / Android low-level behavior
- Better visibility into native timing and transport issues

### Tradeoff

- Less convenient content generation initially
- More scaffolding had to be built manually

This tradeoff was worth it because the current native baseline is now substantially clearer and stronger.

---

## 6. Current Known Limitations

1. **Occasional flash frame during encoded playback**
   - Likely related to current codec config / keyframe resync strategy

2. **Current PC content source is synthetic**
   - Gradient + moving square test pattern
   - Useful for validation, but not yet a real desktop / window capture source

3. **No formal control channel yet**
   - No explicit transport for requesting keyframe / resync / bitrate adaptation / pause / resume

4. **No production-grade transport yet**
   - Current prototype uses UDP directly
   - WebRTC / congestion handling / retransmission / pacing are not yet integrated

---

## 7. Current Best Interpretation of System State

At this stage, the project already has a meaningful native prototype:

- real headset
- real OpenXR session
- real pose uplink
- real encoded video downlink
- real Android hardware decode
- real in-headset visible output

This means the project has moved beyond architecture speculation and into iterative systems refinement.

---

## 8. Recommended Next Steps

### Short-term

1. Reduce or remove flash-frame behavior
2. Clean up codec config / keyframe strategy
3. Preserve late-join resilience without repeated decoder disturbance

### Next after stabilization

1. Replace synthetic sender frames with **real Windows capture**
   - likely via **Windows Graphics Capture** and/or **DXGI Desktop Duplication**
2. Feed captured D3D11 textures directly into NVENC
3. Keep current Android receive/decode/display path

### Later stages

1. Add control channel
2. Add bitrate / resolution adaptation
3. Evaluate WebRTC integration if desired
4. Reintroduce Unity as a content source only after the native transport/rendering foundation is stable

---

## 9. Update Policy for This Document

Going forward, this file should be updated when any of the following happens:

- a major bug is found
- a bug is fixed
- a technical route is changed
- a meaningful tradeoff is accepted
- a milestone is reached
- a subsystem is replaced or upgraded

Recommended append format for future updates:

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

## Update 2026-04-06

### What changed

- Added this persistent development journal file
- Changed encoded-video sender strategy:
  - **standalone codec config is now sent only once at stream startup**
  - normal playback continues with periodic keyframes, but without periodic standalone codec-config packets
- Added an experimental receiver-side attempt to bootstrap MediaCodec from a keyframe if no config has been seen

### Problem

- Periodic standalone codec config packets were the most likely source of the visible “flash frame” during otherwise normal playback

### Fix

- Removed periodic standalone codec-config resend from the normal steady-state sender path
- Kept the initial startup config path
- Kept periodic keyframes for stream recovery cadence

### Verification

- In the normal sequence **(start headset app first, then start sender)**:
  - only the initial codec config is received
  - decoder still starts and renders output
  - repeated reconfigure events no longer appear in the same way as before

### Tradeoff / impact

- Playback should be more stable in the normal “app first, sender second” workflow
- **Late join robustness is reduced for now**
  - if the headset starts after the sender and misses the one-time startup config, it may not recover automatically
- Experimental keyframe bootstrap was tested, but current periodic keyframes do **not** appear to carry extractable SPS/PPS in a way the current receiver can use directly

### Next likely refinement

- Add an explicit **resync / request-keyframe / request-codec-config** control mechanism
- Or adjust the sender/encoder path so late-join recovery does not require recurring standalone config during stable playback

---

## Update 2026-04-06 (Desktop Capture Prototype)

### What changed

- Added a new Windows sender:
  - `D:\videotest\windows-native\src\desktop_capture_nvenc_sender.cpp`
- Added a new build target:
  - `desktop_capture_nvenc_sender`
- Integrated:
  - **DXGI Desktop Duplication** for desktop capture
  - existing **NVENC H.264** path
  - existing UDP encoded-video transport

### Problem

- The previous sender only generated a synthetic test scene
- We needed to validate a more realistic PC-side source without waiting for Unity integration

### Fix

- Implemented a native Windows desktop capture sender using **DXGI Desktop Duplication**
- The sender:
  1. captures the Windows desktop
  2. converts / resizes to target encode resolution
  3. feeds frames into the existing NVENC path
  4. sends H.264 to the headset using the same protocol already proven on the Android side

### Verification

- Windows local smoke test passed:
  - executable starts
  - desktop duplication initializes
  - NVENC produces packets
- PICO-side integration test passed at protocol/decode level:
  - headset receives encoded frames
  - MediaCodec configures for `1280x720`
  - decoded output frames are rendered

### Tradeoff / impact

- Current desktop capture prototype intentionally uses a **CPU readback bridge**
  - capture happens with a D3D11 device tied to the display/output
  - encoding happens with the NVENC-capable D3D11 device
  - frames are copied through CPU memory between the two stages
- This was chosen because it is robust for quick bring-up, especially on systems where:
  - the display output and the NVENC-capable GPU may not be the same adapter
  - e.g. laptop hybrid graphics scenarios

### Costs of the current approach

- Higher CPU cost than a pure GPU path
- Extra copy latency
- Current resize path is **nearest-neighbor**
  - fast and simple
  - but not visually ideal for text/detail compared to better GPU scaling

### Why DXGI first instead of Windows Graphics Capture

- DXGI Desktop Duplication is lower-level and straightforward for a native prototype
- It gives a strong baseline without introducing WinRT / WGC setup complexity yet
- WGC remains a good future option, especially if we later want:
  - more modern capture behavior
  - better window-level capture ergonomics
  - cleaner long-term integration

### Next likely refinement

- Observe real in-headset quality and latency with desktop capture
- If the prototype looks good:
  - move toward lower-copy / more GPU-resident processing
  - improve scaling quality
- If needed later:
  - add WGC path as an alternative capture backend

---

## Update 2026-04-06 (Desktop Capture Optimization Pass)

### What changed

- Optimized `desktop_capture_nvenc_sender` to reduce copy overhead and improve scaling quality
- The sender now uses:
  - **same-adapter D3D11 device** for desktop duplication and NVENC
  - **GPU-side frame scaling**
  - **linear filtering**
  - **aspect-ratio preserving fit with black padding**

### Previous bottleneck

- The first desktop capture prototype used:
  - GPU desktop capture
  - CPU readback
  - CPU nearest-neighbor resize
  - GPU upload back into NVENC input

That path was simple and robust, but it introduced:

- extra latency
- extra CPU cost
- lower-quality scaling for fine UI details

### New approach

- Capture frame stays GPU-resident
- Desktop duplication output is copied into a GPU shader-resource texture
- A lightweight D3D11 shader pass scales the image into an encode-sized render target
- The scaled texture is copied directly into the NVENC input texture

### Verification

- Local Windows smoke test passed
- PICO validation also passed:
  - `MediaCodec configured for 1280x720 H.264`
  - `MediaCodec rendered first output frame`
  - decoded desktop frames continue to arrive and display

### Tradeoff / impact

- This optimized path is better for latency and image quality on systems where:
  - the captured desktop output is on an adapter that also supports NVENC
- It is a good fit for the current machine because the active output is on the NVIDIA adapter

### Remaining tradeoff

- This path is **less universally forgiving** than the older CPU-bridge prototype on unusual cross-adapter configurations
- If a future machine presents:
  - desktop output on non-NVIDIA adapter
  - NVENC on a different adapter
  then a fallback path or alternate capture backend may still be necessary

### Practical improvement expected

- Lower end-to-end latency than the first desktop prototype
- Better visual quality than nearest-neighbor resize
- No aspect distortion; the image should be letterboxed/pillarboxed instead of stretched

---

## Update 2026-04-06 (Pose-Driven Native Scene Sender)

### What changed

- Added a new Windows executable:
  - `D:\videotest\windows-native\build\Debug\pose_scene_nvenc_sender.exe`
- Added source:
  - `D:\videotest\windows-native\src\pose_scene_nvenc_sender.cpp`
- This sender:
  - receives pose on UDP port `25672`
  - renders a procedural 3D-like native scene on Windows using D3D11
  - encodes that scene with NVENC
  - sends it to the headset using the existing H.264 video path

### Why this step matters

- Desktop streaming proved the transport and display path
- This new sender is the first explicit move toward a **true head-motion-driven closed loop**
- It lets us validate:
  - pose uplink
  - native Windows content update
  - native encoded downlink
  - headset display

### Scene design choice

- Instead of integrating a full engine immediately, the sender uses a **procedural shader scene**
- The scene contains:
  - sky gradient
  - floor grid
  - colored spheres
- Camera position and orientation are driven by the latest received headset pose

### Tradeoff

- This is intentionally simpler than a full desktop scene graph or engine renderer
- But it is an excellent systems-integration step because:
  - it is deterministic
  - it is lightweight
  - it visibly reacts to pose
  - it stays fully native and low-level

### Validation status

- Local validation with `mock_pose_sender.exe` succeeded
  - pose packets were received
  - the sender switched from default camera to active pose input
- PICO-side validation confirmed the video path still works
  - headset decodes and displays the scene stream

### Current caveat

- On some Windows setups, inbound UDP to a new executable may be blocked by firewall policy
- If the procedural scene appears but does not react to head movement, the likely causes are:
  - headset app not fully foreground / focused
  - Windows firewall not allowing inbound UDP for this new sender executable

### Practical debugging help added

- The sender now logs a warning if no pose packets arrive after startup
- It also logs when pose stream becomes active

---

## Update 2026-04-09 (Minimal Control Channel)

### What changed

- Added shared control protocol:
  - `D:\videotest\shared-protocol\control_protocol.h`
- Added Android uplink support for control packets via the existing UDP sender
- Added decoder-side control request generation on Android
- Added Windows-side control request handling in:
  - `desktop_capture_nvenc_sender`
  - `pose_scene_nvenc_sender`

### Implemented control messages

- `RequestKeyframe`
- `RequestCodecConfig`

### Why this was needed

- The system previously had only:
  - pose uplink
  - video downlink
- It did **not** have a control plane
- That made recovery logic brittle, especially for:
  - late join
  - decoder bootstrap issues
  - first-frame recovery after missing startup metadata

### How it works now

- Android decoder monitors failure / bootstrap conditions
- When needed, it queues control requests with throttling
- `XrPoseRuntime` sends those control packets back to the PC over the uplink socket
- Windows sender receives the control packet and can:
  - resend standalone codec config
  - force the next encoded frame to be an IDR / keyframe

### Current implementation detail

- Control packets currently share the same uplink UDP path as pose
- For the pose-driven scene sender:
  - pose and control are received on the same socket/port
- For the desktop sender:
  - the sender now also listens on a control UDP port (default `25672`)

### Verification

- Windows-side local verification passed:
  - a crafted `RequestCodecConfig` packet was received successfully
  - sender logged the control request
  - sender resent codec config
  - next frame was forced to keyframe path
- Android build succeeded with the new control path integrated

### Limitation

- A full end-to-end live device verification could not be completed in this pass because ADB/device connection was unavailable at validation time

### Tradeoff / impact

- This is intentionally a **minimal** control plane
- It does not yet include:
  - acknowledgements
  - retransmit logic
  - sender status reports
  - bitrate negotiation
- But it is already enough to move the project from:
  - “video-only prototype”
  to
  - “streaming system with a basic control surface”
---

## Update 2026-04-09 (Sender Core Refactor and Live-Test Blocker)

### What changed

- Added a shared Windows sender core:
  - `D:\videotest\windows-native\include\video_sender_core.h`
  - `D:\videotest\windows-native\src\video_sender_core.cpp`
- Refactored these senders to use the shared core:
  - `D:\videotest\windows-native\src\desktop_capture_nvenc_sender.cpp`
  - `D:\videotest\windows-native\src\pose_scene_nvenc_sender.cpp`
- Introduced a content-source abstraction for Windows senders:
  - desktop capture source
  - pose-driven procedural scene source
- Shared logic now lives in one place for:
  - UDP H.264 chunk send
  - NVENC initialization
  - startup SPS/PPS dispatch
  - keyframe / IDR forcing
  - control-request application
  - common sender logging

### Why this was needed

- The next planned stage is **Unity as a pluggable content source**
- Before Unity enters the project, the transport/encode loop needed to stop being duplicated in multiple executables
- The previous state had the same sender responsibilities reimplemented in both:
  - desktop capture sender
  - pose-driven scene sender

### New architecture direction

- Windows sender is now conceptually split into:
  1. **content source**
     - produces a D3D11 texture each frame
     - optionally polls source-specific inputs such as pose/control
  2. **shared sender core**
     - owns the video socket
     - configures NVENC
     - sends codec config when needed
     - forces keyframes when needed
     - packetizes and transmits H.264

This is the shape we wanted before Unity integration.

### Verification

- Windows build succeeded for:
  - `desktop_capture_nvenc_sender.exe`
  - `pose_scene_nvenc_sender.exe`
- Local smoke test passed for desktop sender
  - capture initialized
  - codec config was sent
  - encoded frames were produced after the refactor
- Local smoke test passed for pose-scene sender
  - sender initialized under the shared core
  - codec config was sent
  - encoded frames were produced under the default camera path

### Real-device blocker discovered during this pass

- Device connection through ADB is healthy
- But current PC/headset LAN reachability is still blocked in the observed setup:
  - PC: `10.51.72.200`
  - PICO 4 Ultra: `10.51.116.65`
- Bidirectional `ping` failed
- That means same `10.51.x.x/16` addressing is **not** sufficient evidence that the two endpoints are actually reachable on the network

### Additional headset runtime blocker observed

- Command-line app launch succeeded
- Headset app process started
- But runtime logs only reached:
  - `Session state changed -> 1`
- At the same time, `dumpsys activity` still showed PICO Home / `com.pvr.tobhome/.RecActivity` holding effective top focus

### Practical conclusion

- The current inability to complete a live end-to-end regression is **not first explained by the sender refactor**
- The more immediate blockers are:
  1. headset and PC are not L3-reachable on the present LAN path
  2. the XR app is not yet consistently becoming the true visible foreground app when launched through the current automated path

### Tradeoff / impact

- This refactor intentionally stops short of making a single generic input channel abstraction for everything
- Source-specific inbound logic still lives inside each source:
  - desktop source polls its control port
  - pose scene source polls pose/control on its pose port
- That tradeoff kept the refactor focused on the highest-value duplication first:
  - encode loop
  - codec-config resend
  - keyframe policy
  - H.264 packetization

### Why this tradeoff is acceptable

- It is enough to make the next Unity step clean:
  - Unity does **not** need to reimplement NVENC setup or packet send logic
  - Unity only needs to supply a D3D11 texture source compatible with the new sender core

### Next likely refinement

1. Restore real device LAN reachability and confirm headset foreground behavior
2. Re-run late-join / recovery tests on the real device
3. Add a Unity-backed Windows content source on top of the new shared sender core

---

## Update 2026-04-10 (Hotspot Recovery and Live Validation)

### What changed

- Moved PC and headset onto a phone hotspot to avoid the previous LAN isolation issue
- Confirmed current temporary development IPs:
  - PC: `172.20.10.2`
  - PICO 4 Ultra: `172.20.10.3`
- Updated Android uplink target in:
  - `D:\videotest\android-native\app\src\main\cpp\openxr_pose_sender.cpp`
- Rebuilt and reinstalled the Android debug APK

### Problem

- The previous environment looked like the PC and headset were on the same `10.51.x.x` network, but they were not actually mutually usable for development
- In addition, the Android app still had the old PC IP hard-coded for pose/control uplink

### Fix

- Switched to a hotspot network with direct reachability
- Updated the Android target host to the new PC IP
- Reinstalled the app and relaunched it

### Verification

- OpenXR app lifecycle now reaches the active running state:
  - session state progressed through `1 -> 2 -> 3 -> 4 -> 5`
- Android activity state now shows the app itself in the focused/resumed foreground path
- Real pose uplink was verified on Windows:
  - `pose_receiver.exe` received live packets from `172.20.10.3`
  - observed about `89 pps`
- Real H.264 downlink was verified again with desktop sender:
  - Android received codec config
  - `MediaCodec configured for 1280x720 H.264`
  - `MediaCodec rendered first output frame`
- Real pose-driven sender was verified again:
  - `pose_scene_nvenc_sender.exe` received live pose packets
  - sender switched to `pose=active`
  - encoded frames were continuously produced and sent

### Important nuance

- `PC -> headset` ICMP ping succeeded on the hotspot
- `headset -> PC` ICMP ping still failed
- But UDP pose uplink to the PC **did** work in practice

### Interpretation

- On this machine, failed inbound ICMP is **not** a reliable indicator that our UDP app traffic is blocked
- For this project, protocol-level validation is more meaningful than raw ping alone

### Tradeoff / impact

- Hotspot is an excellent short-term development network because it removes local infrastructure uncertainty
- But the IPs are likely ephemeral, so any future hard-coded target host must be checked again if the hotspot session changes

---

## Update 2026-04-10 (Configurable Target and Last-Successful Address)

### What changed

- Removed the need to hard-code the PC IP in:
  - `D:\videotest\android-native\app\src\main\cpp\openxr_pose_sender.cpp`
- Added native runtime-config resolution and persistence:
  - `D:\videotest\android-native\app\src\main\cpp\runtime_config_store.h`
  - `D:\videotest\android-native\app\src\main\cpp\runtime_config_store.cpp`
- Added support for launch-time overrides through `Intent` extras
- Added persistence of the last successful runtime config after the first decoded frame is rendered

### New config priority

- Android runtime now resolves config in this order:
  1. built-in defaults
  2. last successful persisted config
  3. explicit `Intent` extras

This means:

- normal launch can reuse the last successful PC address automatically
- temporary overrides can still be injected at launch time without rebuilding the APK

### Implemented override keys

- `target_host`
- `target_port`
- `video_port`
- `encoded_video_port`

### Practical launch example

```powershell
adb shell am force-stop com.videotest.nativeapp
adb shell am start -n com.videotest.nativeapp/android.app.NativeActivity --es target_host 172.20.10.2 --ei target_port 25672 --ei video_port 25673 --ei encoded_video_port 25674
```

### Success persistence trigger

- We intentionally do **not** persist the config on mere startup
- The config is only written after:
  - encoded stream is received
  - decoder produces output
  - first decoded frame is actually rendered

### Why this trigger was chosen

- It avoids treating a mere attempted connection as success
- It records an address only after the full practical media path has worked

### Verification

- Launch-time override validation passed:
  - app logged `source=defaults+intent`
- Persistence validation passed:
  - after first rendered decoded frame, app logged:
    - `Saved last successful runtime config ...`
  - persisted file was confirmed under app internal storage
- Restart without `Intent` extras also passed:
  - app logged `source=defaults+persisted`

### Tradeoff / impact

- This is the right near-term balance for development:
  - no rebuild required when network changes
  - still deterministic and easy to debug
- It is still not full service discovery
- If the PC address changes and no new successful session is established, the persisted address may become stale

### Next likely refinement

- Add UDP service discovery on top of this foundation
- Keep persisted-config fallback even after discovery is introduced

---

## Update 2026-04-11 (Unity Native Plugin Scaffold)

### What changed

- Added a first-pass Unity-facing Windows native plugin target:
  - `D:\videotest\windows-native\src\unity_sender_plugin.cpp`
  - `D:\videotest\windows-native\include\unity_sender_plugin.h`
- Added a Unity integration helper folder:
  - `D:\videotest\unity-integration\README.md`
  - `D:\videotest\unity-integration\UnitySenderPluginBindings.cs`
- Added CMake auto-detection for Unity PluginAPI headers
- Built:
  - `D:\videotest\windows-native\build\Debug\unity_sender_plugin.dll`

### Why this step matters

- The project is now ready to stop treating Unity as an abstract future step
- We now have a concrete native DLL boundary that Unity can call into
- This is the correct next step because the shared sender core already exists

### What the plugin currently does

- Integrates with Unity's native plugin lifecycle:
  - `UnityPluginLoad`
  - `UnityPluginUnload`
- Integrates with Unity's render-thread callback path:
  - `UnitySender_GetRenderEventFunc`
  - `UnitySender_GetCopyTextureEventId`
- Accepts a Unity native texture pointer
- Copies Unity's D3D11 `RenderTexture` into a plugin-owned D3D11 texture on Unity's render thread
- Runs a separate Windows network thread for:
  - pose receive
  - control receive
- Runs a separate sender thread that reuses the existing shared NVENC sender core

### What was explicitly verified before implementation

- Unity official docs confirm that:
  - `Texture.GetNativeTexturePtr()` returns `ID3D11Resource*` on D3D11
  - `GetNativeTexturePtr()` is slow when it forces render-thread sync, so it should be cached and refreshed only when the texture changes
  - `GL.IssuePluginEvent(...)` invokes a native callback on Unity's render thread
- Unity's official native plugin sample confirms:
  - plugin device-event handling through `IUnityGraphics`
  - render-thread event callbacks as the intended low-level rendering integration path
- Microsoft D3D11 docs confirm that:
  - `ID3D11Device` is thread-safe
  - `ID3D11DeviceContext` is not thread-safe
  - `CopyResource` requires matching resource type and dimensions

### Design consequences

- The plugin does **not** run NVENC directly inside Unity's render callback
- Instead, it separates:
  1. render-thread copy
  2. network receive
  3. sender encode/send
- A mutex is used around the shared D3D11 immediate-context access
- The first-pass implementation requires:
  - Unity on Windows
  - Unity renderer = D3D11
  - fixed `RenderTexture` size while streaming

### Verification

- `unity_sender_plugin.dll` built successfully
- Exported API was verified with `LoadLibrary/GetProcAddress`:
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

### Current limitations

- No Unity project has been initialized in this repository yet
- The plugin currently assumes D3D11 only
- No Unity-side coordinate-conversion layer has been finalized yet for the returned pose
- No resize handling is implemented for source textures while streaming

### Next likely refinement

1. Create a minimal Unity Windows project
2. Attach a fixed-size `RenderTexture`
3. Wire `GetNativeTexturePtr()` + `GL.IssuePluginEvent(...)`
4. Poll `UnitySender_GetLatestPose(...)` from C#
5. Validate the first real Unity scene through the existing headset decode path
