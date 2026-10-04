#!/usr/bin/env python3
"""Serve webui/index.html against a simulated periscope so the page can be exercised in a
browser before any flash (docs/REWRITE-PLAN.md §4, "Browser verification").

    python3 webui/dev-server.py            # http://127.0.0.1:8765

The simulation is deliberately small but speaks the same /api/* contract as
UppityFirmware/src/arduino/Net.cpp and prints the same kind of log lines as the console, so the
page's reply-matching is exercised too: a lifter and a rotary that move toward targets, the
E-STOP latch, the lock, the 60 s override, the jog lease, #PNUDGE, the settings keys with their
"(saved)" confirmations, calibration and rotary-test progress, the commissioning steps, an
update upload that "restarts" with a new build id, and factory reset. It is a stand-in for
layout and flow checks, not a model of the motion core (the host tests are that).
"""
import json, threading, time, urllib.parse, pathlib, re, random, traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent
PORT = 8765
STEPS = ["not started", "profile", "wiring check", "lift test", "rotary", "calibration", "safe-spin height", "acceptance", "complete"]
PROFILES = ["Pololu 4757 (6.3:1, original)", "Pololu 4751 (19:1, high-torque)", "IA-Parts"]

def todd():
    return {"MOTOR": 1, "INVERT": 1, "TRAVEL": 1150, "SAFESPIN": 904, "ROTARYTPR": 11855, "HOMEOFFSET": 0, "HOMEPARK": 1, "WIGGLE": 2,
            "MINUP": 50, "MINDOWN": 50, "APPROACH": 0, "EXPERT": 0, "DRIFT": 5, "ROTARYMIN": 40, "ROTARYOFF": 0, "DRIVESIGN": -1,
            "AGGRESSION": 1, "ID": 0, "BAUD": 9600, "DEDUP": 750, "DEBUG": 0, "CMDTX": 0, "LIFTERNC": 0, "ROTARYNC": 0, "WIFI": 1,
            "APCHANNEL": 1, "THEME": 0, "RANDOMLIGHT": 7, "COMMISSIONED": 1}
RANGES = {"MOTOR": (0, 2), "INVERT": (0, 1), "TRAVEL": (0, 5000), "SAFESPIN": (0, 5000), "ROTARYTPR": (0, 50000), "HOMEOFFSET": (-600, 600),
          "HOMEPARK": (0, 2), "WIGGLE": (0, 20), "MINUP": (0, 100), "MINDOWN": (0, 100), "APPROACH": (0, 100), "EXPERT": (0, 1), "DRIFT": (0, 20),
          "ROTARYMIN": (10, 100), "ROTARYOFF": (0, 1), "DRIVESIGN": (-1, 1), "AGGRESSION": (0, 2), "ID": (0, 255), "BAUD": (1200, 921600),
          "DEDUP": (0, 5000), "DEBUG": (0, 1), "CMDTX": (0, 1), "LIFTERNC": (0, 1), "ROTARYNC": (0, 1), "WIFI": (0, 1), "APCHANNEL": (1, 13),
          "THEME": (0, 1), "RANDOMLIGHT": (0, 8), "COMMISSIONED": (0, 2)}

