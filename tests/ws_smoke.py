import base64, hashlib, os, socket, struct
key=base64.b64encode(os.urandom(16)).decode()
s=socket.create_connection(('127.0.0.1',27183),2)
req=('GET / HTTP/1.1\r\nHost: 127.0.0.1:27183\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: '+key+'\r\nSec-WebSocket-Version: 13\r\n\r\n').encode()
s.sendall(req)
r=b''
while not r.endswith(b'\r\n\r\n'): r+=s.recv(1)
assert b'101 Switching Protocols' in r
assert base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode().encode() in r
msg=b'ping'; mask=os.urandom(4); payload=bytes(x^mask[i%4] for i,x in enumerate(msg)); s.sendall(bytes([0x81,0x80|len(msg)])+mask+payload)
h=s.recv(2); assert h[0]==0x81 and h[1]==4
assert s.recv(4)==b'pong'
msg=b'tap 720 1584 20'; mask=os.urandom(4); payload=bytes(x^mask[i%4] for i,x in enumerate(msg)); s.sendall(bytes([0x81,0x80|len(msg)])+mask+payload)
h=s.recv(2); n=h[1]&127; assert n==2 and s.recv(n)==b'ok'
s.close(); print('websocket_smoke_ok')
