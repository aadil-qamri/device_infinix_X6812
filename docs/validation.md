## Validate without a full build

Most build failures come from host tools that are already built. `checkpolicy`,
`check_elf_file` and `build_sepolicy` all live in `out/host/linux-x86/bin/`, so
you can reproduce a failure in seconds and confirm a fix before asking for a
build — which matters because you are not allowed to build.

### General recipe

Every failing ninja rule is written verbatim into `out/error.log` on a
`Command:` line. Most of these commands are directly re-runnable. Extract it
with Python (not `sed`, the line contains quotes, `&&` chains and escaped
newlines):

```bash
python3 - <<'PY'
import re
log = open('out/error.log').read()
m = re.search(r'^Command: /bin/bash -c "(.*)"\s*$', log, re.M)
print(' '.join(m.group(1).split()))
PY
```

Run the result and you get the identical error text without a build. Then, after
changing the tree, re-run the same command to prove the fix. Note that for
soong-generated inputs you must regenerate them first — see below.

### sepolicy

`scripts/check-sepolicy.sh` does the whole thing: it locates `$ANDROID_ROOT`, finds
the `m4` rule for both `vendor_sepolicy.conf` and
`sepolicy_neverallows.checkpolicy.conf` in the split ninja files, runs them, and
runs `checkpolicy` on each. The generated rules land in `out/` (not `/tmp`), so
they survive reboots; the script regenerates them every run, which is what keeps
them from going stale after a `.te` edit.

```bash
scripts/check-sepolicy.sh
```

But **unknown types alone will not catch `neverallow` violations** — that is why
both configs are compiled. `neverallows` has no `<device>` subdirectory.

Gotchas:

- `checkpolicy` prints `ERROR 'unknown type X'` but **still exits 0**. The script
  therefore treats an empty output file as failure and never trusts the exit code.
  If you run `checkpolicy` by hand, check `-s`/`-f` on the `.cil` yourself.
- The rule block moves between `build.lineage_X6812.N.ninja` splits across
  builds. The script globs all of them rather than hardcoding `.9.ninja`.
- A missing rule means `out/` is stale (no successful configure since the module
  was last touched). The script says so instead of silently passing.

### VINTF manifest and matrix (checkvintf / assemble_vintf)

Validate device manifest changes, fragment conflicts (duplicate FqInstances),
and framework compatibility matrix (FCM) requirements without a build:

```bash
# 1. Re-assemble vendor manifest and update staging target
BOARD_SEPOLICY_VERS="202404" PRODUCT_ENFORCE_VINTF_MANIFEST=true \
  ../../../out/host/linux-x86/bin/assemble_vintf \
  -i configs/manifests/manifest.xml \
  -o ../../../out/soong/.intermediates/system/libhidl/vintfdata/vendor_manifest.xml/android_arm64_armv8-2a-dotprod_cortex-a75/manifest.xml

cp -f -d ../../../out/soong/.intermediates/system/libhidl/vintfdata/vendor_manifest.xml/android_arm64_armv8-2a-dotprod_cortex-a75/manifest.xml \
  ../../../out/target/product/X6812/vendor/etc/vintf/manifest.xml

# 2. Check vendor manifest against fragments in /vendor/etc/vintf/manifest/
../../../out/host/linux-x86/bin/checkvintf --check-one \
  --dirmap /vendor:../../../out/target/product/X6812/vendor \
  --dirmap /apex:../../../out/target/product/X6812/obj/PACKAGING/check_vintf_all_intermediates/apex \
  --property ro.boot.product.vendor.sku=

# 3. Check full framework/device compatibility matrix
../../../out/host/linux-x86/bin/checkvintf --check-compat \
  --dirmap /system:../../../out/target/product/X6812/system \
  --dirmap /vendor:../../../out/target/product/X6812/vendor \
  --dirmap /odm:../../../out/target/product/X6812/vendor/odm \
  --dirmap /product:../../../out/target/product/X6812/product \
  --dirmap /system_ext:../../../out/target/product/X6812/system_ext \
  --dirmap /apex:../../../out/target/product/X6812/obj/PACKAGING/check_vintf_all_intermediates/apex \
  --property ro.product.first_api_level=30 \
  --property ro.boot.product.hardware.sku= \
  --property ro.boot.product.vendor.sku=
```

