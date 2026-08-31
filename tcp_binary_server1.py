import socket
import threading
import sqlite3
import struct
import re
from datetime import datetime

HOST = "0.0.0.0"
PORT = 5000
DB_FILE = "sensor_data_binary.db"

DEVICE_MAP = {
    "192.168.1.200": "GPS-1",
    "192.168.1.201": "ACCEL-1",
    "192.168.1.203": "ACCEL-2",
    "192.168.1.204": "ACCEL-3",
    "192.168.1.205": "ACCEL-4",
    "192.168.1.211": "ODOMETER-1",   # Railway axle box encoder odometer
}

GPS_SYNC1 = 0xAA
GPS_SYNC2 = 0x55
GPS_PKT_SIZE = 14

ACCEL_SYNC1 = 0xAB
ACCEL_SYNC2 = 0x56
ACCEL_PKT_SIZE = 16

db_lock = threading.Lock()


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def init_db():
    conn = sqlite3.connect(DB_FILE)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS sensor_data (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT NOT NULL,
            device_name TEXT NOT NULL,
            ip_address TEXT NOT NULL,
            raw_data TEXT NOT NULL,
            lat REAL, lon REAL, speed_kmph REAL,
            x REAL, y REAL, z REAL,
            crc_ok INTEGER,
            enc_count INTEGER,
            enc_km INTEGER, enc_m INTEGER, enc_mm INTEGER,
            enc_speed_ms REAL, enc_speed_kmh REAL
        )
    """)
    conn.commit()
    conn.close()


def save_to_db(timestamp, device_name, ip, raw_data,
                lat=None, lon=None, speed=None,
                x=None, y=None, z=None, crc_ok=None,
                enc_count=None, enc_km=None, enc_m=None, enc_mm=None,
                enc_speed_ms=None, enc_speed_kmh=None):
    with db_lock:
        conn = sqlite3.connect(DB_FILE)
        conn.execute(
            """INSERT INTO sensor_data
               (timestamp, device_name, ip_address, raw_data, lat, lon, speed_kmph, x, y, z, crc_ok,
                enc_count, enc_km, enc_m, enc_mm, enc_speed_ms, enc_speed_kmh)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
            (timestamp, device_name, ip, raw_data, lat, lon, speed, x, y, z, crc_ok,
             enc_count, enc_km, enc_m, enc_mm, enc_speed_ms, enc_speed_kmh)
        )
        conn.commit()
        conn.close()


def parse_gps_packet(pkt: bytes):
    sync1, sync2, lat_raw, lon_raw, speed_raw, crc_recv = struct.unpack('>BBiiHH', pkt)
    payload = pkt[2:12]
    crc_ok = (crc16_ccitt(payload) == crc_recv)
    return lat_raw / 1_000_000, lon_raw / 1_000_000, speed_raw / 100, crc_ok


def parse_accel_packet(pkt: bytes):
    sync1, sync2, x_raw, y_raw, z_raw, crc_recv = struct.unpack('>BBiiiH', pkt)
    payload = pkt[2:14]
    crc_ok = (crc16_ccitt(payload) == crc_recv)
    return x_raw / 1_000_000, y_raw / 1_000_000, z_raw / 1_000_000, crc_ok


def parse_odometer_line(line: str):
    """
    Parses a line like:
    ENCODER,COUNT=5524,KM=0,METER=342,MM=247,SPEED_MS=0.079,SPEED_KMH=0.29
    Returns a dict of the parsed fields, or None if the line doesn't match.
    """
    if not line.startswith("ENCODER,"):
        return None

    fields = {}
    for part in line.strip().split(","):
        if "=" in part:
            key, val = part.split("=", 1)
            fields[key.strip()] = val.strip()

    try:
        return {
            "count":       int(fields["COUNT"]),
            "km":          int(fields["KM"]),
            "meter":       int(fields["METER"]),
            "mm":          int(fields["MM"]),
            "speed_ms":    float(fields["SPEED_MS"]),
            "speed_kmh":   float(fields["SPEED_KMH"]),
        }
    except (KeyError, ValueError):
        return None


