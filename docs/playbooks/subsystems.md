## Subsystem playbooks

### Audio

- Never rely on `dumpsys media_session` or app-level `PlaybackState` to conclude
  audio is playing. Verify with `dumpsys media.audio_flinger`: check
  `Standby: yes/no`, active tracks, written frames, underruns.
- logcat main/system wrap fast when verbose HALs flood them (e.g.
  `powerhal-libperfmgr` `sched_setattr`). Check the audio HAL PID, `dmesg`, and
  direct dumpsys before concluding logs are missing.
- Stock MTK audio uses DTS processing (`libdtsaudio.so`, `mtk_bessound`). In
  AOSP, device effects with `EFFECT_FLAG_TYPE_POST_PROC` can be rejected on
  stream output sessions unless patched or given a software fallback.
- If speaker media or game audio is disproportionately quiet at mid slider
  (e.g. 50%), check `AUDIO_STREAM_MUSIC` `DEVICE_CATEGORY_SPEAKER` in
  `configs/audio/audio_policy_volumes.xml`: AOSP's default curve applies
  -20.8 dB attenuation at step 8 (~9% linear amplitude). Use a balanced
  curve (`<point>1,-4500</point> <point>33,-2500</point> <point>66,-1100</point>`)
  and ensure `ro.vendor.mtk_besloudness_support=1` in `vendor.prop` for DRC.

### Wi-Fi Sharing / Tethering

- On legacy 4.14 kernels without eBPF TC tethering packet forwarding support,
  tethered clients will connect but receive no internet routing if BPF offload
  is active. Set `<bool name="config_tether_enable_bpf_offload">false</bool>`
  in `overlay/TetheringConfigTarget/res/values/config.xml` to force fallback
  to Netfilter/iptables MASQUERADE. Verify with `dumpsys tethering`
  (`enableBpfOffload: false`).
- **Hotspot WPA2/WPA3 (`SECURITY_TYPE_WPA3_SAE_TRANSITION`) failures**:
  - Check `dumpsys wifi | grep -E "SupportedFeatures=|config_wifi_softap_sae_supported"`
    and reproduce cleanly from shell with
    `cmd wifi start-softap "TestAP" wpa3_transition "12345678"` (and `cmd wifi stop-softap`).
  - If `config_wifi_softap_sae_supported` is `false` in `WifiResTarget`,
    `SoftApCapability` omits `SOFTAP_FEATURE_WPA3_SAE` (`SupportedFeatures=43`
    instead of `47`). Settings' Android 14/15 `WifiHotspotSecuritySettings` still
    lets users select WPA2/WPA3 (`SecurityType=2` in
    `/data/misc/apexdata/com.android.wifi/WifiConfigStoreSoftAp.xml`), which
    `ApConfigUtil` then rejects with `Error, SAE requires HAL support`
    (`SoftApState mFailureReason=2`). `wlan_drv_gen4m.ko` and AIDL `hostapd`
    support both `wpa3_transition` and `wpa3` when `config_wifi_softap_sae_supported`
    is `true`.
- **Client connects & gets DHCP IP but has no internet (DNS failure)**:
  - Verify `dnsmasq` and port 53/67 listeners while hotspot is active:
    `ps -eZ | grep dnsmasq`, `ss -tulnp | grep -E ":53|:67"`, and
    `dumpsys netd | grep -iE "dnsmasq|tetherStart|tetherDns"`.
  - `NetworkStack` `DhcpServer` (`IpServer.java`) advertises the downstream
    `ap0` IP as the client DNS server. `netd` (`TetherController.cpp`) only
    launches `dnsmasq` with port-53 DNS forwarding when `usingLegacyDnsProxy=true`
    in `Tethering.java` (maintained via `fuck-bpf` patch
    `packages/modules/Connectivity/0022-Tethering-restore-legacy-DNS-proxy-for-dnsmasq.patch`
    reverting `0019`).
  - Do **not** set `config_tether_enable_legacy_dhcp_server=true` in
    `TetheringConfigTarget` as a workaround: when `usingLegacyDnsProxy=false`,
    `TetherController` still passes `--port=0` (`DNS disabled`), and `dnsmasq`'s
    `192.168.42–47.x` ranges do not match `IpServer`'s randomized `ap0` subnet,
    breaking DHCP lease assignment too.

### USB and Ports

- Kernel 4.14 uses `/sys/class/dual_role_usb/dual-role-usb20` (`musb-hdrc`), NOT
  Linux Type-C connector class (`/sys/class/typec/`).
