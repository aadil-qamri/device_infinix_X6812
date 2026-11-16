## Debugging order

When you hit an error, work in this order. Do not jump to step 5.

1. **Search for an existing fix.** `gh search commits '<error text>'` or the
   GitHub MCP tools. Most MTK/Lineage problems are already solved somewhere.
2. **Read the AOSP source.** Find the exact framework/HAL code, struct layout,
   and log tag that produced the message.
3. **Compare with a sibling device tree.** e.g.
   `mt6768-dev/android_device_xiaomi_mt6768-common`.
4. **Reverse-engineer stock.** Check the stock dump's props, init `.rc`,
   audio/media XMLs, and binaries.
5. **Test live on the device.** Only now start changing things.

The device is ext4 + rw, so you can test hypotheses directly (push a file, edit
a sysfs node, `setprop`) and confirm before committing. If the partition were
erofs, do not force a remount.

## Build error triage

`out/error.log` is **overwritten** by every attempt. Always compare its mtime
against the mtimes of files you changed before trusting it.

### Which stage failed

Two soong stages fail with very different messages — identify which you have:

| Message | Stage | Meaning |
|:--|:--|:--|
| `analyzing Android.bp files and generating ninja` | soong/blueprint | missing src file, bad module, AIDL version clash |
| `writing legacy Make module rules` / `overriding commands for target` | Make install | two modules install to the same destination |

For duplicate installs, grep the target in
`out/soong/installs-lineage_X6812.mk` — it lists every rule, so you can see
both sources.

### Known traps

- **A stale output file does not mean the current config builds.** A
  `build.lineage_X6812.ninja` from an earlier successful run will still be on
  disk after a later failure. Check timestamps.
- **`stem:` renames only the installed file, not the module.** Two modules can
  therefore write one destination. Prebuilt blobs whose `DT_NEEDED` is baked in
  (e.g. `libui-v32.so` wants `android.hardware.graphics.common-V2-ndk_platform.so`)
  need the installed name to match, but check whether
  `hardware/lineage/compat` already installs that name before adding a `stem`.
- **Never point a module at a source-built AIDL stub when a pure prebuilt
  exists.** compat's `*-ndk_platform` pulls `...-V2-ndk-source`, while
  `libui -> libgralloctypes` pulls `...-V6-ndk-source`; soong then rejects the
  variant with `depends on multiple versions of the same aidl_interface`. Pure
  `cc_prebuilt_library_shared` / `vndk_prebuilt_shared` avoid this entirely.
- **`extract-files.py` must finish before you build.** A
  `libmtknetutils.so does not exist` error was purely a race — the build started
  42 s before extract finished writing 629 blobs. Re-run extract to completion.
