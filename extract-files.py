#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
from extract_utils.file import File
from extract_utils.fixups_blob import (
    BlobFixupCtx,
    blob_fixup,
    blob_fixups_user_type,
)
from extract_utils.fixups_lib import (
    lib_fixup_remove,
    lib_fixups,
    lib_fixups_user_type,
)
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)
from extract_utils.tools import (
    llvm_objdump_path,
)
from extract_utils.utils import (
    run_cmd,
)

namespace_imports = [
    'device/infinix/X6812',
    'hardware/mediatek',
    'hardware/mediatek/libaedv',
    'hardware/mediatek/libmtkperf_client',
]


def lib_fixup_vendor_suffix(lib: str, partition: str, *args, **kwargs):
    return f'{lib}_{partition}' if partition == 'vendor' else None


lib_fixups: lib_fixups_user_type = {
    **lib_fixups,
    (
        'vendor.mediatek.hardware.videotelephony@1.0',
    ): lib_fixup_vendor_suffix,
}

blob_fixups: blob_fixups_user_type = {
    'system/lib/libshowlogo.so': blob_fixup()
        .add_needed('libshim_showlogo.so'),
    'vendor/lib/hw/audio.primary.mt6768.so': blob_fixup()
        .add_needed('libshim_audio.so')
        .replace_needed('libalsautils.so', 'libalsautils-mtk.so'),
    'vendor/lib64/hw/android.hardware.camera.provider@2.6-impl-mediatek.so': blob_fixup()
        .add_needed('libcamera_metadata_shim.so'),
    (
        'vendor/lib64/lib3a.flash.so',
    ): blob_fixup()
        .add_needed('liblog.so'),
    (
        'vendor/lib/libmtkcam_stdutils.so',
        'vendor/lib64/libmtkcam_stdutils.so',
        'vendor/lib/hw/dfps.mt6768.so',
        'vendor/lib64/hw/dfps.mt6768.so',
        'vendor/lib/hw/vendor.mediatek.hardware.pq@2.6-impl.so',
        'vendor/lib64/hw/vendor.mediatek.hardware.pq@2.6-impl.so',
    ): blob_fixup()
        .replace_needed('libutils.so', 'libutils-v30.so'),
    'vendor/lib64/libmtkcam_featurepolicy.so': blob_fixup()
        # evaluateCaptureConfiguration()
        .binary_regex_replace(b'\x34\xE8\x87\x40\xB9', b'\x34\x28\x02\x80\x52'),
    (
        'vendor/lib64/libwvhidl.so',
        'vendor/lib64/mediadrm/libwvdrmengine.so',
    ): blob_fixup()
        .replace_needed('libprotobuf-cpp-lite-3.9.1.so', 'libprotobuf-cpp-full-3.9.1.so')
        .add_needed('libcrypto_shim.so'),
    (
        'vendor/bin/hw/android.hardware.keymaster@4.0-service.beanpod',
        'vendor/lib64/libkeymaster4-v30.so',
        'vendor/lib64/libkeymaster4support-v30.so',
        'vendor/lib64/libkeymaster_messages-v30.so',
        'vendor/lib64/libkeymaster_portable-v30.so',
        'vendor/lib64/libpuresoftkeymasterdevice-v30.so',
    ): blob_fixup()
        .replace_needed('libkeymaster4.so', 'libkeymaster4-v30.so')
        .replace_needed('libkeymaster4support.so', 'libkeymaster4support-v30.so')
        .replace_needed('libkeymaster_messages.so', 'libkeymaster_messages-v30.so')
        .replace_needed('libkeymaster_portable.so', 'libkeymaster_portable-v30.so')
        .replace_needed('libpuresoftkeymasterdevice.so', 'libpuresoftkeymasterdevice-v30.so'),
    'vendor/lib64/libudf.so': blob_fixup()
        .add_needed('libshim_unwindstack.so'),
    (
        'vendor/bin/mnld',
        'vendor/lib/libaalservice.so',
        'vendor/lib64/libaalservice.so',
        'vendor/lib64/libcam.utils.sensorprovider.so',
    ): blob_fixup()
        .add_needed('android.hardware.sensors@1.0-convert-shared.so'),
    'vendor/lib/libmnl.so': blob_fixup()
        .add_needed('libcutils.so'),
    (
        'vendor/bin/hw/android.hardware.neuralnetworks@1.3-service-mtk-neuron',
        'vendor/lib/libnvram.so',
        'vendor/lib64/libnvram.so',
        'vendor/lib/libsysenv.so',
        'vendor/lib64/libsysenv.so',
    ): blob_fixup()
        .add_needed('libbase_shim.so'),
    'vendor/bin/hw/mtkfusionrild': blob_fixup()
        .add_needed('libutils-v32.so'),
    'vendor/lib64/hw/hwcomposer.mt6768.so': blob_fixup()
        .add_needed('libprocessgroup_shim.so'),
    (
        'vendor/lib/libutils-v30.so',
        'vendor/lib64/libutils-v30.so',
    ): blob_fixup()
        .add_needed('libprocessgroup_shim.so'),
    'vendor/lib64/hw/sensors.mt6768.so': blob_fixup()
        .patchelf_version('0_17_2')
        .replace_needed('libutils.so', 'libutils-v30.so')
        .replace_needed('libstagefright_foundation.so', 'libstagefright_foundation-v33.so'),
    'vendor/lib64/libtranssion_bodybeauty.so': blob_fixup()
        .patchelf_version('0_17_2')
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_lockPlanes')
        .clear_symbol_version('AHardwareBuffer_release')
        .clear_symbol_version('AHardwareBuffer_unlock'),
    'vendor/lib64/libsdk_sr.so': blob_fixup()
        .sig_replace('B3 28 00 51 B7 20 00 51', 'B3 40 00 51 B7 40 00 51'),
    'vendor/lib/libvpud_vcodec.so': blob_fixup()
        .sig_replace('01 27 75 E6', '00 27 75 E6')
        .sig_replace('D4 F8 B4 00 C5 F8 34 03', '4F F0 00 00 C5 F8 34 03'),
    'vendor/lib64/hw/android.hardware.graphics.allocator@4.0-impl-mediatek.so': blob_fixup()
        .sig_replace('AD 3D 43 F9 5F 01 00 F9 EA 11 83 D2', 'AD 3D 43 F9 4C 01 00 F9 EA 11 83 D2')
        .sig_replace('3F 03 00 F9 7F 03 00 F9 1F 03 00 F9', '3F 03 00 F9 6B 03 00 F9 1F 03 00 F9'),
    'vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-mediatek.so': blob_fixup()
        .sig_replace('AD 09 44 F9 5F 01 00 F9 EA 11 83 D2', 'AD 09 44 F9 4C 01 00 F9 EA 11 83 D2')
        .sig_replace('3F 03 00 F9 7F 03 00 F9 1F 03 00 F9', '3F 03 00 F9 6B 03 00 F9 1F 03 00 F9'),
    (
        'vendor/lib/hw/android.hardware.graphics.allocator@4.0-impl-mediatek.so',
        'vendor/lib/hw/android.hardware.graphics.mapper@4.0-impl-mediatek.so',
    ): blob_fixup()
        .sig_replace('01 70 15 48 78 44 00 68 C0 E9 00 22 13 48', '01 70 15 48 78 44 00 68 C0 E9 00 12 13 48')
        .sig_replace('01 70 28 68 C0 E9 00 22 40 46', '01 70 28 68 C0 E9 00 12 40 46'),
    (
        'vendor/lib64/libcodec2_soft_mtk_alacdec.so',
        'vendor/lib64/libcodec2_soft_mtk_apedec.so',
        'vendor/lib64/libcodec2_soft_mtk_imaadpcmdec.so',
        'vendor/lib64/libcodec2_soft_mtk_mp3dec.so',
        'vendor/lib64/libcodec2_soft_mtk_msadpcmdec.so',
    ): blob_fixup()
        .replace_needed('libcodec2_soft_common.so', 'libcodec2_soft_common-v31.so')
        .replace_needed('libcodec2_vndk.so', 'libcodec2_vndk-v31.so')
        .replace_needed('libstagefright_foundation.so', 'libstagefright_foundation-v33.so'),
    (
        'vendor/lib64/libcodec2_hidl@1.0-v31.so',
        'vendor/lib64/libcodec2_hidl@1.1-v31.so',
        'vendor/lib64/libcodec2_hidl@1.2-v31.so',
        'vendor/lib64/libcodec2_mtk_vdec.so',
        'vendor/lib64/libcodec2_mtk_venc.so',
        'vendor/lib64/libcodec2_vndk-v31.so',
    ): blob_fixup()
        .patchelf_version('0_17_2')
        .replace_needed('libui-v32.so', 'libui-v30.so'),
    'vendor/lib64/libcodec2_mtk_c2store.so': blob_fixup()
        .sig_replace('6C 69 62 63 6F 64 65 63 32 5F 6D 74 6B 5F 76 64 65 63 2E 73 6F', '6C 69 62 63 6F 64 65 63 32 5F 6D 74 6B 5F 78 64 65 63 2E 73 6F'),
}  # fmt: skip

module = ExtractUtilsModule(
    'X6812',
    'infinix',
    blob_fixups=blob_fixups,
    lib_fixups=lib_fixups,
    namespace_imports=namespace_imports,
    add_firmware_proprietary_file=True,
)

if __name__ == '__main__':
    utils = ExtractUtils.device(module)
    utils.run()