- Do not use AIDL USB (`android.hardware.usb-service.mediatek`): it relies on
  `/sys/class/typec/` and reports 0 ports on this kernel.
- Use the prebuilt `android.hardware.usb@1.1-service-mediatek`. MTP needs it:
  AOSP's `MtpServer` is FunctionFS-based and opens `/dev/usb-ffs/mtp/ep0`
  (`frameworks/av/media/mtp/MtpDescriptors.h`). This kernel has no
  `CONFIG_USB_F_MTP`, so configfs `mtp.gs0` is a stub and
  `usb_get_function name=mtp` never resolves. MTK implements MTP through the
  legacy `android_usb` gadget (`/dev/mtp_usb`), which only the vendor HAL
  drives. With `dual_role_usb`, `MtpService` logs `Couldn't get control FD!`.
  The prebuilt needs these in `device.mk` because the gadget libs are
  otherwise only pulled in by other services:
  `android.hardware.usb.gadget@1.0`, `@1.1`, `android.hardware.usb@1.0`, `@1.1`.
- Declare `@1.1::IUsb/default` and `@1.1::IUsbGadget/default` in `manifest.xml`.
  The prebuilt installs no VINTF fragment of its own, unlike `dual_role_usb`.
- `device/mediatek/sepolicy_vndr` `base/vendor/file_contexts` already labels
  `android\.hardware\.usb@1\.[0-9]+-service-mediatek` as `mtk_hal_usb_exec`.
- After switching HALs, delete any stale `android.hardware.usb@*` fragment left
  under `out/target/product/X6812/vendor/etc/vintf/manifest/` before running
  `checkvintf --check-one` — a leftover of the other version conflicts as
  duplicate FqInstance.

### Telephony and RIL (HIDL Radio & MtkRadioEx)

- MediaTek's telephony stack registers `vendor.mediatek.hardware.mtkradioex` at
  version **`@2.2`** (via `librilfusion.so` in `mtkfusionrild`).
- If `manifest.xml` declares `@2.0` instead of `@2.2`, HIDL rejects registration:
  `Service vendor.mediatek.hardware.mtkradioex@2.2::IMtkRadioEx/imsSlot1 must be in VINTF manifest in order to register/get`.
- This rejection causes clients (`ImsService.apk`) to fall back to starting it
  as a lazy service, flooding logcat with:
  `hwservicemanager: Since vendor.mediatek.hardware.mtkradioex@2.0::IMtkRadioEx/imsSlot1 is not registered, trying to start it as a lazy HAL`
  `libc: Unable to set property "ctl.interface_start" to "vendor.mediatek.hardware.mtkradioex@2.0::IMtkRadioEx/imsSlot1": PROP_ERROR_HANDLE_CONTROL_MESSAGE (0x20)`
- Declare `@2.2::IMtkRadioEx/*` in `configs/manifests/manifest.xml`, matching stock. The `@2.2` entries
  allow stock MT6768 `mtkfusionrild` (`librilfusion.so`) to register.
- Do **not** declare `@3.0::IMtkRadioEx/*`. There is no `@3.0` implementation on this device: `librilfusion.so`
  only knows `@2.0`/`@2.1`/`@2.2` and `/vendor/lib64` ships only those three stubs. Declaring `@3.0` makes
  `hwservicemanager` resolve the transport, so every client `getService` turns into a lazy-start attempt:
  `Since vendor.mediatek.hardware.mtkradioex@3.0::IMtkRadioEx/imsSlot1 is not registered, trying to start
  it as a lazy HAL` + `Tried to start ... but was unable to` + `init: Control message: Could not find ...`
  + `libc: PROP_ERROR_HANDLE_CONTROL_MESSAGE (0x20)` — four lines per second per instance, and `lshal`
  shows every instance as `DM,FC` with no PID.
- The requester is the client, not the manifest. `ImsService.apk` v13 (from the `X6812B` Lineage build,
  `sharedUserId android.uid.phone`) probes `@3.0::IMtkRadioEx` and the AIDL `IMtkRadioExIms`/`IMtkRadioExVoice`/
  `IMtkRadioExModem` slots once per second and keeps doing so regardless of what is declared. Fix belongs in
  the client path (disable the probing component or ship the AIDL HAL), never in phantom declarations.

### Battery and Charging

