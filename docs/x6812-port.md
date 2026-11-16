## X6812 port checklist

This tree was forked from the X6812B (NFC) tree. Everything below needs the X6812
stock dump or the device, and was deliberately not guessed.

### Blobs

- Run `./extract-files.py <path to the X6812 stock dump>` to generate
  `vendor/infinix/X6812`.
- Create `x6812.txt` from the dump in the format the old `x6812b.txt` used
  (relative paths, one per line). `compare_blobs.py` reads it as the source of
  truth and defaults to that name.

### Kernel, DTB, DTBO

- Build `device/infinix/X6812-kernel` from the X6812 boot image and dtbo image.
  The DTB may differ from the X6812B one.
- Add `BOARD_PREBUILT_DTBOIMAGE` and `dtbo` in `AB_OTA_PARTITIONS` together. Adding
  `dtbo` to the OTA list without a prebuilt image breaks OTA package generation.

### Strings taken from X6812B stock

- `TARGET_BOOTLOADER_BOARD_NAME` in `BoardConfig.mk` and `ro.product.board` in
  `configs/props/system.prop` were renamed mechanically.
- `BuildFingerprint` in `lineage_X6812.mk` and the header of
  `proprietary-firmware.txt` still carry the X6812B stock fingerprint.
- `BuildDesc` in `lineage_X6812.mk` has not been compared with the X6812 stock build.

### Diff against X6812 stock

- `configs/manifests/manifest.xml` against `vendor/etc/vintf/manifest.xml`.
- `rootdir/etc/fstab.mt6768` and the `init.*.rc` files against the stock ones.
- The `by-name` partition list and super partition size.
- `configs/sensors/hals.conf`. The sensor chips named in
  `playbooks/subsystems.md` are X6812B facts.

### Features

- Fingerprint: `sepolicy/vendor/file_contexts` and `hal_fingerprint_default.te`
  exist, but no fingerprint HAL, blobs or manifest entries are wired.
- Check whether the X6812 DTB still declares an NFC node.
