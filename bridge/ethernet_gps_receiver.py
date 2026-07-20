#!/usr/bin/env python3
"""
UABAMS Ethernet GPS Receiver
==============================
Listens for TCP connection from GPS board (192.168.1.200),
parses GPS data (NMEA or custom format), and publishes to
MQTT topic adj/datalogger/sensors/gps so server.js picks it up.

Architecture:
  GPS board (192.168.1.200) ──TCP client──> THIS SCRIPT (TCP server on :5001)
                                                    │
                                                    └──MQTT──> server.js ──> frontend

PORT: 5001  (5000 is the web server — GPS firmware must connect to 5001)
       OR use the iptables redirect trick below to avoid changing firmware.

iptables redirect (run once, no firmware change needed):
  sudo iptables -t nat -A PREROUTING -s 192.168.1.200 -p tcp --dport 5000 -j REDIRECT --to-ports 5001
  # To make it persist across reboots:
  sudo apt install iptables-persistent
  sudo netfilter-persistent save
"""

import socket
import threading
import time
import datetime
import re
import sys
import signal
import paho.mqtt.client as mqtt

# ── Config ────────────────────────────────────────────────────────────────
GPS_LISTEN_HOST = "0.0.0.0"
GPS_LISTEN_PORT = 5001              # GPS firmware must target this port
EXPECTED_GPS_IP = "192.168.1.200"  # Only accept connections from the GPS board

MQTT_HOST  = "127.0.0.1"           # Mosquitto on this machine
MQTT_PORT  = 1883
MQTT_TOPIC = "adj/datalogger/sensors/gps"

RECONNECT_DELAY = 5                 # seconds to wait before retrying MQTT connect

# ── MQTT ──────────────────────────────────────────────────────────────────
_mqtt_connected = False
_mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)


def _on_connect(client, userdata, flags, rc):
    global _mqtt_connected
    if rc == 0:
        _mqtt_connected = True
        print(f"[MQTT] Connected to {MQTT_HOST}:{MQTT_PORT}")
    else:
        print(f"[MQTT] Connection refused rc={rc}")


def _on_disconnect(client, userdata, rc):
    global _mqtt_connected
    _mqtt_connected = False
    print(f"[MQTT] Disconnected rc={rc}, will auto-reconnect")


def mqtt_setup():
    _mqtt_client.on_connect    = _on_connect
    _mqtt_client.on_disconnect = _on_disconnect
    while True:
        try:
            _mqtt_client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
            _mqtt_client.loop_start()
            return
        except Exception as e:
            print(f"[MQTT] Connect error: {e} — retry in {RECONNECT_DELAY}s")
            time.sleep(RECONNECT_DELAY)


def mqtt_publish(payload: str) -> bool:
    if not _mqtt_connected:
        return False
    result = _mqtt_client.publish(MQTT_TOPIC, payload)
    return result.rc == mqtt.MQTT_ERR_SUCCESS


# ── NMEA helpers ──────────────────────────────────────────────────────────
def _nmea_to_decimal(raw: str, direction: str) -> float:
    """Convert NMEA DDMM.MMMM / DDDMM.MMMM to decimal degrees."""
    raw = raw.strip()
    if not raw:
        return 0.0
    dot = raw.index('.')
    degrees = int(raw[:dot - 2])
    minutes = float(raw[dot - 2:])
    val = degrees + minutes / 60.0
    if direction.upper() in ('S', 'W'):
        val = -val
    return val


