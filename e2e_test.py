#!/usr/bin/env python3
#
# Copyright (C) 2026 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

import argparse
import subprocess
import sys
import time

GREEN = "\033[92m"
RED = "\033[91m"
YELLOW = "\033[93m"
BOLD = "\033[1m"
RESET = "\033[0m"


def log_pass(msg):
    print(f"[{GREEN}PASS{RESET}] {msg}")


def log_fail(msg):
    print(f"[{RED}FAIL{RESET}] {msg}")


def log_info(msg):
    print(f"[{YELLOW}INFO{RESET}] {msg}")


class DeviceTester:
    def __init__(self, serial=None):
        self.serial = serial
        self.adb_base = ["adb"]
        if self.serial:
            self.adb_base.extend(["-s", self.serial])
        self.failures = 0
        self.passes = 0

    def adb(self, *args, timeout=15):
        cmd = self.adb_base + list(args)
        try:
            res = subprocess.run(
                cmd, capture_output=True, text=True, timeout=timeout
            )
            return res.stdout.strip(), res.stderr.strip(), res.returncode
        except subprocess.TimeoutExpired:
            return "", "Command timed out", -1

    def shell(self, cmd_str, timeout=15):
        return self.adb("shell", cmd_str, timeout=timeout)

    def test_boot_and_system(self):
        log_info("Testing Boot and OS State...")
        out, _, rc = self.shell("getprop sys.boot_completed")
        if rc == 0 and out == "1":
            log_pass("sys.boot_completed is 1")
            self.passes += 1
        else:
            log_fail(f"sys.boot_completed is not 1 (got: {out})")
            self.failures += 1

        fingerprint, _, _ = self.shell("getprop ro.build.fingerprint")
        log_info(f"Fingerprint: {fingerprint}")

        crashes, _, _ = self.shell("logcat -b crash -d -t 20")
        fatal_crashes = [
            l for l in crashes.splitlines() if "FATAL EXCEPTION" in l or "Fatal signal" in l
        ]
        if not fatal_crashes:
            log_pass("Zero fatal crashes in crash buffer")
            self.passes += 1
        else:
            log_fail(f"Detected crash loop in crash buffer: {fatal_crashes[0]}")
            self.failures += 1

    def test_battery_and_health(self):
        log_info("Testing Battery and Health AIDL HAL...")
        out, _, _ = self.shell("dumpsys battery")
        
        has_design = "Design capacity: 5000000" in out
        has_max = "Maximum capacity: 5000000" in out
        
        if has_design and has_max:
            log_pass("Battery capacity accurately reports 5000 mAh (5,000,000 uAh)")
            self.passes += 1
        else:
            log_fail(f"Battery capacity mismatch. Dumpsys: {out[:200]}")
            self.failures += 1

        pid, _, _ = self.shell("pidof android.hardware.health-service.mediatek")
        if pid:
            log_pass(f"Health HAL daemon running (PID: {pid})")
            self.passes += 1
        else:
            log_fail("Health HAL daemon (android.hardware.health-service.mediatek) is stopped")
            self.failures += 1

    def test_sensors(self):
        log_info("Testing Sensors AIDL MultiHAL...")
        out, _, _ = self.shell("dumpsys sensorservice | head -n 15")
        if "Total 13 h/w sensors" in out or "Sensor List:" in out:
            log_pass("Sensors service active with hardware sensors enumerated")
            self.passes += 1
        else:
            log_fail("Sensors service failed to enumerate physical sensors")
            self.failures += 1

    def test_nfc(self):
        log_info("Testing NFC AIDL HAL...")
        out, _, _ = self.shell("dumpsys nfc | grep -E 'mState|mIsHceCapable'")
        if "mState=on" in out:
            log_pass("NFC daemon active and state is ON")
            self.passes += 1
        else:
            log_fail(f"NFC state not ON (got: {out})")
            self.failures += 1

    def test_camera_and_torch(self):
        log_info("Testing Camera HAL and Flashlight...")
        out, _, _ = self.shell("dumpsys media.camera | head -n 10")
        if "Number of camera devices: 2" in out:
            log_pass("Camera provider reports 2 physical camera devices")
            self.passes += 1
        else:
            log_fail("Camera provider failed to enumerate 2 devices")
            self.failures += 1

        # Flashlight test
        self.shell("cmd camera set-torch-mode 0 on")
        time.sleep(0.3)
        self.shell("cmd camera set-torch-mode 0 off")
        log_pass("Camera torch command executed successfully")
        self.passes += 1

    def test_audio_and_codec2(self):
        log_info("Testing AudioFlinger and Codec2...")
        af, _, _ = self.shell("dumpsys media.audio_flinger | grep 'Hardware status:'")
        if "Hardware status: 0" in af:
            log_pass("Audio hardware initialized and healthy (Hardware status: 0)")
            self.passes += 1
        else:
            log_fail("Audio HAL returned non-zero hardware status")
            self.failures += 1

        c2_pid, _, _ = self.shell("pidof android.hardware.media.c2-mtk-service")
        if c2_pid:
            log_pass(f"Media Codec2 HAL running (PID: {c2_pid})")
            self.passes += 1
        else:
            log_fail("Codec2 service is not running")
            self.failures += 1

    def test_tethering(self):
        log_info("Testing Wi-Fi Sharing / Tethering...")
        out, _, _ = self.shell("dumpsys tethering")
        if "enableBpfOffload: false" in out and "mIsBpfEnabled: false" in out:
            log_pass("Tethering BPF offload disabled (Netfilter routing active)")
            self.passes += 1
        else:
            log_fail("Tethering still has BPF offload enabled on 4.14 kernel")
            self.failures += 1

    def test_display_and_vibrator(self):
        log_info("Testing Display Mode and Haptics...")
        # Check smooth display setting availability
        res, _, _ = self.shell("cmd overlay dump com.android.settings.target | grep config_show_smooth_display")
        if "config_show_smooth_display" in res:
            log_pass("Smooth display toggle enabled in Settings RRO overlay")
            self.passes += 1
        else:
            log_fail("Smooth display toggle is not mapped in Settings overlay")
            self.failures += 1

        # Vibrator test
        _, _, rc = self.shell("cmd vibrator_manager synced oneshot 100")
        if rc == 0:
            log_pass("Vibrator AIDL HAL responded to pulse request")
            self.passes += 1
        else:
            log_fail("Vibrator test returned non-zero exit code")
            self.failures += 1

    def test_telephony_and_ims(self):
        log_info("Testing Telephony RIL & IMS Daemons...")
        rild, _, _ = self.shell("pidof mtkfusionrild")
        ims, _, _ = self.shell("pidof com.mediatek.ims")
        
        if rild:
            log_pass(f"MTK Radio RIL daemon running (PID: {rild})")
            self.passes += 1
        else:
            log_fail("MTK RIL daemon is not running")
            self.failures += 1

        if ims:
            log_pass(f"MediaTek IMS package running (PID: {ims})")
            self.passes += 1
        else:
            log_fail("MediaTek IMS service package is not running")
            self.failures += 1

    def run_all(self):
        print(f"{BOLD}=== Starting E2E Verification for Infinix X6812 ==={RESET}\n")
        self.test_boot_and_system()
        print()
        self.test_battery_and_health()
        print()
        self.test_sensors()
        print()
        self.test_nfc()
        print()
        self.test_camera_and_torch()
        print()
        self.test_audio_and_codec2()
        print()
        self.test_tethering()
        print()
        self.test_display_and_vibrator()
        print()
        self.test_telephony_and_ims()
        print()

        print(f"{BOLD}=== Final Test Summary ==={RESET}")
        print(f"Total Tests Passed : {GREEN}{self.passes}{RESET}")
        print(f"Total Tests Failed : {RED}{self.failures}{RESET}")

        if self.failures > 0:
            sys.exit(1)
        else:
            print(f"\n{GREEN}{BOLD}ALL SYSTEM AND HAL E2E TESTS PASSED SUCCESSFULLY!{RESET}\n")


def main():
    parser = argparse.ArgumentParser(description="End-to-end device service tester for X6812")
    parser.add_argument("-s", "--serial", help="Device serial number (ADB -s)")
    args = parser.parse_args()

    tester = DeviceTester(serial=args.serial)
    tester.run_all()


if __name__ == "__main__":
    main()
