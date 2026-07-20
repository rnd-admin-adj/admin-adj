import socket
import threading
import sqlite3
from datetime import datetime

HOST = "0.0.0.0"
PORT = 5000
DB_FILE = "sensor_data.db"

DEVICE_MAP = {
    "192.168.1.200": "GPS-1",
    "192.168.1.202": "ACCEL-1",
    "192.168.1.203": "ACCEL-2",
    "192.168.1.204": "ACCEL-3",
    "192.168.1.205": "ACCEL-4",
}

db_lock = threading.Lock()


def init_db():
    conn = sqlite3.connect(DB_FILE)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS sensor_data (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT NOT NULL,
            device_name TEXT NOT NULL,
            ip_address TEXT NOT NULL,
            raw_data TEXT NOT NULL
        )
    """)
    conn.commit()
    conn.close()


def save_to_db(timestamp, device_name, ip, raw_data):
    with db_lock:
        conn = sqlite3.connect(DB_FILE)
        conn.execute(
            "INSERT INTO sensor_data (timestamp, device_name, ip_address, raw_data) VALUES (?, ?, ?, ?)",
            (timestamp, device_name, ip, raw_data)
        )
        conn.commit()
        conn.close()


def handle_client(conn, addr):
    ip = addr[0]
    device_name = DEVICE_MAP.get(ip, f"UNKNOWN({ip})")

    print(f"[CONNECTED] {device_name} ({ip})")

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