def _parse_gprmc(line: str):
    """
    $GPRMC,HHMMSS.ss,A,DDMM.MMMM,N,DDDMM.MMMM,E,spd_knots,course,DDMMYY,...
    Returns parsed dict or None on no-fix / bad parse.
    """
    parts = line.split(',')
    if len(parts) < 10:
        return None
    try:
        time_str    = parts[1]
        status      = parts[2]
        lat_raw, lat_dir = parts[3], parts[4]
        lon_raw, lon_dir = parts[5], parts[6]
        speed_knots = float(parts[7]) if parts[7] else 0.0
        date_str    = parts[9]

        if status != 'A' or not lat_raw or not lon_raw:
            return None

        lat = _nmea_to_decimal(lat_raw, lat_dir)
        lng = _nmea_to_decimal(lon_raw, lon_dir)
        speed_cms = int(speed_knots * 51.44)   # knots → cm/s  (1 knot = 51.44 cm/s)

        hh = int(time_str[0:2]); mm_t = int(time_str[2:4]); ss = int(float(time_str[4:]))
        dd = int(date_str[0:2]); mo   = int(date_str[2:4]); yr = 2000 + int(date_str[4:6])

        return dict(lat=lat, lng=lng, speed_cms=speed_cms,
                    hh=hh, mm=mm_t, ss=ss, dd=dd, mo=mo, yr=yr)
    except Exception:
        return None


def _parse_gpgga(line: str):
    """
    $GPGGA,HHMMSS.ss,DDMM.MMMM,N,DDDMM.MMMM,E,quality,sats,...
    No speed available in GGA; use 0.
    """
    parts = line.split(',')
    if len(parts) < 7:
        return None
    try:
        time_str = parts[1]
        lat_raw, lat_dir = parts[2], parts[3]
        lon_raw, lon_dir = parts[4], parts[5]
        quality = int(parts[6]) if parts[6] else 0

        if quality == 0 or not lat_raw or not lon_raw:
            return None

        lat = _nmea_to_decimal(lat_raw, lat_dir)
        lng = _nmea_to_decimal(lon_raw, lon_dir)
        hh = int(time_str[0:2]); mm_t = int(time_str[2:4]); ss = int(float(time_str[4:]))
        now = datetime.datetime.now()

        return dict(lat=lat, lng=lng, speed_cms=0,
                    hh=hh, mm=mm_t, ss=ss, dd=now.day, mo=now.month, yr=now.year)
    except Exception:
        return None


# ── Custom format parser ──────────────────────────────────────────────────
def _parse_custom(line: str):
    """
    Handles:
      GPS:T-13:13:47 D-27-03-2026 LAT:28584835N LON:77315948E SPD:25cm/s
      LAT:28584835N LON:77315948E SPD:25cm/s
    LAT/LON integers are already degrees × 1e6.
    """
    lat_m = re.search(r'LAT:(\d+)([NS])', line, re.IGNORECASE)
    lon_m = re.search(r'LON:(\d+)([EW])', line, re.IGNORECASE)
    spd_m = re.search(r'SPD:(\d+(?:\.\d+)?)cm/s', line, re.IGNORECASE)

    if not lat_m or not lon_m:
        return None

    lat = (int(lat_m.group(1)) / 1e6) * (1 if lat_m.group(2).upper() == 'N' else -1)
    lng = (int(lon_m.group(1)) / 1e6) * (1 if lon_m.group(2).upper() == 'E' else -1)
    speed_cms = int(float(spd_m.group(1))) if spd_m else 0

    now    = datetime.datetime.now()
    time_m = re.search(r'T-(\d{1,2}):(\d{2}):(\d{2})', line)
    date_m = re.search(r'D-(\d{1,2})-(\d{1,2})-(\d{4})', line)

    hh = int(time_m.group(1)) if time_m else now.hour
    mm_t = int(time_m.group(2)) if time_m else now.minute
    ss = int(time_m.group(3)) if time_m else now.second
    dd = int(date_m.group(1)) if date_m else now.day
    mo = int(date_m.group(2)) if date_m else now.month
    yr = int(date_m.group(3)) if date_m else now.year

    return dict(lat=lat, lng=lng, speed_cms=speed_cms,
                hh=hh, mm=mm_t, ss=ss, dd=dd, mo=mo, yr=yr)