- Transsion/MediaTek kernel fuelgauge drivers (`fuelgauged`) report scaled-down
  ticks in sysfs `charge_full_design` (e.g. `231000` instead of `5000000`).
- The true battery design capacity is exported at `/sys/devices/platform/tran_battery/BATTERY_QMAX`
  (`5000`). Label this node as `u:object_r:sysfs_batteryinfo:s0` in
  `sepolicy/vendor/file_contexts` so `hal_health_default` can read it.
- In `hardware/mediatek/aidl/health/Health.cpp`, override `UpdateHealthInfo()` to
  read `BATTERY_QMAX` and populate `batteryFullChargeDesignCapacityUah = 5000000`.

### Display & Refresh Rate

- To expose the 90 Hz high refresh rate toggle in Settings -> Display, overlay
  `<bool name="config_show_smooth_display">true</bool>` in `overlay/SettingsResTarget/res/values/config.xml`.
- Verify availability with `cmd overlay dump com.android.settings.target`.
- To keep 90 Hz active without `min_refresh_rate=90`, BOTH are required:
  1. `<integer name="config_defaultRefreshRate">0</integer>` in
     `overlay/FrameworksResTarget/res/values/config.xml` (leaving it at AOSP's
     default `60` causes `PRIORITY_DEFAULT_RENDER_FRAME_RATE` to vote
     `RenderVote(0, 60)` and cap `primaryRanges` render rate to `60 Hz`).
  2. Do **not** set `ro.surface_flinger.enable_frame_rate_override=false` in
     `configs/props/vendor.prop`: when `false`, `VoteSummary` and
     `RefreshRateSelector` discard physical modes outside the `render` range and
     `Scheduler` sets `defaultTouchTimerValue=0`.
- Verify live state and isolate framework voting from HWC mode switching:
  ```bash
  dumpsys display | grep -E "mActiveMode|mActiveRenderFrameRate|mDesiredDisplayModeSpecs|PRIORITY_"
  dumpsys SurfaceFlinger | grep -E "renderRate=|activeMode=|displayManagerPolicy=|frameRateOverrideConfig="
  # Force 90 Hz bypassing PRIORITY_DEFAULT_REFRESH_RATE (revert with delete afterwards):
  settings put system min_refresh_rate 90
  settings delete system min_refresh_rate
  ```

### Touch gestures / DT2W

- Inspect driver wake keys in the kernel (`Image.gz` disassembly of
  `*_ts_work_func`). MTK panels (Novatek, Focaltech) often emit key 183
  (`KEY_F13`) or 143 (`KEY_WAKEUP`) to `/dev/input/event*` while suspended.
- Without a custom keylayout (`usr/keylayout/mtk-tpd.kl`) Android falls back to
  `Generic.kl`, where 183 maps to `F13` — not marked `WAKE` in
  `PhoneWindowManager` — so the wake event is discarded.
- Verify PowerHAL `powerhint.json`: `DoubleTapToWakeEnable` uses
  `/proc/gesture_function`, requiring `"cc1"` to enable and `"cc2"` to disable.

### Display cutout / status bar padding

- Inspect the physical punch-hole position on the device, then compare
  `FrameworksResTarget` `config_mainBuiltInDisplayCutout` and
  `status_bar_height_portrait` against the stock RRO values.
- In `SystemUIResTarget` check `rounded_corner_content_padding`,
  `status_bar_padding_start`, `status_bar_padding_top`. Excess padding or a
  missing status bar height causes clock/icon clipping or asymmetric alignment.

### Offline charging (`kpoc_charger`)

- MTK `kpoc_charger` is a **32-bit ARM** executable (`system/bin/kpoc_charger`)
  requiring 32-bit `system/lib/libshowlogo.so`. Extract both and set
  `compile_multilib: "32"` in `libshims/Android.bp`.
- Add `PRODUCT_PACKAGES += libsuspend` in `device.mk`.
- `init.mt6768.rc` in `on charger` must start **`vendor.light-default`** (the
  AIDL light HAL name, not `vendor.light`) and chown the LED brightness nodes.
- Remove any `service vendor.charger /system/bin/charger` from
  `init.mt6768.rc` — `kpoc_charger.rc` already defines `service kpoc_charger`
  in `class charger`.
