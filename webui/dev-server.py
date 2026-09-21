#!/usr/bin/env python3
"""Serve webui/index.html against a simulated periscope so the page can be exercised in a
browser before any flash (docs/REWRITE-PLAN.md §4, "Browser verification").

    python3 webui/dev-server.py            # http://127.0.0.1:8765

The simulation is deliberately small: a lifter that moves toward a target over time, a rotary
that turns, an E-STOP latch, a lock, refusal codes, and a log that grows. It speaks the same
/api/* contract as UppityFirmware/src/arduino/Net.cpp.
"""
import json, threading, time, urllib.parse, pathlib, re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent
PORT = 8765

class Sim:
    def __init__(self):
        self.lock = threading.Lock()
        self.t0 = time.time()
        self.lifter = 0.0; self.target = 0.0; self.travel = 1150
        self.rot = 0.0; self.rotTarget = 0.0; self.spin = 0.0
        self.estop = False; self.commissioned = True; self.faults = 0
        self.job = "NONE"; self.last = ("SEEK", "DONE_LIMIT", "LIMIT"); self.ids = 0
        self.random = False; self.light = 1; self.theme = 0
        self.log = "Uppity Spinner ALT v4.0.0-dev  phase 3 (sim)\nHAL=ok\nEXPANDER=ok (drivers off, lights off)\nSETTINGS=loaded schema 3\nREADY (type #PHELP)\n"
        self.enabled = False; self.enabledUntil = 0
        threading.Thread(target=self.run, daemon=True).start()
    def say(self, s):
        self.log += "t=%.3f %s\n" % (time.time() - self.t0, s)
    def run(self):
        while True:
            time.sleep(0.05)
            with self.lock:
                if self.estop: self.job = "NONE"; self.spin = 0; continue
                if abs(self.lifter - self.target) > 2:
                    self.lifter += max(-60, min(60, self.target - self.lifter)); self.job = "SEEK"; self.enabled = True; self.enabledUntil = time.time() + 2
                elif self.job == "SEEK": self.job = "NONE"; self.last = ("SEEK", "DONE_TARGET", "TARGET"); self.say("SEEK DONE_TARGET TARGET"); self.enabledUntil = time.time() + 2
                if self.spin: self.rot = (self.rot + self.spin) % 360; self.job = "SPIN"; self.enabled = True
                elif abs((self.rotTarget - self.rot + 180) % 360 - 180) > 2:
                    d = (self.rotTarget - self.rot + 180) % 360 - 180; self.rot = (self.rot + max(-8, min(8, d))) % 360; self.job = "ROT_ABS"; self.enabled = True; self.enabledUntil = time.time() + 2
                elif self.job == "ROT_ABS": self.job = "NONE"; self.last = ("ROT_ABS", "DONE_TARGET", "TARGET"); self.say("ROT_ABS DONE_TARGET TARGET")
                if self.job == "NONE" and time.time() > self.enabledUntil: self.enabled = False
    def state(self):
        with self.lock:
            st = "STOPPED" if self.estop else "LOCKED" if not self.commissioned else "FAULT" if self.faults else "MOVING" if self.job != "NONE" else "IDLE"
            return {"fw": "4.0.0-dev", "up": int(time.time() - self.t0), "state": st,
                    "lifter": {"ticks": int(self.lifter), "pct": int(self.lifter * 100 / self.travel), "travel": self.travel, "conf": "KNOWN"},
                    "rotary": {"ticks": int(self.rot * 1185.5 / 360), "deg": int(self.rot), "conf": "SWITCH_LIVE" if abs(self.rot) < 5 else "LATCHED", "tpr": 1185.5},
                    "limits": {"top": self.lifter >= self.travel - 5, "bot": self.lifter <= 5, "home": abs(self.rot) < 5},
                    "enabled": self.enabled, "estop": self.estop, "estopSource": "web" if self.estop else "none", "faults": self.faults,
                    "job": self.job, "jobId": self.ids, "last": {"job": self.last[0], "result": self.last[1], "reason": self.last[2], "id": self.ids},
                    "override": 0, "commissioned": self.commissioned, "tick": {"maxUs": 46, "late": 0},
                    "random": {"on": self.random, "level": 1}, "seq": False, "light": self.light, "safeSpin": 904, "travelSet": 1150,
                    "persist": "saved", "theme": self.theme, "inputs": 0, "log": len(self.log),
                    "wifi": {"ap": "R2Uppity-SIM0", "clients": 1, "sta": "off", "ssid": "", "ip": "192.168.4.1", "rssi": 0, "host": "uppity"}}
    def cmd(self, line):
        with self.lock:
            self.say("(web) " + line)
            if self.estop and line.startswith(":P") and line[2:3] in "PHADR": self.say("SEEK REFUSED ESTOP"); return
            if not self.commissioned and line.startswith(":P") and line[2:3] in "PHADR": self.say("SEEK REFUSED LOCKED"); return
            m = re.match(r":PP(\d+)", line)
            if m: self.ids += 1; self.target = int(m.group(1)) * self.travel / 100; self.random = False; self.say("QUEUED=SEEK id=%d" % self.ids); return
            if line.startswith(":PH"): self.ids += 1; self.rotTarget = 0; self.target = 0; self.light = 1; self.random = False; self.say("QUEUED=SAFE_LOWER id=%d" % self.ids); return
            m = re.match(r":PA(\d+)", line)
            if m: self.ids += 1; self.rotTarget = int(m.group(1)) % 360; self.spin = 0; self.random = False; self.say("QUEUED=ROT_ABS id=%d" % self.ids); return
            m = re.match(r":PR(-?\d+)", line)
            if m: self.ids += 1; self.spin = int(m.group(1)) / 10; self.random = False; self.say("QUEUED=SPIN id=%d" % self.ids); return
            m = re.match(r":PL(\d)", line)
            if m: self.light = int(m.group(1)); self.say("LIGHT=%d" % self.light); return
            if line.startswith(":PM"): self.random = True; self.say("RANDOM=on level=1 (medium)"); return
            if line == "#PSTATE": self.say("STATE=IDLE lifter=%d/1150 ..." % self.lifter); return
            if line == "#PCONFIG": self.say("CONFIG=begin v4.0.0-dev schema=3\n#PINVERT=1\n#PTRAVEL=1150\n#PSAFESPIN=904\nCONFIG=end"); return
            if line == "#PUNLOCK": self.commissioned = True; self.say("COMMISSIONED=1 (saved)"); return
            if line == "#PLOCK": self.commissioned = False; self.say("COMMISSIONED=0 (saved)"); return
            if line == "#PCLEARFAULT": self.faults = 0; self.say("FAULT=clear_requested"); return
            m = re.match(r"#PTHEME=(\d)", line)
            if m: self.theme = int(m.group(1)); self.say("#PTHEME=%d (saved)" % self.theme); return
            if line == "#PFAULT": self.faults = 1; return
            self.say("INVALID")

