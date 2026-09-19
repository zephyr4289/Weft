#!/usr/bin/env python3
# weft_exporter.py — TIER5 §2 Prometheus exporter for Weft metrics (issue #20, task 2).
#
# Zero-dependency (stdlib only): runs the C reference metrics emitter
# (core/c/metrics-dump — built by `make -C core/c metrics-dump`) and serves
# its exposition on :9108/metrics. Cache window (default 2 s) keeps scrape
# pressure off the render box; /scrape forces a refresh.
#
# Works with ANY Weft app that can run the C kernel (native, JNI, FFI):
# point the exporter at a wrapper that renders YOUR app's kernel counters
# in the same exposition format and set WEFT_METRICS_CMD.
#
# Usage:
#   python3 tools/prometheus/weft_exporter.py [--port 9108] [--interval 2]
# Env:
#   WEFT_METRICS_CMD  command producing the exposition (default: core/c/metrics-dump)
#   WEFT_METRICS_HOST label injected into the exposition (default: hostname)

import http.server
import os
import socket
import subprocess
import threading
import time

PORT = int(os.environ.get("WEFT_EXPORTER_PORT", "9108"))
INTERVAL = float(os.environ.get("WEFT_EXPORTER_INTERVAL", "2"))
CMD = os.environ.get("WEFT_METRICS_CMD")
if not CMD:
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    CMD = os.path.join(root, "core", "c", "metrics-dump")

_lock = threading.Lock()
_cache = {"text": "# HELP weft_exporter_up Exporter state.\n# TYPE weft_exporter_up gauge\nweft_exporter_up 0\n", "ts": 0.0, "err": None}

def refresh():
    with _lock:
        try:
            out = subprocess.run(CMD, shell=True, capture_output=True, text=True,
                                 timeout=30, check=True).stdout
            host = os.environ.get("WEFT_METRICS_HOST", socket.gethostname())
            _cache["text"] = out.replace('host="sandbox"', f'host="{host}"') if 'host="sandbox"' in out else out
            _cache["ts"] = time.time()
            _cache["err"] = None
        except (subprocess.SubprocessError, OSError) as e:
            _cache["err"] = str(e)

def loop():
    while True:
        refresh()
        time.sleep(INTERVAL)

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/scrape":
            refresh()
        with _lock:
            body = _cache["text"].encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass

if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=PORT)
    p.add_argument("--interval", type=float, default=INTERVAL)
    a = p.parse_args()
    threading.Thread(target=loop, daemon=True).start()
    print(f"weft-exporter: serving {CMD} on :{a.port}/metrics (interval {a.interval}s)")
    http.server.HTTPServer(("", a.port), Handler).serve_forever()
