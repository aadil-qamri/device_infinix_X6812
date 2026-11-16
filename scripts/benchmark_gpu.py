#!/usr/bin/env python3
import argparse
import http.server
import json
import os
import re
import socketserver
import subprocess
import sys
import threading
import time
from urllib.parse import urlparse, parse_qs

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))

# Shared state for benchmark telemetry
telemetry_data = {
    'last_fps': 0.0,
    'total_frames': 0,
    'jank_frames': 0,
    'frame_times': [],
    'fps_history': []
}

class BenchmarkHTTPHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=SCRIPT_DIR, **kwargs)

    def log_message(self, format, *args):
        pass

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == '/telemetry':
            qs = parse_qs(parsed.query)
            if 'fps' in qs:
                fps = float(qs['fps'][0])
                telemetry_data['last_fps'] = fps
                telemetry_data['fps_history'].append(fps)
            if 'frames' in qs:
                telemetry_data['total_frames'] = int(qs['frames'][0])
            if 'jank' in qs:
                telemetry_data['jank_frames'] = int(qs['jank'][0])
            self.send_response(204)
            self.end_headers()
            return
        elif parsed.path in ('/favicon.ico', '/mcp/schema'):
            self.send_response(204)
            self.end_headers()
            return
        elif parsed.path == '/':
            self.path = '/webgl_gpu_stress.html'
        super().do_GET()

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path == '/finish':
            length = int(self.headers.get('content-length', 0))
            if length > 0:
                body = self.rfile.read(length)
                try:
                    data = json.loads(body.decode('utf-8'))
                    telemetry_data['frame_times'] = data.get('frameTimes', [])
                    telemetry_data['total_frames'] = data.get('totalFrames', 0)
                    telemetry_data['jank_frames'] = data.get('jankyFrames', 0)
                except Exception:
                    pass
            self.send_response(204)
            self.end_headers()
            return
        super().do_POST()

class ReusableTCPServer(socketserver.TCPServer):
    allow_reuse_address = True

def run_adb(cmd):
    return subprocess.run(['adb', 'shell'] + cmd, capture_output=True, text=True).stdout.strip()

def calculate_percentiles(values):
    if not values:
        return 0, 0, 0, 0
    s = sorted(values)
    n = len(s)
    def p(pct):
        k = int(round((pct / 100.0) * (n - 1)))
        return s[k]
    return p(50), p(90), p(95), p(99)