- **A prebuilt ELF check failure usually means a SONAME mismatch, not a missing
  dependency.** The checker compares each `DT_NEEDED` against the `DT_SONAME` of
  every declared `shared_libs` entry
  (`build/make/tools/check_elf_file.py`, `check_dt_needed`), *not* against their
  filenames. So `DT_NEEDED "libui-v32.so" is not specified in shared_libs` fires
  even when `libui-v32` **is** declared, because our module points at AOSP's
  `libui.so` whose SONAME is `libui.so`.

  The SONAME cannot be corrected from this tree: `prebuilts/vndk` is AOSP-owned
  and `cc_prebuilt_library_shared` has no `soname` property. The sanctioned fix,
  per `device/common/generate-android-bp-for-blobs.sh:37`, is
  `check_elf_files: false` for that module. Apply it **per blob** with
  `;DISABLE_CHECKELF` in `proprietary-files.txt` — never by restoring
  `BUILD_BROKEN_PREBUILT_ELF_FILES`, which disables the check for all ~543
  prebuilts and hides real problems.

  Before treating a failure as a SONAME mismatch, confirm the dependency is
  actually installed (see the vendor-tree scan under
  [prebuilt ELF dependencies](#prebuilt-elf-dependencies)). A genuinely missing
  library needs a blob, not a check bypass.

## SELinux

Validate changes with the standalone loop in
[Validate without a full build](#validate-without-a-full-build) — it catches both
unknown types and `neverallow` violations in about a minute.

### Rules that bite

- **`init_daemon_domain(X)` expands to `domain_auto_trans(init, X_exec, X)`**, so
  it references a type named `X_exec`. Never delete a `*_exec` type because a
  literal grep finds no other user — grep for the macro argument instead. Same
  trap applies to `add_hwservice`, `binder_use`, `r_dir_file`.
- **An "unused type" scan based on literal text is unsound.** Let `checkpolicy`
  prove it.
- **Vendor policy cannot reference types declared under
  `device/mediatek/sepolicy_vndr/base/private/`** (that is platform policy).
  e.g. `proc_cl_cam_status` lives there, so a vendor `.te` fails with
  `unknown type`.
- **`vendor_default_prop` is under `treble_sysprop_neverallow`**
  (`system/sepolicy/private/property.te:158`). Granting it to a vendor domain
  compiles but fails neverallow.
- **Versioned app domains are siblings, not children.** `untrusted_app` and
  `untrusted_app_29` are separate types, so `get_prop(untrusted_app, ...)` does
  **not** cover an app with `targetSdkVersion=29`. Use `untrusted_app_all`, which
  `untrusted_app_domain()` populates for every version.
- **Test every `genfscon` / `file_contexts` path on the device first** with
  `ls -Z <path>`. A wrong path never matches and fails silently: our `OTG_CTL`
  entry pointed at the mt6781 `odm/odm:tran_battery` path, so the node stayed
  labelled generic `sysfs` and system_server's grant could never apply.
- **The shared `device/mediatek/sepolicy_vndr` already covers most MTK nodes**
  (charger, batteryinfo, wakeup, extcon, vibrator, block queues). Check there
  before adding an entry.
- **`camerahalserver` runs as `mtk_hal_camera`**, not `hal_camera_default`, and
  `venc_srv` runs as `vpud_native`; put `default_prop` `dontaudit` rules on
  those domains.
- **Dropping `;PRESIGNED` from a vendor/system_ext APK changes its SELinux
  domain** from `untrusted_app_*` to `platform_app` (`certificate: "platform"`),
  so verify `sepolicy/vendor/platform_app.te` has the app's camera/sysfs grants.

## Proprietary blobs

`proprietary-files.txt` and `extract-files.py` are coupled — read both together.

- **Pinning is driven by the `|hash` suffix, not the `- from` comment.**
  `extract_utils` treats any file with a hash as pinned and restores it from the
  vendor tree instead of re-extracting.
- **A pinned file that has a blob fixup needs TWO hashes: `source|fixed-up`.**
  A single hash holding the post-fixup value always mismatches, so the pin
  silently never applies and the log says `fixed up` instead of
  `restored pinned file ...`.
- **Do not put ` - from` in a section header** unless you want
  `update-sha1sums.py` to hash everything beneath it — it keys off that exact
  substring and will also collapse `source|fixup` pairs into one hash.
- **Header attributions must be true.** This file once claimed AEE, Widevine,
  Graphics Hardware and WiFi came from Redmi/realme firmware; every one of those
  blobs exists in the stock X6812B dump and differs only by fixups. Only
  `# Media (Codec2)` is external
  (`mt6768-dev/android_device_xiaomi_mt6768-common`).
- **`restored pinned file with hash X has fixup hash X` in RED is not an error.**
  It is a request to add the second hash and returns `MATCH`. Only `BAD_FIXUP`
  is a real failure.
- **`apktool_patch()` requires at least one pre-existing modified file** in
  the patch: `fixups_blob.py` runs `git add` on existing affected paths and
  `git commit`, which fails with `nothing to commit` on new-file-only patches.

## Dead-config sweeps

Before removing a `.rc` line, sepolicy path, or blob:

1. **Diff against stock first.** Anything identical to stock is not our bug;
   deleting it only adds divergence and noisier future diffs. Only paths
   introduced *by this tree* are suspects.
2. **Test every path on the device**, respecting the fstype —
   `genfscon sysfs` → `/sys`, `genfscon proc` → `/proc`:
   ```bash
   for p in /sys/... /proc/...; do [ -e "$p" ] || echo "MISS $p"; done
   ```
3. **`ueventd` rules for absent partitions are not dead** — ueventd skips them
   (`md1dsp`, `secro`, `misc2`).
4. **`disabled` services are not dead** — check for `start X` triggers first.
   `stp_dump` was only removable after confirming
   `persist.vendor.connsys.coredump.mode` is 0.
5. **Stock vendor `.rc` files still list services whose binaries we do not
   extract** (`bt_dump`, `wifi_dump` in `init_connectivity.rc`). init tolerates
   this; do not patch the rc.


## Commits

- Commit message structure:
  1. Subject line: `<codename>: <summary>` (e.g. `X6812: wifi: enable WPA3-SAE softap support`).
  2. Log snippet: verbatim error, logcat trace, or failing command output showing the bug/repro.
  3. Explanation: root cause, what was verified on hardware vs assumed, and why the chosen fix is correct.
- No explanatory comments in code/config files; put all reasoning in the commit message.

## Maintaining this file

Update this file whenever you fix something non-obvious: how to reproduce, how
to diagnose, what you tried that failed, and any mistake you made. Prefer adding
to an existing section over creating a new one. Keep entries actionable — a
command or a concrete check beats a description.