def _parse_custom_decimal(line: str):
    """
    Handles the board's actual decimal-degree format (firmware mislabels
    longitude as "LAT" too, so fields are identified by compass letter):
      LAT: 28.584838 N LAT: 77.315866 E Speed: 0.80 km/h
    """
    lat_m = re.search(r'(-?\d+\.\d+)\s*([NS])\b', line, re.IGNORECASE)
    lon_m = re.search(r'(-?\d+\.\d+)\s*([EW])\b', line, re.IGNORECASE)
    if not lat_m or not lon_m:
        return None

    lat = float(lat_m.group(1)) * (1 if lat_m.group(2).upper() == 'N' else -1)
    lng = float(lon_m.group(1)) * (1 if lon_m.group(2).upper() == 'E' else -1)

    spd_m = re.search(r'Speed:\s*(\d+(?:\.\d+)?)\s*km/h', line, re.IGNORECASE)
    speed_kmh = float(spd_m.group(1)) if spd_m else 0.0
    speed_cms = int(round(speed_kmh * 27.7778))

    now    = datetime.datetime.now()
    time_m = re.search(r'T-(\d{1,2}):(\d{2}):(\d{2})', line)
    date_m = re.search(r'D-(\d{1,2})-(\d{1,2})-(\d{4})', line)

    hh = int(time_m.group(1)) if time_m else now.hour
    mm_t = int(time_m.group(2)) if time_m else now.minute
    ss = int(time_m.group(3)) if time_m else now.second
    dd = int(date_m.group(1)) if date_m else now.day
    mo = int(date_m.group(2)) if date_m else now.month
    yr = int(date_m.group(3)) if date_m else now.year

    return dict(lat=lat, lng=lng, speed_cms=speed_cms,
                hh=hh, mm=mm_t, ss=ss, dd=dd, mo=mo, yr=yr)


# ── Line dispatcher ───────────────────────────────────────────────────────
def parse_line(raw: str):
    line = raw.strip()
    if not line:
        return None

    print(f"[RAW ] {line}")

    # Custom LAT/LON format (most likely for this board)
    if re.search(r'LAT:\d+[NS]', line, re.IGNORECASE):
        return _parse_custom(line)

    # Board decimal-degree format (e.g. "LAT: 28.584838 N LAT: 77.315866 E Speed: 0.80 km/h")
    if re.search(r'\d+\.\d+\s*[NS]\b', line, re.IGNORECASE) and re.search(r'\d+\.\d+\s*[EW]\b', line, re.IGNORECASE):
        return _parse_custom_decimal(line)

    # NMEA RMC (has speed + date — preferred)
    if line.startswith(('$GPRMC', '$GNRMC')):
        return _parse_gprmc(line)

    # NMEA GGA (position quality, no speed)
    if line.startswith(('$GPGGA', '$GNGGA')):
        return _parse_gpgga(line)

    # Other NMEA sentences we don't need — silently skip
    if line.startswith('$GP') or line.startswith('$GN'):
        return None

    # Completely unknown format — print so we can identify it
    print(f"[WARN] Unknown line format: {line!r}")
    return None


# ── Payload builder ───────────────────────────────────────────────────────
def build_payload(p: dict) -> str:
    """Serialise to the format server.js expects on adj/datalogger/sensors/gps."""
    lat_raw = int(abs(p['lat']) * 1e6)
    lon_raw = int(abs(p['lng']) * 1e6)
    lat_dir = 'N' if p['lat'] >= 0 else 'S'
    lon_dir = 'E' if p['lng'] >= 0 else 'W'
    return (
        f"GPS:T-{p['hh']:02d}:{p['mm']:02d}:{p['ss']:02d} "
        f"D-{p['dd']:02d}-{p['mo']:02d}-{p['yr']} "
        f"LAT:{lat_raw}{lat_dir} "
        f"LON:{lon_raw}{lon_dir} "
        f"SPD:{p['speed_cms']}cm/s"
    )