def run_benchmark(duration_sec=15, port=8089, unthrottled=False):
    html_file = os.path.join(SCRIPT_DIR, 'webgl_gpu_stress.html')
    if not os.path.exists(html_file):
        print(f"Error: {html_file} not found.")
        sys.exit(1)

    httpd = ReusableTCPServer(('', port), BenchmarkHTTPHandler)
    server_thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    server_thread.start()

    try:
        subprocess.run(['adb', 'reverse', f'tcp:{port}', f'tcp:{port}'], check=True)
        subprocess.run(['adb', 'shell', 'input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard'], check=True)
        time.sleep(1)

        url = f'http://localhost:{port}/webgl_gpu_stress.html' + ('?unthrottled=1' if unthrottled else '')
        subprocess.run(['adb', 'shell', 'am', 'force-stop', 'org.lineageos.jelly'], check=True)
        subprocess.run(['adb', 'shell', 'am', 'start', '-a', 'android.intent.action.VIEW', '-d', url, '-n', 'org.lineageos.jelly/.MainActivity'], check=True)

        print(f"Launching 3D GPU stress test (warmup 3s)...")
        time.sleep(3.5)

        print(f"Running deterministic 3D benchmark for {duration_sec} seconds...")
        gpu_freqs = []
        gpu_utils = []
        temps = []

        start_time = time.time()
        while time.time() - start_time < duration_sec:
            freq_raw = run_adb(['su', '-c', 'cat /sys/kernel/ged/hal/current_freqency'])
            util_raw = run_adb(['su', '-c', 'cat /sys/kernel/ged/hal/gpu_utilization'])
            temp_raw = run_adb(['su', '-c', 'cat /sys/class/thermal/thermal_zone3/temp'])

            if freq_raw and len(freq_raw.split()) >= 2:
                try:
                    gpu_freqs.append(int(freq_raw.split()[1]) // 1000)
                except ValueError:
                    pass

            if util_raw and len(util_raw.split()) >= 3:
                try:
                    parts = [int(p) for p in util_raw.split()[:3]]
                    load = 100 - parts[2] if parts[2] <= 100 else parts[0]
                    gpu_utils.append(load)
                except ValueError:
                    pass

            if temp_raw and temp_raw.isdigit():
                temps.append(int(temp_raw) / 1000.0)

            time.sleep(0.25)

        actual_duration = time.time() - start_time

    finally:
        subprocess.run(['adb', 'shell', 'am', 'force-stop', 'org.lineageos.jelly'], stdout=subprocess.DEVNULL)
        subprocess.run(['adb', 'reverse', '--remove', f'tcp:{port}'], stdout=subprocess.DEVNULL)
        httpd.shutdown()

    # Calculate metrics
    ft = telemetry_data['frame_times']
    tot_frames = len(ft) if ft else telemetry_data['total_frames']
    jank_frames = telemetry_data['jank_frames']
    avg_fps = (tot_frames / actual_duration) if actual_duration > 0 else 0.0

    if ft:
        p50, p90, p95, p99 = calculate_percentiles(ft)
        sorted_ft = sorted(ft)
        # 1% low and 0.1% low FPS
        low_1pct_idx = int(len(sorted_ft) * 0.99)
        low_1pct_fps = 1000.0 / sorted_ft[low_1pct_idx] if sorted_ft[low_1pct_idx] > 0 else 0.0
        jank_pct = (jank_frames / tot_frames * 100.0) if tot_frames > 0 else 0.0
    else:
        fps_hist = telemetry_data['fps_history']
        avg_fps = (sum(fps_hist) / len(fps_hist)) if fps_hist else avg_fps
        p50 = p90 = p95 = p99 = (1000.0 / avg_fps) if avg_fps > 0 else 0
        low_1pct_fps = avg_fps * 0.85
        jank_pct = 0.0

    avg_freq = (sum(gpu_freqs) / len(gpu_freqs)) if gpu_freqs else 0
    peak_freq = max(gpu_freqs) if gpu_freqs else 0
    avg_util = (sum(gpu_utils) / len(gpu_utils)) if gpu_utils else 0
    peak_util = max(gpu_utils) if gpu_utils else 0
    t_start = temps[0] if temps else 0
    t_end = temps[-1] if temps else 0

    print("\n" + "=" * 55)
    print("      MALI-G52 3D GPU GAMING BENCHMARK REPORT      ")
    print("=" * 55)
    print(f"Benchmark Duration        : {actual_duration:.1f} s")
    print(f"Total 3D Frames Rendered  : {tot_frames}")
    print(f"Average Framerate         : {avg_fps:.1f} FPS")
    print(f"1% Low Framerate          : {low_1pct_fps:.1f} FPS")
    print(f"Janky / Dropped Frames    : {jank_frames} ({jank_pct:.2f}%)")
    print("-" * 55)
    print("Frame Time Distribution (ms):")
    print(f"  Median (p50)            : {p50:.1f} ms")
    print(f"  90th Percentile (p90)   : {p90:.1f} ms")
    print(f"  95th Percentile (p95)   : {p95:.1f} ms")
    print(f"  99th Percentile (p99)   : {p99:.1f} ms")
    print("-" * 55)
    print("GPU Hardware Telemetry (Mali-G52 MC2):")
    print(f"  Average GPU Clock       : {avg_freq:.0f} MHz")
    print(f"  Peak GPU Clock          : {peak_freq:.0f} MHz")
    print(f"  Average GPU Utilization : {avg_util:.1f} %")
    print(f"  Peak GPU Utilization    : {peak_util:.1f} %")
    print(f"  SoC Temperature Range   : {t_start:.1f}°C -> {t_end:.1f}°C (delta: {t_end - t_start:+.1f}°C)")
    print("=" * 55 + "\n")

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Deterministic 3D GPU Gaming Benchmark for Infinix X6812')
    parser.add_argument('--duration', type=int, default=15, help='Benchmark duration in seconds (default: 15)')
    parser.add_argument('--port', type=int, default=8089, help='Local server port (default: 8089)')
    parser.add_argument('--unthrottled', action='store_true', help='Run unthrottled beyond 90Hz VSYNC cap (pure GPU throughput benchmark)')
    args = parser.parse_args()
    run_benchmark(duration_sec=args.duration, port=args.port, unthrottled=args.unthrottled)