- 32-bit Android 15 `libgui`/`libui` shimming pitfalls:
  1. Returning `sp<T>` in `extern "C"` triggers `-Wreturn-type-c-linkage` under
     `-Werror`. Under ARM32 AAPCS `sp<T>` returns via an sret pointer in `r0`;
     use placement `new (out) sp<T>(...)` with `sp<T>* out` first.
  2. `SurfaceComposerClient.h` has `using gui::LayerMetadata;` inside
     `namespace android`. Do not declare `struct LayerMetadata` in the same TU;
     split `android::LayerMetadata` ctor/vtable symbols into
     `libshim_showlogo_metadata.cpp`.
  3. In `getActiveDisplayConfig`, Android 15 `ui::DisplayMode` has `id` at `+0`
     and `std::vector` at `+20`. Never cast `DisplayConfig*` to
     `ui::DisplayMode*` (stack corruption, 4-byte offset shift); copy
     `resolution`, `dpi`, `refreshRate` explicitly.

### Sensors

- On Infinix X6812B, the magnetometer is MEMSIC `mmc5603_x6812` and the accelerometer is Silan `sc7a20`.
- The device has no discrete hardware gyroscope chip; MediaTek implements virtual gyroscope (`getVirtualGyro`) and sensor fusion (Orientation, Game Rotation Vector, Geomagnetic Rotation Vector, Rotation Vector, Gravity, Linear Acceleration) inside `sensors.mt6768.so` (`VendorInterface`).
- At startup, `sensors.mt6768.so` reads `/sys/class/sensor/m_mag_misc/maglibinfo` (`mmc5603_x6812`) and dynamically executes `dlopen("libmmc5603_x6812.so", 2)` to load MEMSIC's sensor fusion library.
- If `vendor/lib64/libmmc5603_x6812.so` is missing from `proprietary-files.txt`, `VendorInterface` logs `get lib_interface fail dlopen operation`. As a consequence, Magnetometer, Gyroscope, Orientation, and all 6 fusion sensors return all zeros (`0.00, 0.00, 0.00`) and appear completely dead in apps like DevCheck.
- Keep both `vendor/lib/libmmc5603_x6812.so` and `vendor/lib64/libmmc5603_x6812.so` in `proprietary-files.txt` under `# Sensors`.

### UI jank and power

- Jank: check both 60 Hz and 90 Hz; `dumpsys SurfaceFlinger` (HWC/GPU missed
  frames, phase offsets), `dumpsys gfxinfo <pkg> framestats`
  (UI/RenderThread/GPU), FrameTracker CUJ logs, and process scheduling groups
  (`top-app` vs `foreground`).
- Power: PowerHAL node fds and hint stats
  (`dumpsys android.hardware.power.IPower/default`), CPU/GPU clock scaling
  (`/sys/devices/system/cpu/cpufreq/`, `/sys/kernel/ged/hal/`), EAS/stune
  (`/dev/stune/`). Watch for kernel ABI mismatches, e.g. a 4.14 kernel rejecting
  newer uclamp `sched_setattr` with `E2BIG`/`err=7`.

#### Benchmarking

Use a deterministic sequence, not gestures. `input swipe` opens or closes the
shade depending on the state it is already in, so the frame count varies 4x
between identical runs and no two numbers are comparable:

```bash
cmd statusbar collapse; sleep 0.5
dumpsys gfxinfo com.android.systemui reset >/dev/null
i=0; while [ $i -lt 4 ]; do
  cmd statusbar expand-notifications; sleep 0.9
  cmd statusbar collapse; sleep 0.9; i=$((i+1))
done
dumpsys gfxinfo com.android.systemui | grep -E "Total frames|Janky frames:|percentile"
```

##### App Open/Close & Interaction Benchmarking

Profile both the opening activity and `com.android.launcher3` (which renders the icon zoom and window leashes):

```bash
# Reset and run deterministic warm open-close cycles
for app in com.android.dialer/.main.impl.MainActivity org.lineageos.jelly/.MainActivity com.android.settings/.Settings; do
  am start -n $app; sleep 1.0; input keyevent KEYCODE_HOME; sleep 1.0
done
dumpsys gfxinfo com.android.launcher3 | grep -E "Total frames|Janky frames:|percentile"
```

Note on sample size: Cold activity launch (`am force-stop` followed by `am start`) renders only ~25 frames total during XML inflation and texture creation; a single delayed frame in 25 frames mathematically registers as ~4% jank. Normal interactive app switching and scroll workloads easily achieve sub-2% jank on tuned configs (e.g. Settings scroll 0.19%, Launcher swipe 0.21%, Dialer scroll 0.52%, Jelly 1.91%).