# ── Client handler ────────────────────────────────────────────────────────
def handle_client(conn, addr):
    print(f"[TCP ] GPS board connected from {addr[0]}:{addr[1]}")
    buf = b""
    last_pub = 0

    try:
        while True:
            chunk = conn.recv(1024)
            if not chunk:
                print(f"[TCP ] GPS board {addr[0]} disconnected (EOF)")
                break
            buf += chunk

            # Process complete lines (delimited by \n or \r\n)
            while b'\n' in buf:
                line_bytes, buf = buf.split(b'\n', 1)
                line = line_bytes.decode(errors='replace')
                parsed = parse_line(line)
                if parsed:
                    payload = build_payload(parsed)
                    ok = mqtt_publish(payload)
                    now = time.time()
                    # Rate-limit console spam to 1 line/sec
                    if now - last_pub >= 1.0:
                        print(f"[GPS ] lat={parsed['lat']:.6f} lng={parsed['lng']:.6f} "
                              f"spd={parsed['speed_cms']}cm/s  MQTT={'OK' if ok else 'FAIL'}")
                        last_pub = now
    except ConnectionResetError:
        print(f"[TCP ] GPS board {addr[0]} reset connection")
    except Exception as e:
        print(f"[TCP ] Error handling {addr[0]}: {e}")
    finally:
        conn.close()
        print(f"[TCP ] Connection from {addr[0]} closed")


# ── TCP server ────────────────────────────────────────────────────────────
def run_server():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        srv.bind((GPS_LISTEN_HOST, GPS_LISTEN_PORT))
    except OSError as e:
        print(f"[ERR ] Cannot bind to port {GPS_LISTEN_PORT}: {e}")
        print( "[ERR ] Is something else already on that port?  Try: ss -tlnp | grep 5001")
        sys.exit(1)

    srv.listen(2)
    print(f"[TCP ] Listening on {GPS_LISTEN_HOST}:{GPS_LISTEN_PORT}")
    print(f"[TCP ] Accepting connections only from {EXPECTED_GPS_IP}")
    print()

    while True:
        try:
            conn, addr = srv.accept()
            if addr[0] != EXPECTED_GPS_IP:
                print(f"[TCP ] Rejected {addr[0]} (not {EXPECTED_GPS_IP})")
                conn.close()
                continue
            # Thread per connection — board reconnects after signal loss
            t = threading.Thread(target=handle_client, args=(conn, addr), daemon=True)
            t.start()
        except KeyboardInterrupt:
            break
        except Exception as e:
            print(f"[TCP ] Server error: {e}")
            time.sleep(1)

    srv.close()


# ── Entry point ───────────────────────────────────────────────────────────
def main():
    print()
    print("╔══════════════════════════════════════════════════════╗")
    print("║        UABAMS  Ethernet GPS Receiver                 ║")
    print("╠══════════════════════════════════════════════════════╣")
    print(f"║  GPS board IP  : {EXPECTED_GPS_IP:<35}║")
    print(f"║  Listen port   : {GPS_LISTEN_PORT:<35}║")
    print(f"║  MQTT broker   : {MQTT_HOST}:{MQTT_PORT:<30}║")
    print(f"║  MQTT topic    : {MQTT_TOPIC:<35}║")
    print("╠══════════════════════════════════════════════════════╣")
    print("║  If GPS firmware is locked to port 5000, run:       ║")
    print("║    sudo iptables -t nat -A PREROUTING \\             ║")
    print("║      -s 192.168.1.200 -p tcp --dport 5000 \\        ║")
    print("║      -j REDIRECT --to-ports 5001                    ║")
    print("╚══════════════════════════════════════════════════════╝")
    print()

    signal.signal(signal.SIGINT,  lambda *_: (print("\n[GPS ] Interrupted, shutting down"), sys.exit(0)))
    signal.signal(signal.SIGTERM, lambda *_: (print("\n[GPS ] Terminated, shutting down"), sys.exit(0)))

    mqtt_setup()
    time.sleep(0.5)  # brief pause so MQTT connect message prints cleanly

    run_server()


if __name__ == "__main__":
    main()
