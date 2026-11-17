#
# Copyright (C) 2018-2022 ArrowOS
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from those products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)

# Inherit from device makefile
$(call inherit-product, device/infinix/X6812/device.mk)

# Inherit some common crDroid stuff.
$(call inherit-product, vendor/lineage/config/common_full_phone.mk)

# Device identifier. This must come after all inclusions.
PRODUCT_NAME := lineage_X6812
PRODUCT_DEVICE := X6812
PRODUCT_BRAND := infinix
PRODUCT_MODEL := Infinix X6812
PRODUCT_MANUFACTURER := infinix

PRODUCT_GMS_CLIENTID_BASE := android-transsion

PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildDesc="sys_tssi_64_infinix-user 11 RP1A.200720.011 435920 release-keys" \
    BuildFingerprint=Infinix/X6812-GL/Infinix-X6812:11/RP1A.200720.011/230921V810:user/release-keys \
    SystemName=$(PRODUCT_SYSTEM_NAME) \
    DeviceProduct=$(PRODUCT_SYSTEM_NAME)

# Time
LINEAGE_VERSION_APPEND_TIME_OF_DAY := true

$(call inherit-product-if-exists, vendor/gapps/arm64/arm64-vendor.mk)

WITH_GMS := true