##### 3D GPU Gaming Benchmark (`scripts/benchmark_gpu.py`)

Run the standalone, offline WebGL 3D GPU stress test (12,000 instanced lit triangles with sub-ms frame telemetry):

```bash
# VSYNC-capped benchmark (evaluates 90Hz display pacing & stability):
python3 scripts/benchmark_gpu.py --duration 15

# Unthrottled GPU throughput benchmark (measures raw Mali-G52 hardware FPS & thermals):
python3 scripts/benchmark_gpu.py --unthrottled --duration 15
```

The harness automatically sets up `adb reverse`, launches `org.lineageos.jelly`, and samples real-time Mali-G52 telemetry:
- GPU Clock: `/sys/kernel/ged/hal/current_freqency`
- GPU Utilization: `/sys/kernel/ged/hal/gpu_utilization`
- SoC Temperature: `/sys/class/thermal/thermal_zone3/temp` (`mtktscpu`)
It outputs average framerate, 1% low FPS, jank %, frame time percentiles (p50/p90/p95/p99), and clock/thermal scaling.

"Achieved frame rate" needs `Total frames rendered` divided by the wall time of
the sequence; the percentiles alone cannot distinguish 40 fps from 90 fps,
because a consistently late surface gets a multi-vsync `FrameDeadline` and is
then reported as *not* janky.

Pitfalls that invalidate a measurement:

- `debug.hwui.renderer` set with `setprop` **sticks for every process started
  afterwards**. A bench run after a `skiavk` experiment measures Vulkan, not the
  shipped default. Confirm the backend with
  `dumpsys gfxinfo <pkg> | grep -m1 Pipeline` before trusting a number. The
  shipped default is OpenGL (`ro.hwui.use_vulkan` is unset/empty).
- Every line of the dump must come from the same process generation. Restarting
  SystemUI resets the counters, so `reset` *after* the restart, wait ~8 s, wake
  the device (`input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard`), and check
  that the first run is not `Total frames rendered: 0`.
- `killall com.android.systemui` and `pidof com.android.systemui` can fail with
  `Operation not permitted` / no output under APatch `su`; `kill -9 <pid>` with
  the PID from `ps` works.
- Killing the power HAL leaves `PowerManagerService` holding a dead binder
  (`PowerHalController: setMode failed ... DEAD_OBJECT`). The first hint after
  the reconnect can be dropped, so give it ~2 s and verify with
  `dumpsys android.hardware.power.IPower/default` that the node's
  `Current Index` matches the hint you expected.

#### MTK CPU and GPU boost nodes

`configs/powerhint.json` runs on `hal_power_default`, and only some MTK nodes
actually do anything. Verified on device by triggering a hint and reading the
node back:

- `/sys/devices/system/cpu/cpufreq/policy*/scaling_min_freq` — **ignored**. PPM
  owns the frequency floor: a LAUNCH hint asking for 1800000/2000000 leaves
  `scaling_min_freq` at 500000/850000.
- `scaling_max_freq` — works, but only caps. The old config capped the little
  cluster to 1625000 on every touch, i.e. it downclocked the cluster that
  scrolls the UI.
- `/proc/perfmgr/boost_ctrl/eas_ctrl/perfserv_{ta,fg,bg}_boost` (`-100..100`) and
  `perfserv_{,ta_,fg_,bg_}uclamp_min` (`0..100`) — the sanctioned boost path,
  same set the stock MTK HAL uses in `vendor/etc/powercontable.xml`. Writes from
  `hal_power_default` take effect.
- `/proc/perfmgr/boost_ctrl/cpu_ctrl/perfserv_freq` — `"<c0min> <c0max> <c1min>
  <c1max>"`, `-1` leaves a value alone. This is the only node that moves the
  reported min frequency, and it is not used by stock.
- `/dev/stune/*/schedtune.prefer_idle` — **cannot** be written by the power HAL
  (`Failed to write to node ... fd: -1`); only `schedtune.boost` is granted via
  the `cgroup` label. The MTK equivalent is
  `/proc/perfmgr/boost_ctrl/eas_ctrl/perfserv_prefer_idle`, which is a
  multi-line per-cgroup array, so do not feed it a single value.
- Every node with `ResetOnInit` needs an explicit `DefaultIndex`. Without one
  libperfmgr logs `Failed to read Node[N]'s DefaultIndex, set to last index` and
  resets the node to its **last** value at HAL start — e.g. a permanent 10 %
  `top-app` `schedtune.boost`.