def handle_binary_stream(conn, ip, device_name, sync1, sync2, pkt_size, parser_func, kind):
    buf = bytearray()
    while True:
        try:
            data = conn.recv(1024)
        except ConnectionResetError:
            break
        if not data:
            break
        buf.extend(data)

        while True:
            sync_idx = buf.find(bytes([sync1, sync2]))
            if sync_idx == -1:
                if len(buf) > 1:
                    del buf[:-1]
                break

            if sync_idx > 0:
                print(f"[{device_name}] {sync_idx} garbage bytes discarded before sync")
                del buf[:sync_idx]

            if len(buf) < pkt_size:
                break

            pkt = bytes(buf[:pkt_size])
            del buf[:pkt_size]

            ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]

            if kind == "gps":
                lat, lon, speed, crc_ok = parser_func(pkt)
                if crc_ok:
                    print(f"[{ts}] {device_name} ({ip}) | LAT:{lat:.6f} LON:{lon:.6f} SPEED:{speed:.2f}km/h")
                else:
                    print(f"[{ts}] {device_name} ({ip}) | *** CRC FAIL *** raw={pkt.hex()}")
                save_to_db(ts, device_name, ip, pkt.hex(), lat=lat, lon=lon, speed=speed, crc_ok=int(crc_ok))
            else:  # accel
                x, y, z, crc_ok = parser_func(pkt)
                if crc_ok:
                    print(f"[{ts}] {device_name} ({ip}) | X:{x:.3f} Y:{y:.3f} Z:{z:.3f}")
                else:
                    print(f"[{ts}] {device_name} ({ip}) | *** CRC FAIL *** raw={pkt.hex()}")
                save_to_db(ts, device_name, ip, pkt.hex(), x=x, y=y, z=z, crc_ok=int(crc_ok))


def handle_odometer_stream(conn, ip, device_name):
    """
    Odometer sends plain ASCII CSV lines terminated by \\r\\n (not binary
    packets), so this reads line-by-line instead of using sync-byte framing.
    """
    buf = ""
    while True:
        try:
            data = conn.recv(1024)
        except ConnectionResetError:
            break
        if not data:
            break

        buf += data.decode("ascii", errors="ignore")

        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = line.strip()
            if not line:
                continue

            ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
            parsed = parse_odometer_line(line)

            if parsed:
                print(f"[{ts}] {device_name} ({ip}) | "
                      f"Count:{parsed['count']} "
                      f"Dist:{parsed['km']}KM {parsed['meter']}M {parsed['mm']}MM "
                      f"Speed:{parsed['speed_ms']:.3f}m/s {parsed['speed_kmh']:.2f}km/h")
                save_to_db(ts, device_name, ip, line,
                           enc_count=parsed["count"],
                           enc_km=parsed["km"], enc_m=parsed["meter"], enc_mm=parsed["mm"],
                           enc_speed_ms=parsed["speed_ms"], enc_speed_kmh=parsed["speed_kmh"],
                           crc_ok=1)
            else:
                print(f"[{ts}] {device_name} ({ip}) | *** UNPARSEABLE LINE *** raw={line}")
                save_to_db(ts, device_name, ip, line, crc_ok=0)


def handle_client(conn, addr):
    ip = addr[0]
    device_name = DEVICE_MAP.get(ip, f"UNKNOWN({ip})")
    print(f"[CONNECTED] {device_name} ({ip})")

    if device_name.startswith("GPS"):
        handle_binary_stream(conn, ip, device_name, GPS_SYNC1, GPS_SYNC2, GPS_PKT_SIZE, parse_gps_packet, "gps")
    elif device_name.startswith("ACCEL"):
        handle_binary_stream(conn, ip, device_name, ACCEL_SYNC1, ACCEL_SYNC2, ACCEL_PKT_SIZE, parse_accel_packet, "accel")
    elif device_name.startswith("ODOMETER"):
        handle_odometer_stream(conn, ip, device_name)
    else:
        print(f"[{device_name}] unknown device type, ignoring")

    print(f"[DISCONNECTED] {device_name} ({ip})")
    conn.close()


def main():
    init_db()
    print(f"Database ready: {DB_FILE}")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, PORT))
    s.listen(10)
    print(f"Listening on port {PORT}... waiting for devices to connect")
    print(f"Known devices: {list(DEVICE_MAP.values())}\n")

    while True:
        conn, addr = s.accept()
        t = threading.Thread(target=handle_client, args=(conn, addr), daemon=True)
        t.start()


if __name__ == "__main__":
    main()
