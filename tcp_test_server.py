import socket
from datetime import datetime

HOST = "0.0.0.0"
PORT = 5000

EXPECTED_GPS_IP = "192.168.1.200"   # apne fixed GPS board IP se match karo

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((HOST, PORT))
s.listen(1)
print(f"Listening on port {PORT}... waiting for GPS board ({EXPECTED_GPS_IP}) to connect")

while True:
    conn, addr = s.accept()

    if addr[0] != EXPECTED_GPS_IP:
        print(f"[REJECTED] Unknown device tried to connect: {addr}")
        conn.close()
        continue

    print(f"[CONNECTED] GPS board confirmed: {addr}")

    while True:
        try:
            data = conn.recv(1024)
        except ConnectionResetError:
            break
        if not data:
            print("[DISCONNECTED] GPS board connection closed")
            break

        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        line = data.decode(errors='replace').strip()
        print(f"[{ts}] {line}")

    conn.close()
    print("Waiting for GPS board to reconnect...")