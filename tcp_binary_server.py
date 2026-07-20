import socket
import threading
import sqlite3
import struct
from datetime import datetime

HOST = "0.0.0.0"
PORT = 5000
DB_FILE = "sensor_data_binary.db"

DEVICE_MAP = {
    "192.168.1.200": "GPS-1",
    "192.168.1.202": "ACCEL-1",
    "192.168.1.203": "ACCEL-2",
    "192.168.1.204": "ACCEL-3",
    "192.168.1.205": "ACCEL-4",
}

# GPS binary packet framing
GPS_SYNC1 = 0xAA
GPS_SYNC2 = 0x55
GPS_PKT_SIZE = 14   # 2 sync + 4 lat + 4 lon + 2 speed + 2 crc

db_lock = threading.Lock()


def crc16_ccitt(data: bytes) -> int:
    """STM32 side ke crc16_ccitt() se exact match hona chahiye (poly 0x1021, init 0xFFFF)"""
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
            lat REAL,
            lon REAL,
            speed_kmph REAL,
            crc_ok INTEGER
        )
    """)
    conn.commit()
    conn.close()


def save_to_db(timestamp, device_name, ip, raw_data, lat=None, lon=None, speed=None, crc_ok=None):
    with db_lock:
        conn = sqlite3.connect(DB_FILE)
        conn.execute(
            """INSERT INTO sensor_data
               (timestamp, device_name, ip_address, raw_data, lat, lon, speed_kmph, crc_ok)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?)""",
            (timestamp, device_name, ip, raw_data, lat, lon, speed, crc_ok)
        )
        conn.commit()
        conn.close()


def parse_gps_packet(pkt: bytes):
    """14-byte binary packet -> (lat, lon, speed, crc_ok). pkt me sync bytes shamil nahi honi chahiye
       (caller already strip kar chuka hai) — yahan hum poora packet (sync sahit) lete hain."""
    sync1, sync2, lat_raw, lon_raw, speed_raw, crc_recv = struct.unpack('>BBiiHH', pkt)
    payload = pkt[2:12]   # sirf lat+lon+speed, sync/crc exclude
    crc_calc = crc16_ccitt(payload)
    crc_ok = (crc_calc == crc_recv)

    lat = lat_raw / 1_000_000
    lon = lon_raw / 1_000_000
    speed = speed_raw / 100
    return lat, lon, speed, crc_ok


def handle_gps_binary(conn, ip, device_name):
    """GPS device ke liye — sync-byte framed binary packets padhta hai"""
    buf = bytearray()
    while True:
        try:
            data = conn.recv(1024)
        except ConnectionResetError:
            break
        if not data:
            break
        buf.extend(data)

        # Buffer me jitne complete packets mil sakte hain sab process karo
        while True:
            # Sync pattern dhoondo
            sync_idx = buf.find(bytes([GPS_SYNC1, GPS_SYNC2]))
            if sync_idx == -1:
                # Koi sync nahi mila — garbage/partial data, sirf last byte rakho
                # (agar last byte hi SYNC1 ho sakta hai, agla data usse jud sakta hai)
                if len(buf) > 1:
                    del buf[:-1]
                break

            # Sync se pehle ka garbage discard karo
            if sync_idx > 0:
                print(f"[{device_name}] {sync_idx} garbage bytes discarded before sync")
                del buf[:sync_idx]

            if len(buf) < GPS_PKT_SIZE:
                break   # poora packet abhi nahi aaya, aur data ka wait karo

            pkt = bytes(buf[:GPS_PKT_SIZE])
            del buf[:GPS_PKT_SIZE]

            lat, lon, speed, crc_ok = parse_gps_packet(pkt)
            ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]

            if crc_ok:
              print(f"[{ts}] {device_name} ({ip}) | LAT:{lat:.6f} LON:{lon:.6f} SPEED:{speed:.2f}km/h | raw={pkt.hex()}")
            else:
                print(f"[{ts}] {device_name} ({ip}) | *** CRC FAIL (EMI corruption?) *** raw={pkt.hex()}")

            save_to_db(ts, device_name, ip, pkt.hex(), lat, lon, speed, int(crc_ok))


def handle_client_text(conn, ip, device_name):
    """Baaki (text-based) devices ke liye — purana line-based logic"""
    buffer = ""
    while True:
        try:
            data = conn.recv(1024)
        except ConnectionResetError:
            break
        if not data:
            break

        buffer += data.decode(errors='replace')

        while "\n" in buffer:
            line, buffer = buffer.split("\n", 1)
            line = line.strip()
            if not line:
                continue

            ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
            print(f"[{ts}] {device_name} ({ip}) | {line}")
            save_to_db(ts, device_name, ip, line)


def handle_client(conn, addr):
    ip = addr[0]
    device_name = DEVICE_MAP.get(ip, f"UNKNOWN({ip})")
    print(f"[CONNECTED] {device_name} ({ip})")

    if device_name.startswith("GPS"):
        handle_gps_binary(conn, ip, device_name)
    else:
        handle_client_text(conn, ip, device_name)

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