Step 2 catches conflicting FqInstances between `manifest.xml` and installed HAL
fragments. Step 3 prints `COMPATIBLE` on success.

### SELinux contexts (checkfc, check_prop_prefix)

Validate context files against `precompiled_sepolicy` to catch invalid regex,
undeclared types, or missing labels without building:

```bash
POLICY="../../../out/target/product/X6812/vendor/etc/selinux/precompiled_sepolicy"

# File contexts
../../../out/host/linux-x86/bin/checkfc "$POLICY" sepolicy/vendor/file_contexts

# Property contexts syntax & types
../../../out/host/linux-x86/bin/checkfc -p "$POLICY" sepolicy/vendor/property_contexts

# HW service contexts (HIDL)
../../../out/host/linux-x86/bin/checkfc -l "$POLICY" sepolicy/vendor/hwservice_contexts

# Service / vndservice contexts (AIDL / Binder)
../../../out/host/linux-x86/bin/checkfc -s "$POLICY" sepolicy/vendor/service_contexts
../../../out/host/linux-x86/bin/checkfc -v "$POLICY" sepolicy/vendor/vndservice_contexts

# Verify vendor property Treble prefix compliance
../../../out/host/linux-x86/bin/check_prop_prefix \
  --property-contexts sepolicy/vendor/property_contexts \
  --allowed-property-prefix vendor. ro.vendor. persist.vendor. \
  --allowed-context-prefix vendor_ \
  --strict
```

### Init scripts (host_init_verifier)

Catch syntax errors, unrecognized options, illegal trigger lines, and undefined
UID/GID permissions in `.rc` files (note: `ueventd*.rc` uses ueventd format, not init):

```bash
PASSWD="../../../system/core/libcutils/include/private/android_filesystem_config.h"
for rc in rootdir/etc/init/hw/*.rc rootdir/etc/init.recovery.mt6768.rc; do
  [ -f "$rc" ] && ../../../out/host/linux-x86/bin/host_init_verifier -p "$PASSWD" "$rc"
done
```

### XML syntax and schemas (xmllint)

Check XML well-formedness and validate configs against AOSP XSD schemas:

```bash
# Syntax check across configs
../../../out/host/linux-x86/bin/xmllint --noout \
  configs/audio/audio_policy_configuration.xml \
  configs/media/media_codecs.xml \
  configs/manifests/manifest.xml

# Schema validation for media codecs
../../../out/host/linux-x86/bin/xmllint --noout \
  --schema ../../../frameworks/av/media/libstagefright/xmlparser/media_codecs.xsd \
  configs/media/media_codecs.xml
```

### prebuilt ELF dependencies

`check_elf_file` validates that every `DT_NEEDED` of a prebuilt resolves to a
declared `shared_libs` entry. The command in `error.log` is directly re-runnable:

```bash
python3 - <<'PY'
import re
log = open('out/error.log').read()
m = re.search(r'check_elf_file (.*?) \) && \(touch', log, re.S)
open('/tmp/opencode/elfcheck.sh','w').write(
    'out/host/linux-x86/bin/check_elf_file ' + ' '.join(m.group(1).split()) + '\n')
PY
bash /tmp/opencode/elfcheck.sh     # exit 0 = pass, 2 = fail
```

It uses the already-built dependency `.so` files under
`out/target/product/X6812/obj/SHARED_LIBRARIES/`, so it is valid as long as
those exist from a previous build.

Useful standalone check across the whole vendor tree — does any `DT_NEEDED` name
lack an installed file? Compare against `out/soong/installs-lineage_X6812.mk`,
which is the authoritative list of what gets installed:

```bash
python3 - <<'PY'
import re, subprocess, glob, os
inst=open('out/soong/installs-lineage_X6812.mk', errors='replace').read()
known={os.path.basename(m) for m in
       re.findall(r'out/target/product/X6812/[^\s:]*lib64/([A-Za-z0-9@._+-]+\.so):', inst)}
vdir='vendor/infinix/X6812/proprietary/vendor/lib64'
known |= {os.path.basename(f) for f in glob.glob(vdir+'/*.so')}
need={}
for f in glob.glob(vdir+'/*.so'):
    out=subprocess.run(['readelf','-d',f],capture_output=True,text=True).stdout
    for n in re.findall(r'Shared library: \[([^\]]+)\]', out):
        need.setdefault(n,set()).add(os.path.basename(f))
for n,w in sorted(need.items()):
    if n not in known: print(n, '<-', sorted(w)[:4])
PY
```

