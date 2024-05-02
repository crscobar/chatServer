import socket
import time

HOST = 'localhost'    # The local host
PORT = 5000    # The same port as used by the server
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.connect((HOST, PORT))
    s.sendall(b'GET /index.ht')
    print("sent 'GET /index.ht'")
    time.sleep(3)
    s.sendall(b'mlf HTTP/1.1\r\n\r\n')
    print("sent 2")
    data = s.recv(8192)
print('Received', repr(data))