SIM = Sim()

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def js(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Cache-Control", "no-store"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        u = urllib.parse.urlparse(self.path); q = urllib.parse.parse_qs(u.query)
        if u.path in ("/", "/index.html"):
            b = (ROOT / "index.html").read_bytes(); self.send_response(200); self.send_header("Content-Type", "text/html"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b); return
        if u.path == "/api/state": self.js(200, SIM.state()); return
        if u.path == "/api/log":
            since = q.get("since", ["tail"])[0]; total = len(SIM.log)
            frm = max(0, total - 2048) if since == "tail" else min(int(since), total)
            self.js(200, {"from": frm, "next": total, "text": SIM.log[frm:total]}); return
        if u.path == "/api/estop": SIM.estop = True; SIM.say("ESTOP LATCHED (source 3)"); self.js(200, {"ok": True, "msg": "E-STOP latched"}); return
        self.js(404, {"ok": False, "err": "unknown"})
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); body = self.rfile.read(n).decode(); q = urllib.parse.parse_qs(body); u = urllib.parse.urlparse(self.path)
        if u.path == "/api/estop": SIM.estop = True; SIM.say("ESTOP LATCHED (source 3)"); self.js(200, {"ok": True, "msg": "E-STOP latched"}); return
        if u.path == "/api/estop/reset": was = SIM.estop; SIM.estop = False; SIM.say("ESTOP released (source 3)" if was else "ESTOP=not_latched"); self.js(200, {"ok": True, "msg": "E-STOP released" if was else "E-STOP was not latched"}); return
        if u.path == "/api/stop": SIM.random = False; SIM.spin = 0; SIM.target = SIM.lifter; SIM.say("STOP=web"); self.js(200, {"ok": True, "msg": "Stopping"}); return
        if u.path == "/api/cmd":
            line = q.get("line", [""])[0].strip()
            if not line or line[0] not in ":#": self.js(400, {"ok": False, "err": "invalid"}); return
            SIM.cmd(line); self.js(200, {"ok": True}); return
        if u.path == "/api/config": self.js(200, {"ok": True}); return
        self.js(404, {"ok": False, "err": "unknown"})

if __name__ == "__main__":
    print("Uppity Spinner dev server: http://127.0.0.1:%d  (Ctrl+C to stop)" % PORT)
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