Do **not** compare only within one directory — most dependencies legitimately
come from `/system` or the vendor tree, so a same-directory scan produces a long
list of false positives.
### Standalone vendor tree SONAME audit

Validate all `.so` binaries in `vendor/infinix/X6812/` for `DT_SONAME != filename`
mismatches without a build (which trigger Soong `check_elf_files` failures):

```bash
python3 - <<'PY'
import os, glob, subprocess

vendor_dir = "vendor/infinix/X6812/proprietary/vendor"
for f in glob.glob(f"{vendor_dir}/**/*.so", recursive=True):
    fname = os.path.basename(f)
    out = subprocess.run(["readelf", "-d", f], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if "SONAME" in line:
            soname = line.split("[")[1].split("]")[0]
            if soname != fname:
                print(f"SONAME mismatch: {fname} (SONAME: {soname}) in {f}")
PY
```

When a mismatch is identified on a prebuilt that is dynamically loaded via `dlopen`
(e.g. `libmmc5603_x6812.so` having `DT_SONAME "libmemsic.so"`), append `;DISABLE_CHECKELF`
to its line in `proprietary-files.txt` and re-run `./extract-files.py`.


### Test compile without Soong

Individual components can be test-compiled in seconds without waiting for a
full Soong build:

#### 1. Overlays (`aapt2 compile`)
```bash
python3 - <<'PY'
import glob, subprocess, os, tempfile
aapt2 = '../../../out/host/linux-x86/bin/aapt2'
with tempfile.TemporaryDirectory() as tmpdir:
    for ov in sorted(glob.glob('overlay/*')):
        xmls = glob.glob(f'{ov}/res/**/*.xml', recursive=True)
        if xmls:
            res = subprocess.run([aapt2, 'compile', '-o', tmpdir, '--pseudo-localize'] + xmls,
                                 cwd='../../..', capture_output=True, text=True)
            print(f"{ov}: {'OK' if res.returncode == 0 else 'FAIL: ' + res.stderr}")
PY
```

#### 2. C++ Shims (`clang++`)
```bash
prebuilts/clang/host/linux-x86/clang-r536225/bin/clang++ -c \
    device/infinix/X6812/libshims/libshim_unwindstack.cpp -o /tmp/shim.o \
    -target aarch64-linux-android10000 \
    -Isystem/unwindstack/include -Isystem/core/include -Isystem/libbase/include \
    -std=c++20 -fPIC -Wall -Werror && rm -f /tmp/shim.o
```

#### 3. Blueprint formatting (`bpfmt`)
```bash
# Check formatting without writing (exit 0 if clean)
find . -name Android.bp -exec ../../../prebuilts/build-tools/linux-x86/bin/bpfmt -d {} +

# Auto-format in place
find . -name Android.bp -exec ../../../prebuilts/build-tools/linux-x86/bin/bpfmt -w {} +
```

### Live-test RRO overlays on device

Test resource and overlay changes live on device without rebuilding:

```bash
python3 - <<'PY'
import os, subprocess, tempfile, glob

android_root = os.path.abspath('../../..')
aapt2 = os.path.join(android_root, 'out/host/linux-x86/bin/aapt2')
zipalign = os.path.join(android_root, 'out/host/linux-x86/bin/zipalign')
apksigner = os.path.join(android_root, 'out/host/linux-x86/bin/apksigner')
framework_res = os.path.join(android_root, 'out/target/product/X6812/system/framework/framework-res.apk')

# 1. Target overlay directory (e.g. overlay/SettingsResTarget or overlay/FrameworksResTarget)
overlay_dir = os.path.abspath('overlay/FrameworksResTarget')
manifest = os.path.join(overlay_dir, 'AndroidManifest.xml')
res_xmls = glob.glob(f"{overlay_dir}/res/**/*.xml", recursive=True)

pem = os.path.join(android_root, 'vendor/lineage-priv/keys/releasekey.x509.pem')
pk8 = os.path.join(android_root, 'vendor/lineage-priv/keys/releasekey.pk8')

with tempfile.TemporaryDirectory() as tmpdir:
    apk_unaligned = os.path.join(tmpdir, 'unaligned.apk')
    apk_aligned = os.path.join(tmpdir, 'aligned.apk')
    out_apk = '/tmp/test_overlay.apk'

    subprocess.run([aapt2, 'compile', '-o', tmpdir, '--pseudo-localize'] + res_xmls,
                   check=True, cwd=android_root)
    flats = glob.glob(f"{tmpdir}/*.flat")
    # Link against framework-res.apk for internal framework attrs/resources;
    # for APEX targets (WifiResTarget, TetheringConfigTarget), also pass
    # -I /tmp/ServiceWifiResources.apk or /tmp/TetheringNext.apk pulled from /apex/.
    subprocess.run([aapt2, 'link', '-o', apk_unaligned, '-I', framework_res,
                    '--manifest', manifest, '--min-sdk-version', '35',
                    '--target-sdk-version', '35', '--keep-raw-values'] + flats,
                   check=True, cwd=android_root)
    subprocess.run([zipalign, '-f', '4', apk_unaligned, apk_aligned], check=True, cwd=android_root)
    subprocess.run([apksigner, 'sign', '--key', pk8, '--cert', pem, '--out', out_apk, apk_aligned],
                   check=True, cwd=android_root)
    print("Built signed overlay at /tmp/test_overlay.apk")
PY

# 2. When /vendor is mounted ro (no remount), push to /data/local/tmp and bind-mount
DEST="/vendor/overlay/FrameworksResTarget.apk"
adb push /tmp/test_overlay.apk /data/local/tmp/test_overlay.apk
adb shell "su -c 'chmod 644 /data/local/tmp/test_overlay.apk && chcon u:object_r:vendor_overlay_file:s0 /data/local/tmp/test_overlay.apk && mount -o bind /data/local/tmp/test_overlay.apk $DEST && stop && start'"

# To revert a bind-mounted overlay cleanly (plain umount fails with EBUSY while system_server holds it):
adb shell "su -c 'umount -l $DEST && rm -f /data/local/tmp/test_overlay.apk && stop && start'"

# 3. Verify idmap on device
adb shell "cmd overlay dump android.overlay.target"
```

- **Live-test `ro.*` props without a full reboot**: use APatch's
  `/data/adb/ap/bin/resetprop` (e.g. `resetprop -d <prop>` or `resetprop <prop> <val>`),
  then restart the consuming native daemon plus Zygote (`stop surfaceflinger && start surfaceflinger && stop && start`).

### Compile and test individual binaries with Ninja (without a full build)

When developing or testing a HAL binary (e.g. `android.hardware.health-service.mediatek`),
you can compile ONLY that binary target using the prebuilt ninja without invoking a full ROM build:

```bash
# 1. Compile the single binary target
../../../prebuilts/build-tools/linux-x86/bin/ninja -f ../../../out/combined-lineage_X6812.ninja \
  out/soong/.intermediates/hardware/mediatek/aidl/health/android.hardware.health-service.mediatek/android_vendor_arm64_armv8-2a-dotprod_cortex-a75/android.hardware.health-service.mediatek

# 2. Push to live device and set permissions/SELinux context
BIN="../../../out/soong/.intermediates/hardware/mediatek/aidl/health/android.hardware.health-service.mediatek/android_vendor_arm64_armv8-2a-dotprod_cortex-a75/android.hardware.health-service.mediatek"
adb push "$BIN" /vendor/bin/hw/android.hardware.health-service.mediatek
adb shell "chmod 755 /vendor/bin/hw/android.hardware.health-service.mediatek && chcon u:object_r:hal_health_default_exec:s0 /vendor/bin/hw/android.hardware.health-service.mediatek"

# 3. Restart the service and inspect live status/dumpsys
adb shell "killall -9 android.hardware.health-service.mediatek; sleep 1.5; pidof android.hardware.health-service.mediatek"
adb shell "dumpsys battery"
```
### What cannot be fixed without a build

`stem`/`module name` collisions, duplicate install rules and AIDL version
clashes are resolved by soong's module graph, not by a standalone tool. For those
you must reason from `Android.bp` and the installs file, then ask for a build.