class Sim:
    def __init__(self):
        self.lock = threading.Lock()
        self.t0 = time.time(); self.build = "%08x" % random.getrandbits(32)
        self.cfg = todd(); self.strings = {"SSID": "WordNet", "PASS": "(set)", "APPASS": ""}
        self.cal = {"valid": True, "applies": True, "travel": 1179, "minUp": 250, "minDown": 150, "motor": 1}
        self.lifter = 0.0; self.lifterKnown = True; self.target = None; self.liftSpeed = 700.0
        self.rot = 0.0; self.rotTarget = None; self.spin = 0.0; self.rotKnown = True; self.homeProv = "SWITCH"
        self.estop = False; self.estopSource = "none"; self.faults = 0; self.override_until = 0; self.suspect = False
        self.job = "NONE"; self.program = "NONE"; self.phase = 0; self.detail = 0; self.jobUntil = 0; self.jobKind = None
        self.last = ("NONE", "NONE", "-"); self.ids = 0
        self.random = False; self.light = 1
        self.jogUntil = 0; self.jog = None
        self.cm = {"step": 0, "active": False, "run": "idle", "msg": "", "detail": 0, "prevSafeSpin": 0}
        self.enabledUntil = 0
        self.log = "Uppity Spinner ALT v4.0.0-dev  phase 4  build %s  (dev server)\nHAL=ok\nEXPANDER=ok (drivers off, lights off)\nSETTINGS=loaded schema 4\nREADY (type #PHELP)\n" % self.build
        threading.Thread(target=self.run, daemon=True).start()

    # ---- helpers
    def now(self): return time.time() - self.t0
    def say(self, s):
        # event lines carry a timestamp on the board ("t=12.345 SEEK START ..."); console answers ("KEY=...") do not
        for l in s.split("\n"):
            event = l.startswith(("SEEK", "HOME", "ROT", "CREEP", "SPIN", "CAL ", "ESTOP", "OVERRIDE", "LIMIT", "ALIGN", "ROTARY", "STATE", "JOG_L", "CALIBRATE")) and not re.match(r"^[A-Z_]+=", l)
            self.log += ("t=%.3f %s\n" % (self.now(), l)) if event else l + "\n"
    def travel(self): return self.cfg["TRAVEL"] or self.cal["travel"] or 1350
    def safe(self): return self.cfg["SAFESPIN"] or self.travel()
    def pct(self): return max(0, min(100, int(self.lifter * 100 / self.travel())))
    def overrideLive(self): return time.time() < self.override_until
    def locked(self): return self.cfg["COMMISSIONED"] == 0 and not self.cm["active"]
    def home(self): return self.cfg["ROTARYOFF"] or (self.rotKnown and min(self.rot % 360, 360 - self.rot % 360) <= 10)
    def aboveSafe(self): return self.lifter >= self.travel() - 1 or self.lifter > self.safe()
    def begin(self, kind, program=None):
        self.ids += 1; self.job = kind; self.program = program or kind; self.phase = 0; self.detail = 0; self.enabledUntil = time.time() + 2
        self.say("%s START param=0 id=%d" % (kind, self.ids)); return self.ids
    def end(self, result="DONE", reason="-"):
        self.say("%s %s %s" % (self.job, result, reason)); self.last = (self.program, result, reason)
        self.job = "NONE"; self.program = "NONE"; self.phase = 0; self.detail = 0; self.target = None; self.rotTarget = None; self.jobKind = None; self.jog = None
    def refuse(self, kind, reason):
        self.ids += 1; self.last = (kind, "REFUSED", reason); self.say("%s REFUSED %s" % (kind, reason)); return False
    def gate(self, kind):
        if self.estop: return self.refuse(kind, "ESTOP")
        if self.locked(): return self.refuse(kind, "LOCKED")
        return True

    # ---- the motion loop (20 Hz)
    def run(self):
        while True:
            time.sleep(0.05)
            with self.lock:
                try: self.step(0.05)
                except Exception: import traceback; traceback.print_exc()

    def step(self, dt):
        t = time.time()
        if self.overrideLive() is False and getattr(self, "_ovrWas", False): self.say("OVERRIDE off")
        self._ovrWas = self.overrideLive()
        if self.estop: return
        if self.job in ("SEEK", "CREEP") and self.target is not None:
            d = self.target - self.lifter; step = self.liftSpeed * dt
            if abs(d) <= step:
                self.lifter = self.target; k = self.jobKind
                self.end("DONE_LIMIT" if self.target in (0, self.travel()) else "DONE_TARGET", "LIMIT" if self.target in (0, self.travel()) else "TARGET")
                if k == "cm-lift": self.cmPass("reached the top switch; the creep saw 1181 ticks from the bottom switch (travel in use: %d)" % self.travel())
                elif k == "cm-raise": self.begin("ROT_ABS", "ROT_ABS"); self.rotTarget = 90; self.jobKind = "cm-turn"
                elif k == "acc": self.accNext()
                elif k == "pa0": self.begin("HOME", "HOME"); self.rotTarget = 0; self.jobKind = "pa0h"
            else: self.lifter += step if d > 0 else -step
            self.detail = self.phase = 0
        if self.job == "JOG_L":
            if t > self.jogUntil: self.end("DONE", "LEASE_EXPIRED")
            else:
                self.lifter = max(0, min(self.travel(), self.lifter + self.jog * 250 * dt))
                if self.jog < 0 and not self.home() and not self.overrideLive() and self.lifter <= self.safe() and self.lifter > self.safe() - 5: self.end("CLAMPED", "ROTARY_NOT_HOME")
        if self.job == "ROT_JOG":
            if t > self.jogUntil: self.end("DONE", "LEASE_EXPIRED")
            else: self.rot = (self.rot + self.jog * 40 * dt) % 360
        if self.job == "SPIN": self.rot = (self.rot + self.spin * dt) % 360
        if self.job in ("ROT_ABS", "HOME") and self.rotTarget is not None:
            d = (self.rotTarget - self.rot + 180) % 360 - 180
            if abs(d) <= 4:
                self.rot = self.rotTarget % 360; self.rotKnown = True; k = self.jobKind
                if self.job == "HOME": self.homeProv = "EDGE"; self.suspect = False
                self.end("DONE" if self.job == "HOME" else "DONE_TARGET", "-" if self.job == "HOME" else "TARGET")
                if k == "cm-turn": self.cm.update(run="idle", msg="head turned 90 degrees at the top. Turn the override on, then lower in small steps while watching; stop while the head is still clear and store that height"); self.say("COMMISSION=" + self.cm["msg"])
                elif k == "cm-home": self.cmPass("1185.4 ticks/rev (spread 0.1 %), contact arc 50 ticks, 7 closures over 4 passes, confident; home found")
                elif k == "acc": self.accNext()
            else: self.rot = (self.rot + max(-12, min(12, d))) % 360
        if self.job == "ROTARYTEST":
            self.rot = (self.rot + 60 * dt) % 360; self.detail = int((t - self.jobStart) / 1.5)
            if self.detail >= self.revs + 1:
                self.say("ROTARY ticks/rev=1185.4 spread=0.1%\nROTARY contact arc=50 ticks closures=7\nROTARY verdict=confident (stored)")
                self.rot = 0; self.cfg["ROTARYTPR"] = 11854; k = self.jobKind; self.end("DONE", "-")
                if k == "cm-rot": self.begin("HOME", "HOME"); self.rotTarget = 0; self.jobKind = "cm-home"
                else: self.say("ROTARY stored: 1185.4 ticks/rev confident=1 arc=50")
        if self.job == "CALIBRATE":
            e = t - self.jobStart; total = 12.0
            ph = 1 + int(e / total * 6)
            if ph != self.phase: self.say("CAL phase %d at %d" % (ph, self.lifter))
            self.phase = min(ph, 6); frac = (e / total * 6) % 1
            self.detail = int(frac * 4) if self.phase == 1 else int(500 - frac * 300) if self.phase in (2, 3) else int(5 + frac * 12) if self.phase in (4, 5) else 0
            self.lifter = abs(((e * 400) % (2 * self.travel())) - self.travel())
            if e >= total:
                self.lifter = 0; k = self.jobKind; self.say("CAL travel=1181 ticks spread=0.4%\nCAL min power up=250 down=150 (permille)"); self.end("DONE", "-")
                self.cal.update(valid=True, applies=True, travel=1181, minUp=250, minDown=150, motor=self.cfg["MOTOR"])
                self.say("CAL stored: travel=1181 minUp=25%% minDown=15%% (profile %d)" % self.cfg["MOTOR"])
                if k == "cm-cal": self.cmPass("travel 1181 ticks, lowest power up 25 % / down 15 % (stored)")
        if self.acc and self.job == "NONE" and self.accWait and t > self.accWait: self.accWait = 0; self.accNext()

    # ---- commissioning (the device-side flow, simplified)
    acc = None; accWait = 0; jobStart = 0; revs = 3
    def cmPass(self, msg): self.cm.update(run="passed", msg=msg); self.say("COMMISSION=step %d passed: %s (#PCOMMISSIONNEXT)" % (self.cm["step"], msg))
    def cmFail(self, msg): self.cm.update(run="failed", msg=msg); self.say("COMMISSION=step %d FAILED: %s" % (self.cm["step"], msg))
    def accNext(self):
        if not self.acc: return
        el = self.acc.pop(0)
        if el == "done": self.acc = None; self.random = False; self.say("RANDOM=off (commissioning)"); self.cmPass("home, 0 %, 50 %, 100 %, home and a random action all ran"); return
        if el == "random": self.random = True; self.say("RANDOM=on level=0 (gentle)\nCOMMISSION=acceptance: one gentle random action\nRANDOM action=3 next=8000ms"); self.acc = ["ra", "done"] + self.acc; self.accNext(); return
        if el == "ra": self.begin("ROT_ABS", "ROT_ABS"); self.rotTarget = 137; self.jobKind = "acc"; self.acc.insert(0, "home"); return
        if el == "home": self.begin("HOME", "HOME"); self.rotTarget = 0; self.jobKind = "acc"; return
        self.begin("SEEK", "SEEK"); self.target = int(el * self.travel() / 100); self.jobKind = "acc"

    def commission(self, word):
        cm = self.cm
        if word == "":
            self.say("COMMISSION=step %d/7 (%s) active=%d run=%s commissioned=%d" % (cm["step"], STEPS[cm["step"]], cm["active"], cm["run"], self.cfg["COMMISSIONED"])); return
        if word == "START":
            if self.estop: self.say("COMMISSION=refused (E-STOP latched: reset it first)"); return
            if cm["step"] == 8: self.say("COMMISSION=already complete (#PCOMMISSIONRESET runs it again from step 1)"); return
            if cm["step"] == 0: cm.update(step=1, run="idle", msg="")
            cm["active"] = True; self.say("COMMISSION=active step %d/7 (%s): the commissioning steps may move the motors even while LOCKED" % (cm["step"], STEPS[cm["step"]])); return
        if word == "STOP": cm["active"] = False; self.say("COMMISSION=paused at step %d (operator): #PCOMMISSIONSTART resumes" % cm["step"]); return
        if word == "RESET": cm.update(step=1 if cm["active"] else 0, run="idle", msg=""); self.say("COMMISSION=reset to step %d" % cm["step"]); return
        if word == "BYPASS": self.cfg["COMMISSIONED"] = 1; cm["active"] = False; self.say("COMMISSIONED=1 (expert bypass: unlocked without commissioning; saved; #PLOCK to undo)"); return
        if not cm["active"]: self.say("COMMISSION=not active (#PCOMMISSIONSTART)"); return
        if word == "NEXT":
            if cm["run"] == "busy": self.say("COMMISSION=busy (wait for the step to finish)"); return
            if cm["step"] >= 3 and cm["run"] != "passed": self.say("COMMISSION=step %d not done: this step has not passed yet (#PCOMMISSIONRUN)" % cm["step"]); return
            if cm["step"] == 7: cm.update(step=8, active=False, run="idle", msg=""); self.cfg["COMMISSIONED"] = 2; self.say("COMMISSIONED=2 (commissioning complete; saved)"); return
            cm.update(step=cm["step"] + 1, run="idle", msg="", detail=0); self.say("COMMISSION=step %d/7 (%s)" % (cm["step"], STEPS[cm["step"]])); return
        if word == "RUN":
            if cm["run"] == "busy" or self.job != "NONE": self.say("COMMISSION=wait (something is still moving)"); return
            s = cm["step"]; cm.update(run="busy", msg="")
            if s == 3:
                if self.lifter >= self.travel() - 1: self.cmPass("the mast is already on the top switch: step 5's first move (a creep down) checks the direction"); return
                self.begin("CREEP", "CREEP"); self.target = self.travel(); self.jobKind = "cm-lift"; cm["msg"] = "creep up to the top switch (continuous, the profile's lowest power)"
            elif s == 4:
                self.begin("ROTARYTEST", "ROTARYTEST"); self.jobStart = time.time(); self.revs = 3; self.jobKind = "cm-rot"; self.lifter = self.travel(); cm["msg"] = "raising to the top, then three turns to measure the rotary"
            elif s == 5:
                self.begin("CALIBRATE", "CALIBRATE"); self.jobStart = time.time(); self.jobKind = "cm-cal"; cm["msg"] = "calibrating the lifter (several minutes; E-STOP stops it)"
            elif s == 6:
                cm["prevSafeSpin"] = self.cfg["SAFESPIN"] or cm["prevSafeSpin"]; self.cfg["SAFESPIN"] = 0
                self.say("SAFESPIN=0 (commissioning: only the top counts as clear until the measured height is stored; it was %d)" % cm["prevSafeSpin"])
                self.begin("SEEK", "SEEK"); self.target = self.travel(); self.jobKind = "cm-raise"; cm["msg"] = "raising to the top"
            elif s == 7:
                self.acc = ["home", 0, 50, 100, "home", "random", "done"]; cm["msg"] = "home, 0 %, 50 %, 100 %, home, then one random action"; self.accNext()
            else: cm["run"] = "idle"; self.say("COMMISSION=nothing to run in step %d (%s): the page's checklist, then #PCOMMISSIONNEXT" % (s, STEPS[s])); return
            self.say("COMMISSION=step %d running: %s" % (s, cm["msg"]))

    # ---- /api/state
    def state(self):
        with self.lock:
            st = "STOPPED" if self.estop else "LOCKED" if self.cfg["COMMISSIONED"] == 0 else "FAULT" if self.faults & ~4096 else "MOVING" if self.job != "NONE" else "IDLE"
            tr = self.travel(); ovr = max(0, int(self.override_until - time.time() + 0.999))
            rotHome = self.rotKnown and min(self.rot % 360, 360 - self.rot % 360) < 3
            return {"fw": "4.0.0-dev", "up": int(self.now()), "state": st,
                    "lifter": {"ticks": int(self.lifter), "pct": self.pct(), "travel": tr, "conf": "KNOWN" if self.lifterKnown else "UNKNOWN"},
                    "rotary": {"ticks": int(self.rot * 1185.5 / 360), "deg": int(self.rot) % 360, "conf": ("SWITCH_LIVE" if rotHome else "LATCHED") if self.rotKnown else "UNKNOWN", "prov": self.homeProv, "tpr": self.cfg["ROTARYTPR"] / 10, "off": bool(self.cfg["ROTARYOFF"])},
                    "limits": {"top": self.lifter >= tr - 1, "bot": self.lifter <= 1, "home": rotHome},
                    "diag": {"lifter": False, "rotary": False}, "out": {"lifter": 500 if self.job in ("SEEK", "CREEP", "JOG_L", "CALIBRATE") else 0, "rotary": 400 if self.job in ("SPIN", "ROT_ABS", "HOME", "ROT_JOG", "ROTARYTEST") else 0},
                    "enabled": time.time() < self.enabledUntil or self.job != "NONE", "estop": self.estop, "estopSource": self.estopSource, "faults": self.faults,
                    "job": self.job, "jobId": self.ids, "program": self.program, "phase": self.phase, "detail": self.detail,
                    "alignment": "suspect" if self.suspect else "ok", "commissioning": self.cm["active"],
                    "last": {"job": self.last[0], "result": self.last[1], "reason": self.last[2], "id": self.ids},
                    "override": ovr, "commissioned": self.cfg["COMMISSIONED"], "tick": {"maxUs": 46, "late": 0},
                    "random": {"on": self.random, "level": self.cfg["AGGRESSION"]}, "seq": bool(self.acc), "light": self.light, "safeSpin": self.cfg["SAFESPIN"], "travelSet": tr,
                    "persist": "saved", "theme": self.cfg["THEME"], "build": self.build, "inputs": 0, "expander": True,
                    "commission": {"step": self.cm["step"], "name": STEPS[self.cm["step"]], "active": self.cm["active"], "run": self.cm["run"], "msg": self.cm["msg"], "detail": self.cm["detail"], "prevSafeSpin": self.cm["prevSafeSpin"]},
                    "log": len(self.log),
                    "wifi": {"ap": "R2Uppity-SIM0", "clients": 1, "sta": "up", "ssid": self.strings["SSID"], "ip": "127.0.0.1", "rssi": -61, "host": "uppity", "estopPort": 81, "estopHits": 0, "webEstops": 0, "stalled": 0}}

    def config(self):
        with self.lock:
            c = dict(self.cfg); c.update({"SSID": self.strings["SSID"], "PASS": "(set)" if self.strings["PASS"] else "", "APPASS": "(set)" if self.strings["APPASS"] else "", "apSsid": "R2Uppity-SIM0"})
            m = self.cfg["MOTOR"]; cap = 75 if m == 1 and not self.cfg["EXPERT"] else 100; prof = [65, 45, 30][m]
            cal = self.cal["valid"] and self.cal["motor"] == m
            c["cal"] = dict(self.cal, applies=cal)
            c["pw"] = {"floorUp": self.cfg["MINUP"] or (self.cal["minUp"] // 10 if cal else prof), "floorDown": self.cfg["MINDOWN"] or (self.cal["minDown"] // 10 if cal else prof),
                       "slowUp": self.cal["minUp"] // 10 if cal else prof, "slowDown": self.cal["minDown"] // 10 if cal else prof, "cap": cap,
                       "rotaryMin": self.cfg["ROTARYMIN"], "rotarySlow": max(12, self.cfg["ROTARYMIN"] * 2 // 3), "profile": PROFILES[m], "travel": self.travel()}
            c["fw"] = "4.0.0-dev"; c["build"] = self.build
            return c

    # ---- commands (the console's answers)
    def cmd(self, line, src="web"):
        with self.lock: self._cmd(line.strip(), src)

    def setkey(self, key, text):
        if key in self.strings:
            if key in ("PASS", "APPASS"):
                if key == "APPASS" and 0 < len(text) < 8: self.say("#PAPPASS=(set) (unchanged; takes effect after #PRESTART)\nAPPASS note: WPA2 needs 8+ characters; kept the previous password"); return
                self.strings[key] = "(set)" if text else ""; self.say("#P%s=%s (saved; takes effect after #PRESTART)" % (key, "(set)" if text else "(cleared)")); return
            changed = self.strings[key] != text; self.strings[key] = text; self.say("#P%s=%s (%s; takes effect after #PRESTART)" % (key, text, "saved" if changed else "unchanged")); return
        if key not in self.cfg: self.say("INVALID=#P%s (unknown; #PHELP lists the keys)" % key); return
        try: v = int(text)
        except ValueError: self.say("OUT_OF_RANGE=#P%s=%s" % (key, text)); return
        lo, hi = RANGES[key]
        if v < lo or v > hi or (key == "TRAVEL" and v and v < 100) or (key == "DRIVESIGN" and v == 0): self.say("OUT_OF_RANGE=#P%s%d" % (key, v)); return
        changed = self.cfg[key] != v; note = ""
        if key == "MOTOR" and changed:
            self.cfg.update(COMMISSIONED=0, TRAVEL=0, SAFESPIN=0, MINUP=0, MINDOWN=0, APPROACH=0); self.cm["step"] = 1 if self.cm["step"] else 0
            note = " (new profile: LOCKED again, travel/safe-spin/min powers cleared, calibration ignored until #PSC, commissioning back to step 1)"
        if key == "INVERT": note = " (re-referenced from the limit switches)"
        self.cfg[key] = v
        self.say("#P%s=%d (%s)%s" % (key, v, "saved" if changed else "unchanged", note))

    def _cmd(self, line, src):
        say = self.say
        if not line: return
        m = re.match(r"#P([A-Z]+)=(.*)$", line)
        if m and m.group(1) not in ("SEQNAME",): self.setkey(m.group(1), m.group(2)); return
        if line == "#PSTATE": say("STATE=%s lifter=%d/%d(%d%%) rotary %ddeg" % (self.job, self.lifter, self.travel(), self.pct(), self.rot)); return
        if line == "#PSTATUS": say("VERSION=4.0.0-dev phase4 build %s\nWIFI=ap \"R2Uppity-SIM0\" 192.168.4.1 clients=1 home=WordNet up 127.0.0.1" % self.build); return
        if line == "#PCONFIG":
            say("CONFIG=begin v4.0.0-dev schema=4 (every #P line below can be pasted back)")
            for k in ["MOTOR", "INVERT", "TRAVEL", "SAFESPIN", "ROTARYTPR", "HOMEOFFSET", "HOMEPARK", "WIGGLE", "MINUP", "MINDOWN", "APPROACH", "EXPERT", "DRIFT", "ROTARYMIN", "ROTARYOFF", "DRIVESIGN", "AGGRESSION", "ID", "BAUD", "DEDUP", "DEBUG", "CMDTX", "LIFTERNC", "ROTARYNC", "WIFI", "APCHANNEL", "THEME", "RANDOMLIGHT", "COMMISSIONED"]:
                say("#P%s=%d" % (k, self.cfg[k]))
            say("#PSSID=%s\nINFO=PASS set, APPASS default (passwords are never printed)\nSEQ=none stored\nCONFIG=end travel_in_use=%d (#PTRAVEL)" % (self.strings["SSID"], self.travel())); return
        if line == "#PCLEARFAULT": self.faults = 0; say("FAULT=clear_requested"); return
        if line == "#PRESUME": was = self.estop; self.estop = False; say("ESTOP=released" if was else "ESTOP=not_latched"); return
        if line == "#PUNLOCK": self.cfg["COMMISSIONED"] = 1; say("COMMISSIONED=1 (saved: the board boots unlocked from now on; #PLOCK to undo)"); return
        if line == "#PLOCK": self.cfg["COMMISSIONED"] = 0; say("COMMISSIONED=0 (saved)"); return
        if line == "#PFAULT": self.faults = 8; return                            # dev only: show the fault banner
        if line == "#PFACTORY":
            if self.job != "NONE": say("FACTORY=refused (something is moving; :PX or wait)"); return
            say("FACTORY=cleared settings, calibration and sequences (v3's stored values untouched)\nFACTORY=done, restarting")
            self.cfg = todd(); self.cfg.update(COMMISSIONED=0, INVERT=0, TRAVEL=0); self.cm.update(step=0, active=False, run="idle", msg=""); self.restart(); return
        if line == "#PRESTART":
            if self.job != "NONE": say("RESTART=refused (something is moving; :PX or wait)"); return
            say("RESTART=in 300 ms"); self.restart(); return
        m = re.match(r"#POVERRIDE([01]?)$", line)
        if m:
            if m.group(1) == "0": self.override_until = 0; say("OVERRIDE=off"); return
            if self.estop: say("OVERRIDE=refused (E-STOP latched: reset it first)"); return
            self.override_until = time.time() + 60; say("OVERRIDE=on (60 s; #POVERRIDE0 ends it)\nOVERRIDE on for 60s: descents without a home and rotary jog below the safe-spin height are allowed"); return
        m = re.match(r"#PJOG([LR])([+-])(\d*)$", line)
        if m:
            kind = "JOG_L" if m.group(1) == "L" else "ROT_JOG"
            if not self.gate("CREEP" if kind == "JOG_L" else "ROT_JOG"): return
            dirn = 1 if m.group(2) == "+" else -1
            if kind == "JOG_L" and dirn < 0 and not self.lifterKnown: self.refuse("CREEP", "LIFTER_UNKNOWN"); return
            if kind == "ROT_JOG" and not self.aboveSafe() and not self.overrideLive(): self.refuse("ROT_JOG", "BELOW_SAFE_HEIGHT"); return
            if kind == "ROT_JOG" and not self.aboveSafe(): self.suspect = True; say("ALIGNMENT suspect: the head may be turned at or below the safe-spin height; descents need a home first (or the override)")
            self.begin(kind, "CREEP" if kind == "JOG_L" else "ROT_JOG"); self.jog = dirn; say("JOG=%s at %s%%" % (kind, m.group(3) or "slow")); return
        if line == "#PJOG0": self.jogUntil = 0; return
        m = re.match(r"#PNUDGE([+-]?\d+)$", line)
        if m:
            n = max(-100, min(100, int(m.group(1))))
            if not self.gate("CREEP"): return
            if n < 0 and not self.home() and not self.overrideLive():
                if self.lifter >= self.travel() - 1 or self.lifter > self.safe(): say("CREEP START param=%d id=%d\nCREEP CLAMPED ROTARY_NOT_HOME" % (n, self.ids + 1)); self.ids += 1; return
                if self.suspect: self.refuse("CREEP", "ROTARY_NOT_HOME"); return
            if n < 0 and not self.home() and self.overrideLive(): self.suspect = True
            self.begin("CREEP", "CREEP"); self.target = max(0, min(self.travel(), self.lifter + n)); say("NUDGE=%+d ticks at 15%% id=%d" % (n, self.ids)); return
        if line == "#PSETHOME":
            if self.estop: say("SETHOME=refused (E-STOP latched)"); return
            if self.job != "NONE": say("SETHOME=refused (something is moving; :PX or wait)"); return
            self.rot = 0; self.rotKnown = True; self.homeProv = "MANUAL"; self.suspect = False
            say("SETHOME=requested (the head's position now becomes home)"); say("HOME SET by the operator (the head was at 290 ticks, LATCHED): this position is home now"); return
        m = re.match(r"#PCOMMISSION(START|STOP|RUN|NEXT|RESET|BYPASS)?$", line)
        if m: self.commission(m.group(1) or ""); return
        if line == "#PSAFESPINHERE":
            cm = self.cm; turned = min(self.rot % 360, 360 - self.rot % 360) >= 45
            why = None
            if not cm["active"] or cm["step"] != 6: why = "only in commissioning step 6, after lowering the turned head"
            elif self.job != "NONE": why = "wait until nothing moves"
            elif self.lifter >= self.travel() - 1: why = "the mast is still at the top: lower it first"
            elif not turned: why = "the head is within 45 degrees of home: turn it at the top first"
            if why: say("SAFESPINHERE=refused (%s)" % why); return
            margin = max(20, self.travel() * 3 // 100); v = int(self.lifter) + margin
            self.cfg["SAFESPIN"] = v if v < self.travel() else 0
            cm.update(run="passed", msg="stored %d ticks (%d %%): measured %d + %d margin" % (v, v * 100 // self.travel(), self.lifter, margin), detail=v)
            say("SAFESPIN=%d (saved; %s). Next: raise and bring the head home (:PA0), end the override (#POVERRIDE0)" % (self.cfg["SAFESPIN"], cm["msg"])); return
        m = re.match(r"#PROTARYTEST(\d*)$", line)
        if m:
            if not self.gate("ROTARYTEST"): return
            self.revs = int(m.group(1) or 3); self.begin("ROTARYTEST", "ROTARYTEST"); self.jobStart = time.time(); self.jobKind = None
            self.lifter = self.travel(); say("QUEUED=ROTARYTEST id=%d" % self.ids); return
        if line == "#PSC":
            if not self.gate("CALIBRATE"): return
            self.begin("CALIBRATE", "CALIBRATE"); self.jobStart = time.time(); self.jobKind = None; say("QUEUED=CALIBRATE id=%d" % self.ids); return
        m = re.match(r"#PTHEME=?(\d)$", line)
        if m: self.setkey("THEME", m.group(1)); return
        if line.startswith("#P"):
            m = re.match(r"#P([A-Z]+)(-?\d+)$", line)
            if m: self.setkey(m.group(1), m.group(2)); return
            say("INVALID"); return
        # ---- motion
        if self.estop and line[:3] in (":PP", ":PH", ":PA", ":PD", ":PR"): self.refuse("SEEK", "ESTOP"); return
        if self.locked() and line[:3] in (":PP", ":PH", ":PA", ":PD", ":PR"): self.refuse("SEEK", "LOCKED"); return
        self.acc = None
        m = re.match(r":PP(\d+)", line)
        if m:
            tgt = int(m.group(1)) * self.travel() // 100
            if tgt < self.lifter and not self.home() and not self.overrideLive():
                if self.lifter > self.safe(): say("SEEK CLAMPED %d -> %d reason=ROTARY_NOT_HOME" % (tgt, self.safe())); tgt = max(tgt, self.safe())
                elif self.suspect: self.refuse("SEEK", "ROTARY_NOT_HOME"); return
            self.begin("SEEK", "SEEK"); self.target = tgt; self.random = False; say("QUEUED=SEEK id=%d" % self.ids); return
        if line.startswith(":PH"): self.begin("HOME", "SAFE_LOWER"); self.rotTarget = 0; self.jobKind = "ph"; self.light = 1; self.random = False; say("QUEUED=SAFE_LOWER id=%d" % self.ids); self._ph = True; return
        m = re.match(r":PA(\d+)", line)
        if m:
            deg = int(m.group(1)) % 360
            if deg == 0 and not self.aboveSafe(): self.begin("SEEK", "HOME"); self.target = self.travel(); self.jobKind = "pa0"; say("QUEUED=HOME id=%d" % self.ids); return
            if not self.aboveSafe(): self.begin("SEEK", "ROT_ABS"); self.target = self.travel(); self.jobKind = None
            self.begin("HOME" if deg == 0 else "ROT_ABS", "HOME" if deg == 0 else "ROT_ABS"); self.rotTarget = deg; self.spin = 0; self.random = False; say("QUEUED=%s id=%d" % ("HOME" if deg == 0 else "ROT_ABS", self.ids)); return
        m = re.match(r":PR(-?\d+)", line)
        if m: self.begin("SPIN", "SPIN"); self.spin = int(m.group(1)); self.random = False; say("QUEUED=SPIN id=%d" % self.ids); return
        m = re.match(r":PL(\d)", line)
        if m: self.light = int(m.group(1)); say("LIGHT=%d" % self.light); return
        if line.startswith(":PM"): self.random = True; say("RANDOM=on level=1 (medium)"); return
        say("INVALID=" + line)

    def restart(self):
        def later():
            time.sleep(0.8)
            with self.lock: self.build = "%08x" % random.getrandbits(32); self.t0 = time.time(); self.log += "\nUppity Spinner ALT v4.0.0-dev  phase 4  build %s  (dev server, restarted)\nREADY\n" % self.build
        threading.Thread(target=later, daemon=True).start()

    def stop(self):
        with self.lock:
            self.random = False; self.acc = None
            if self.job != "NONE": self.end("ABORTED", "CANCEL")
            self.say("STOP=web")

SIM = Sim()
# ":PH" in the sim: home, then lower (the real one raises first when needed)
_orig_end = Sim.end
def _end(self, result="DONE", reason="-"):
    ph = self.jobKind == "ph" and self.job == "HOME"
    _orig_end(self, result, reason)
    if ph and result == "DONE": self.begin("SEEK", "SAFE_LOWER"); self.target = 0
Sim.end = _end

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def js(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Cache-Control", "no-store"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        u = urllib.parse.urlparse(self.path); q = urllib.parse.parse_qs(u.query)
        if u.path in ("/", "/index.html"):
            b = (ROOT / "index.html").read_bytes(); self.send_response(200); self.send_header("Content-Type", "text/html"); self.send_header("Cache-Control", "no-cache"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b); return
        if u.path == "/api/state": self.js(200, SIM.state()); return
        if u.path == "/api/config": self.js(200, SIM.config()); return
        if u.path == "/api/log":
            since = q.get("since", ["tail"])[0]; total = len(SIM.log)
            frm = max(0, total - 2048) if since == "tail" else min(int(since), total)
            to = min(total, frm + 3000)
            self.js(200, {"from": frm, "next": to, "text": SIM.log[frm:to]}); return
        if u.path == "/api/estop": self.estop(); return
        self.js(404, {"ok": False, "err": "unknown"})
    def estop(self):
        with SIM.lock:
            if not SIM.estop:
                SIM.estop = True; SIM.estopSource = "web"; SIM.override_until = 0; SIM.jogUntil = 0; SIM.acc = None; SIM.random = False
                if SIM.job != "NONE": SIM.end("ABORTED", "ESTOP")
                SIM.say("ESTOP WEB from 127.0.0.1\nESTOP LATCHED (source 3)")
                if SIM.cm["active"]:
                    if SIM.cm["run"] == "busy": SIM.cm.update(run="failed", msg="stopped (E-STOP)")
                    SIM.cm["active"] = False; SIM.say("COMMISSION=paused at step %d (E-STOP): #PCOMMISSIONSTART resumes" % SIM.cm["step"])
        self.js(200, {"ok": True, "msg": "E-STOP latched"})
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); u = urllib.parse.urlparse(self.path)
        if u.path == "/update":
            remaining = n
            while remaining > 0: chunk = self.rfile.read(min(65536, remaining)); remaining -= len(chunk); time.sleep(0.02)
            with SIM.lock:
                if SIM.job != "NONE": self.js(409, {"ok": False, "err": "armed", "msg": "update refused: a motor is enabled or a job is running"}); return
                SIM.say("OTA 100% done, restarting")
            SIM.restart(); self.js(200, {"ok": True, "msg": "Update written, restarting"}); return
        body = self.rfile.read(n).decode(errors="replace"); q = urllib.parse.parse_qs(body, keep_blank_values=True)
        if u.path == "/api/estop": self.estop(); return
        if u.path == "/api/estop/reset":
            with SIM.lock: was = SIM.estop; SIM.estop = False; SIM.say("ESTOP released" if was else "ESTOP=not_latched")
            self.js(200, {"ok": True, "msg": "E-STOP released" if was else "E-STOP was not latched"}); return
        if u.path == "/api/stop": SIM.stop(); self.js(200, {"ok": True, "msg": "Stopping"}); return
        if u.path == "/api/restart": SIM.cmd("#PRESTART"); self.js(200, {"ok": True, "msg": "Restarting"}); return
        if u.path == "/api/jog":
            axis = q.get("axis", [""])[0].upper(); d = q.get("dir", [""])[0]; pct = q.get("pct", ["0"])[0]
            if axis not in ("L", "R") or d not in ("1", "-1"): self.js(400, {"ok": False, "err": "invalid"}); return
            with SIM.lock: SIM.jogUntil = time.time() + 2.0
            if q.get("start", ["0"])[0] == "1": SIM.cmd("#PJOG%s%s%s" % (axis, "+" if d == "1" else "-", pct))
            self.js(200, {"ok": True}); return
        if u.path == "/api/jog/stop":
            with SIM.lock: SIM.jogUntil = 0
            self.js(200, {"ok": True}); return
        if u.path == "/api/cmd":
            line = q.get("line", [""])[0].strip()
            if not line or line[0] not in ":#": self.js(400, {"ok": False, "err": "invalid", "msg": "a command starts with :P or #P"}); return
            SIM.cmd(line); self.js(200, {"ok": True}); return
        if u.path == "/api/config":
            key = q.get("key", [""])[0].strip().upper(); value = q.get("value", [""])[0]
            if not re.fullmatch(r"[A-Z]{1,12}", key): self.js(400, {"ok": False, "err": "invalid", "msg": "key"}); return
            SIM.cmd("#P%s=%s" % (key, value)); self.js(200, {"ok": True}); return
        self.js(404, {"ok": False, "err": "unknown"})

if __name__ == "__main__":
    print("Uppity Spinner dev server: http://127.0.0.1:%d  (Ctrl+C to stop)" % PORT)
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
