#!/usr/bin/env python3
"""
Test server that mimics the ESP8266 temperature control web UI.
Simulated inputs: temperature drifts around setpoint. No hardware required.
Run: python3 test_web_server.py
Open: http://127.0.0.1:8080
"""

import http.server
import urllib.parse
from datetime import datetime

# Simulated state (mirrors ESP8266 globals)
INPUT = 62.0          # current temperature (simulated)
SETPOINT = 65.0
Kp, Ki, Kd = 1.5, 2.0, 2.5
MANUAL_MODE = False
MANUAL_STATE = False
SHELLY_IP = "192.168.178.88"

# Simulate temperature slowly drifting toward setpoint
def tick_simulation():
    global INPUT, SETPOINT
    import random
    # Random walk toward setpoint
    diff = SETPOINT - INPUT
    INPUT += diff * 0.02 + (random.random() - 0.5) * 0.3
    INPUT = max(10.0, min(90.0, INPUT))


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)

        if path == "/":
            tick_simulation()
            msg = qs.get("msg", [None])[0]
            html = self._root_html(msg)
            self._send_html(html)
        else:
            self.send_error(404)

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8")
        form = urllib.parse.parse_qs(body)

        if path == "/set":
            msg = self._handle_set(form)
            self.send_response(303)
            self.send_header("Location", "/" + ("?msg=" + msg if msg else ""))
            self.end_headers()
        elif path == "/manual":
            global MANUAL_STATE
            MANUAL_STATE = not MANUAL_STATE
            self.send_response(303)
            self.send_header("Location", "/")
            self.end_headers()
        else:
            self.send_error(404)

    def _handle_set(self, form):
        global SETPOINT, MANUAL_MODE
        setpoint_changed = False
        mode_changed = False
        if "setpoint" in form:
            try:
                SETPOINT = float(form["setpoint"][0])
                setpoint_changed = True
            except ValueError:
                pass
        if "mode" in form:
            MANUAL_MODE = form["mode"][0] == "man"
            mode_changed = True
        if setpoint_changed and mode_changed:
            return "setpoint_ok"
        if setpoint_changed:
            return "setpoint_ok"
        if mode_changed:
            return "mode_ok"
        return None

    def _root_html(self, msg):
        mode_opts = (
            '<option value="auto">Auto</option><option value="man" selected>Manuell</option>'
            if MANUAL_MODE
            else '<option value="auto" selected>Auto</option><option value="man">Manuell</option>'
        )
        msg_block = ""
        if msg == "setpoint_ok":
            msg_block = '<p style="color:green"><strong>Soll-Temperatur wurde übernommen.</strong></p>'
        elif msg == "mode_ok":
            msg_block = '<p style="color:green"><strong>Modus wurde übernommen.</strong></p>'

        manual_block = ""
        if MANUAL_MODE:
            label = "Ausschalten" if MANUAL_STATE else "Einschalten"
            manual_block = f'<form method="POST" action="/manual" style="margin-top:12px"><button type="submit" class="btn">{label}</button></form>'

        plug_on = MANUAL_STATE if MANUAL_MODE else (INPUT < SETPOINT)
        status_class = "on" if plug_on else "off"
        status_text = "Steckdose Ein" if plug_on else "Steckdose Aus"

        return f"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="5">
<title>Temperatursteuerung</title>
<style>
body{{font-family:sans-serif;max-width:360px;margin:20px auto;padding:0 16px;background:#f5f5f5;}}
.card{{background:#fff;border-radius:12px;padding:20px;margin-bottom:16px;box-shadow:0 2px 8px rgba(0,0,0,.08);}}
h1{{margin:0 0 16px;font-size:1.35rem;color:#333;}}
.temp{{font-size:2rem;font-weight:700;color:#1565c0;margin:8px 0;}}
.temp span{{font-size:1rem;font-weight:400;color:#666;}}
.status{{display:inline-flex;align-items:center;gap:6px;padding:6px 12px;border-radius:20px;font-size:0.9rem;font-weight:600;}}
.status.on{{background:#e8f5e9;color:#2e7d32;}}
.status.off{{background:#f5f5f5;color:#616161;}}
.status-dot{{width:8px;height:8px;border-radius:50%;}}
.status.on .status-dot{{background:#4caf50;}}
.status.off .status-dot{{background:#9e9e9e;}}
.msg{{color:#2e7d32;font-size:0.9rem;margin:8px 0;}}
label{{display:block;font-size:0.85rem;color:#666;margin-bottom:4px;}}
input[type="number"]{{width:100%;box-sizing:border-box;padding:10px;border:1px solid #ddd;border-radius:8px;font-size:1rem;}}
select{{padding:10px;border:1px solid #ddd;border-radius:8px;font-size:1rem;min-width:120px;}}
button,.btn{{padding:10px 16px;border:none;border-radius:8px;font-size:0.95rem;cursor:pointer;background:#1565c0;color:#fff;}}
button:hover,.btn:hover{{background:#0d47a1;}}
.row{{margin-bottom:12px;}}
.foot{{font-size:0.75rem;color:#999;margin-top:12px;}}
</style></head><body>
<div class="card">
<h1>Temperatursteuerung</h1>
<div class="temp">{INPUT:.1f} <span>°C</span></div>
<p style="margin:0 0 12px;font-size:0.85rem;color:#666">Soll: {SETPOINT:.1f} °C</p>
{msg_block}
<form method="POST" action="/set">
<div class="row"><label>Soll-Temperatur</label>
<input name="setpoint" value="{SETPOINT}" type="number" step="0.5" min="0" max="100">
</div><button type="submit">Soll übernehmen</button></form>
<form method="POST" action="/set" style="margin-top:12px">
<div class="row"><label>Modus</label>
<select name="mode">{mode_opts}</select></div><button type="submit">Modus übernehmen</button></form>
{manual_block}
<div style="margin-top:16px;padding-top:12px;border-top:1px solid #eee">
<span class="status {status_class}"><span class="status-dot"></span>{status_text}</span>
<p class="foot">Shelly Plug S @ {SHELLY_IP} &nbsp; PID: Kp={Kp} Ki={Ki} Kd={Kd}</p>
<p class="foot">[Test-Server – Simulierte Temperatur]</p>
</div></div></body></html>"""

    def _send_html(self, html):
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", len(html.encode("utf-8")))
        self.end_headers()
        self.wfile.write(html.encode("utf-8"))

    def log_message(self, format, *args):
        print(f"[{datetime.now().strftime('%H:%M:%S')}] {args[0]}")


def main():
    port = 8080
    server = http.server.HTTPServer(("127.0.0.1", port), Handler)
    print(f"Test server: http://127.0.0.1:{port}")
    print("Simulated temperature updates every page load. Ctrl+C to stop.")
    server.serve_forever()


if __name__ == "__main__":
    main()