The stock dump's `vendor/etc/powercontable.xml` is the authoritative reference
for a node's meaning and range (`MaxValue`/`MinValue`/`DefaultValue`).

- **Schedutil rate limits**: `/sys/devices/system/cpu/cpufreq/schedutil/up_rate_limit_us` (500)
  and `down_rate_limit_us` (5000) set in `init.mt6768.power.rc`. The 1 ms default down rate
  decayed CPU frequencies between consecutive frame presentations; 5 ms holds clocks across
  VSYNC intervals to eliminate micro-stutters.
- **Mali GPU DVFS & GED scaling**:
  `/sys/kernel/ged/hal/dvfs_margin_value = 50` in `init.mt6768.rc` and `init.mt6768.power.rc`
  (stock `on property:sys.boot_completed=1` clamped it down to 10, starving the Mali-G52 at
  299 MHz during 3D loads). `/sys/module/ged/parameters/gx_force_cpu_boost = 1` and
  `/sys/devices/platform/13040000.mali/dvfs_period = 50` cut DVFS sampling time in half.
- **PowerHAL Gaming & Launch Profiles (`configs/powerhint.json`)**:
  - `LAUNCH`: Sets `DRAMOppMin = 0` (OPP 0 maximum memory bandwidth) and triggers MTK EAS
    `perfserv_ta_boost = 100` and `perfserv_fg_boost = 100` to migrate top-app threads to
    the Cortex-A75 cluster during window creation.
  - `SUSTAINED_PERFORMANCE` & `FIXED_PERFORMANCE`: `GPUCustomBoostFreq = 20/16`,
    `DRAMOppMin = 0`, `CCIFreqMode = 1`, `PerfservFreq = "1800000 -1 2000000 -1"`, and
    `TABoostPerf = 100`.
- **Boot-time DEX2OAT (`configs/props/system.prop`)**:
  `dalvik.vm.boot-dex2oat-cpu-set=0,1,2,3,4,5,6,7` and `dalvik.vm.boot-dex2oat-threads=8`.
  Without these, ART `odrefresh` and `artd` fall back to `0,1,2,3,4,5` (6 threads),
  completely excluding both 2.0 GHz Cortex-A75 performance cores during boot compilation.
- **Storage Queue Depth (`rootdir/etc/init/hw/init.mt6768.rc`)**:
  `write /sys/block/mmcblk0/queue/nr_requests 64` on boot completion. Shaves ~7 ms off cold
  app starts on eMMC 5.1. Note that `/sys/block/sdc` (UFS) does not exist on this SoC.

Current known bottleneck: the notification shade. Its frames take ~25-30 ms
(`p50`) regardless of any power hint or `hwc.min.duration` tried so far.
`dumpsys SurfaceFlinger` shows the wallpaper layer as `CLIENT`-composited while
six layers are visible (HWC runs out of overlays), and the shade's own GPU stage
is 10-15 ms, but the layer count is not the cause: over an opaque app there are
0 CLIENT layers and the GPU stage is unchanged.

#### SurfaceFlinger phase offsets

These are the highest-leverage frame-pacing knob, converting offsets to work durations in
`PhaseOffsets::constructOffsets`:

    sfWorkDuration  = vsyncDuration - sfOffset
    appWorkDuration = vsyncDuration + (sfOffset - appOffset)

Tuned production configurations in `configs/props/vendor.prop`:

1. **90 Hz High-FPS Mode (`vsyncDuration = 11.11 ms`)**:
   ```properties
   debug.sf.high_fps_late_sf_phase_offset_ns=0
   debug.sf.high_fps_late_app_phase_offset_ns=-14000000
   debug.sf.high_fps_early_phase_offset_ns=0
   debug.sf.high_fps_early_app_phase_offset_ns=-14000000
   debug.sf.high_fps_early_gl_phase_offset_ns=0
   debug.sf.high_fps_early_gl_app_phase_offset_ns=-14000000
   ro.surface_flinger.max_frame_buffer_acquired_buffers=4
   ```
   - `sfOffset = 0 ns`: Gives SurfaceFlinger exactly **1 full 11.11 ms VSYNC frame** to latch and composite to HWC.
   - `appOffset = -14000000 ns`: Expands `appWorkDuration` to **25.11 ms** (~2.3 vsyncs), giving the launcher leash and opening activity sufficient headroom to inflate XML, measure, layout, and upload textures.
   - `max_frame_buffer_acquired_buffers = 4`: Absorbs transient buffer bursts during window transitions.
   - Measured: Launcher3 leash jank dropped to 0.84%, Dialer to 1.83%, Messaging to 1.83%, Calculator to 2.80%, Jelly to 3.62%, and Settings to 9.35%. Interactive scroll workloads achieve sub-1% jank (Settings 0.00%, Launcher 0.21%, Dialer 0.00%, Jelly 0.00%, Messaging 0.00%).

2. **60 Hz Standard Mode (`vsyncDuration = 16.67 ms`)**:
   ```properties
   ro.surface_flinger.vsync_sf_event_phase_offset_ns=0
   ro.surface_flinger.vsync_event_phase_offset_ns=-4000000
   ```
   - `sfOffset = 0 ns`: Exactly **1 full 16.67 ms VSYNC frame** (`sfWorkDuration = 16.67 ms`).
   - `appOffset = -4000000 ns`: Expands `appWorkDuration` to **20.67 ms**.
   - Measured: 60 Hz combined jank dropped from 7.64% to 5.33% (Launcher 3.6%, Messaging 3.1%).

3. **GL Backpressure (`debug.sf.enable_gl_backpressure`)**:
   Must stay **`0`** in `vendor.prop`. AOSP SurfaceFlinger defaults to `true`, which forces GPU queue stalls on Mali-G52, spiking launcher jank to 23% with 472 `sfLongGpuJankyFrames`.

For the Settings workload, `pm compile -m speed-profile` beats `-m speed`:
interleaved over 3 rounds it wins 3/3 (15.60/14.55/15.89 % vs
16.04/17.92/14.95 %) and the shade is a wash. That is also what the tree asks
for - `pm.dexopt.install=speed-profile` and `pm.dexopt.bg-dexopt=speed-profile`
in `configs/props/system.prop`. A device left in `speed` by a manual
`pm compile` will read a couple of points worse than the shipped state, so
check `dumpsys package com.android.settings | grep -A3 "Dexopt state"` before
trusting a Settings number.

`debug.sf.hwc.min.duration` was re-tested after the offsets were fixed and is a
wash between 3 ms and 5 ms (shade 10.36/6.97 vs 10.97/8.23). It stays at 5 ms
because there is no gain to buy by changing it.

`debug.hwui.use_hint_manager` and `debug.sf.enable_adpf_cpu_hint` do have an
effect, but the trade is not worth taking: interleaved A/B over 3 rounds shows
the HWUI hint manager winning the shade 3/3 (7.25/7.35/7.97 % vs
8.27/7.77/8.11 %) while losing the launcher 3/3 (6.11/6.67/6.84 % vs
5.70/6.58/6.49 %). `true` also needs `target_cpu_time_percent` near the AOSP
default of 70; the tree's 30 is too low.

After all of the above the remaining jank is app-bound, not config-bound: a
Perfetto trace of the transitions shows Settings' main thread `traversal`
averaging 8.1 ms (max 92 ms) on an AOT-`speed` app with uclamp pinned to max,
and 62 of 67 shade-expansion frames landing on an exact 11 ms cadence.

### Camera & Media (`TranssionCamera` / Codec2)

Debugging steps:

1. Separate app crashes from HAL/kernel failures: clear tombstones and logs,
   exercise one mode at a time (`AI CAM`, `VIDEO`, `BEAUTY`, `PORTRAIT`,
   `SUPER NIGHT`, `DOCUMENTS`), then inspect `logcat -b crash -d`,
   `/data/tombstones/`, and `dmesg | grep -E "avc: denied|MTK_V4L2|MTK_VCODEC"`.
2. Isolate Codec2/VPUD from `StagefrightRecorder`: test `screenrecord --time-limit 2 /data/local/tmp/sr.mp4`
   while watching `logcat -b main -d -s VPUD:* C2MtkVenc:* MPEG4Writer:* MediaRecorder:*`.
3. Test APK smali/dex changes live without rebuilding by signing with
   `testkey` and bind-mounting over `/system_ext/app/TranssionCamera/TranssionCamera.apk`,
   then `stop && start`; test vendor `.so` changes with `adb push`, `chcon u:object_r:vendor_file:s0`,
   and `stop camerahalserver && start camerahalserver` (or `killall vpud`).
4. Screen recording green screen: VirtualDisplay buffers (`GraphicBufferSource`,
   usage `0x10300`) will allocate as AFBC if `vpu_runtime_caps` is 0 in
   `android.hardware.graphics.{allocator,mapper}@4.0-impl-mediatek.so`.
   `libdpframework.so` cannot decode AFBC in `DpEngine_RDMA` (`config frame error -9`),
   leaving output YV12 frames zeroed (solid green).
5. Screen recording green line / shadowing: odd multiples of 16 width
   (e.g. 1072x2448) cause gralloc4 to align chroma stride to 16 bytes
   (`uvPitch=544`), while `MTK_V4L2` venc expects `yPitch/2=536`. Keep
   `<Limit name="alignment" value="32x16" />` in `media_codecs_c2.xml` so
   encoders align width to 32 (e.g. 1056x2432).
6. Screen recording crash on start (`Cannot find throughput setting`):
   SystemUI (`ScreenRecordPermissionDialogDelegate.kt`) defaults to HEVC
   if `c2.mtk.hevc.encoder` is declared. MT6768 `libvcodecdrv.so`
   (`HEVC_ThroughputSetting`) has no throughput profile for 90 fps and rejects
   resolutions > 1080p if framerate != 30 fps. Do not declare `c2.mtk.hevc.encoder`
   in `media_codecs_c2.xml` (align with stock, which disabled C2 HEVC encoder);
   keep `config_screenRecorderMaxFramerate` capped to 60 in `SystemUIResTarget`;
   and add `<Feature name="can-swap-width-height"/>` to `c2.mtk.avc.decoder`
   and `encoder` so portrait `1080x2460` does not fail decoder limits and get
   downscaled to `948x2160`.
7. Video playback failure (`VIDIOC_QBUF Failed: Invalid argument` / `dma_general_buf is err`):
   Prebuilt `libcodec2_mtk_vdec.so` from Xiaomi mt6768 cannot queue DMABUFs to
   Infinix's 4.14.186 kernel `vcodec_dec` driver on `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`,
   failing in `vb2ops_vdec_buf_prepare`. All `c2.mtk.*.decoder` video decoders abort
   immediately on every video. Do not declare `c2.mtk.*.decoder` video decoders in
   `media_codecs_c2.xml`. In addition, prebuilt `libcodec2_mtk_c2store.so` hardcodes
   a static loader table that registers `c2.mtk.*.decoder` directly; patch its reference
   to `libcodec2_mtk_vdec.so` via `extract-files.py` (`sig_replace`) so it does not
   register broken decoders into C2 store, allowing video players (NewPipe, ExoPlayer)
   to use Google's optimized NEON `c2.android.*.decoder` software decoders without crashing.

- `logcat` floods at 30+ fps with `TranCam/Plugin_TranStream` and `Hal3ARaw`;
  always query with `logcat -b main,crash -d -t 500 '*:W'` or `-s <tags>`, and
  check `/data/tombstones/` directly for `camerahalserver` (`MF@CapPipe`) crashes.
- Audit both `System.loadLibrary` (`dexdump`) and `dlopen` strings (`strings`
  for `\.so$` and `/vendor/`) against `proprietary-files.txt`: `TranssionCamera`
  and `libmtkcam_3rdparty.customer.so` load `libanc_hdr.so` (`vendor/etc/anc_siq/*`),
  `libcameracustom.plugin.so` (`libremosaic*`/`librmsclib1.so`), and
  `libcommon.so` (`libXDocProcessSDK.so`) dynamically without `DT_NEEDED`.
- Stock `libsdk_sr.so` has a 4-byte NEON tail over-read (`0x1c8c50: ldr d16, [x8]`)
  that was harmless on Android 11 jemalloc but hits Scudo's guard page on
  Android 15; fixed via `sig_replace` in `extract-files.py`.
- Hybrid `vpud` (stock Android 11 `libvpud_vcodec.so`) + `fire` Android 14
  `libcodec2_mtk_venc.so` breaks `MediaRecorder` (`StagefrightRecorder`) even
  when `screenrecord` and `Aperture` (`MediaMuxer`) succeed: `AP_IPIMSG_ENC_SET_PARAM 15`
  must return `0` (`0x12a2c`: `movs r7, #0`) and `u4TSVC` (`0x11b42`) must be
  zeroed so `VPUD` does not inject a 9-byte `PrefixNALU` rejected by
  `MPEG4Writer::parseAVCCodecSpecificData`.

