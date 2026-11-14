"""Minimal miio client for the ceiling3 lamp: read info / flash a local OTA image.

  python miio_ota.py info                       # miIO.info (token masked)
  python miio_ota.py check                      # is this the lamp the images were made for?
  python miio_ota.py state                      # miIO.get_ota_state + progress
  python miio_ota.py flash <image.bin> --yes    # serve image over HTTP and send miIO.ota

flash refuses any device whose miIO.info does not match EXPECTED below (model, firmware
version, chip, Wi-Fi core firmware). --force skips that check: only for a lamp you can
recover over UART.
Token: 32 hex chars in token.txt next to this script (never passed on the command line).
Lamp IP: --ip=192.168.0.20 (default). The image must pass patch_ota.parse() (all container
checks) before it is offered to the lamp. Needs the `cryptography` package (AES-128-CBC).
"""
import hashlib
import http.server
import json
import os
import re
import socket
import struct
import sys
import threading
import time

from cryptography.hazmat.primitives import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

HERE = os.path.dirname(os.path.abspath(__file__))
ARGS = [a for a in sys.argv[1:] if not a.startswith('--')]
OPTS = dict((a[2:].split('=', 1) + [''])[:2] for a in sys.argv[1:] if a.startswith('--'))
IP = OPTS.get('ip') or '192.168.0.20'
PORT = 54321


class Miio:
    def __init__(self, ip, token_hex):
        self.ip, self.token = ip, bytes.fromhex(token_hex)
        assert len(self.token) == 16, 'token must be 32 hex chars'
        self.key = hashlib.md5(self.token).digest()
        self.iv = hashlib.md5(self.key + self.token).digest()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(5)
        self.msg_id = int(time.time()) % 10000
        self.hello()

    def hello(self):
        self.sock.sendto(bytes.fromhex('21310020' + 'ff' * 28), (self.ip, PORT))
        r = self.sock.recv(1024)
        self.did, stamp = struct.unpack('>II', r[8:16])
        self.stamp_at = (stamp, time.time())

    def _crypt(self, data, enc):
        c = Cipher(algorithms.AES(self.key), modes.CBC(self.iv))
        if enc:
            p = padding.PKCS7(128).padder()
            e = c.encryptor()
            return e.update(p.update(data) + p.finalize()) + e.finalize()
        d = c.decryptor()
        u = padding.PKCS7(128).unpadder()
        return u.update(d.update(data) + d.finalize()) + u.finalize()

    def call(self, method, params=None, retries=3):
        for attempt in range(retries):
            self.msg_id += 1
            body = json.dumps({'id': self.msg_id, 'method': method, 'params': params if params is not None else []})
            payload = self._crypt(body.encode(), True)
            stamp = self.stamp_at[0] + int(time.time() - self.stamp_at[1]) + 1
            head = struct.pack('>HHIII', 0x2131, 32 + len(payload), 0, self.did, stamp)
            pkt = head + hashlib.md5(head + self.token + payload).digest() + payload
            self.sock.sendto(pkt, (self.ip, PORT))
            try:
                while True:
                    r = self.sock.recv(4096)
                    if len(r) <= 32:
                        continue
                    if hashlib.md5(r[:16] + self.token + r[32:]).digest() != r[16:32]:
                        raise ValueError('bad checksum in reply (wrong token?)')
                    reply = json.loads(self._crypt(r[32:], False).rstrip(b'\0').decode())
                    if reply.get('id') == self.msg_id:
                        return reply
            except socket.timeout:
                self.hello()
        raise TimeoutError(f'no reply to {method}')


def token():
    path = os.path.join(HERE, 'token.txt')
    if not os.path.exists(path):
        raise SystemExit('put the 32-hex-char miio token into token.txt next to this script')
    return open(path).read().strip()


def local_ip_for(ip):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((ip, 9))  # no packet is sent; just picks the outgoing interface
    return s.getsockname()[0]


def say(msg):
    """Print and append to flash.log so a run that outlives the terminal can still be read."""
    line = f'{time.strftime("%H:%M:%S")} {msg}'
    print(line, flush=True)
    with open(os.path.join(HERE, 'flash.log'), 'a', encoding='utf-8') as f:
        f.write(line + '\n')


def serve(path):
    data = open(path, 'rb').read()
    name = '/' + hashlib.md5(data).hexdigest() + '.bin'
    hits = []  # peers whose download was confirmed complete (client closed after the last byte)

    class H(http.server.BaseHTTPRequestHandler):
        # httpc_resp_head_recving (0x100ffbc4) requires exact "HTTP/1.1 200 OK" or
        # "HTTP/1.1 206 Partial Content" prefix; HTTP/1.0 is rejected immediately.
        protocol_version = 'HTTP/1.1'

        def do_GET(self):
            peer = self.client_address[0]
            rng = self.headers.get('Range')
            say(f'[http] {peer}:{self.client_address[1]} GET {self.path} '
                f'Range={rng!r} UA={self.headers.get("User-Agent")!r} Host={self.headers.get("Host")!r}')
            if self.path != name:
                self.close_connection = True
                self.send_error(404)
                return
            m = re.match(r'^bytes=(\d+)-(\d*)$', rng.strip()) if rng else None
            if m:
                start = int(m.group(1))
                end = int(m.group(2)) if m.group(2) else len(data) - 1
                end = min(end, len(data) - 1)
                if start > end or start >= len(data):
                    self.close_connection = True
                    self.send_error(416)
                    return
                chunk = data[start:end + 1]
                self.send_response(206, 'Partial Content')
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Range', f'bytes {start}-{end}/{len(data)}')
                self.send_header('Content-Length', str(len(chunk)))
                self.end_headers()
                try:
                    self.wfile.write(chunk)
                    self.wfile.flush()
                    if end == len(data) - 1:
                        say(f'[http] {peer} served final range {start}-{end}/{len(data)}')
                        hits.append(peer)
                except OSError as e:
                    self.close_connection = True
                    say(f'[http] {peer} range transfer INTERRUPTED: {type(e).__name__} {e}')
                return
            self.close_connection = True
            self.send_response(200, 'OK')
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Connection', 'close')
            self.end_headers()
            t_req = time.time()
            try:
                self.wfile.write(data)
                self.wfile.flush()
                # write() returns once bytes are in the OS buffer; the slow embedded client may still be
                # reading. Wait for the client to close its side before claiming success.
                self.connection.shutdown(socket.SHUT_WR)
                self.connection.settimeout(180)
                while self.connection.recv(4096):
                    pass
                say(f'[http] {peer} received all {len(data)} bytes in {time.time() - t_req:.1f}s (client closed the connection)')
                hits.append(peer)
            except OSError as e:
                say(f'[http] {peer} transfer INTERRUPTED after {time.time() - t_req:.1f}s: {type(e).__name__} {e}')

        def log_message(self, *a):
            pass

    # The lamp's httpc_req (0x10100750) does not parse ":port" out of the URL: the port comes from a
    # separate request field that the OTA code always leaves 0, i.e. 80 for http://. A URL with
    # ":8321" ends up as the host name "192.168.0.100:8321", fails DNS and no connection is made.
    port = int(OPTS.get('port') or 80)
    if port != 80:
        say(f'WARNING: port {port} -- the lamp can only download from port 80')
    srv = http.server.ThreadingHTTPServer(('0.0.0.0', port), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    host = local_ip_for(IP)
    url = f'http://{host}{name}' if port == 80 else f'http://{host}:{port}{name}'
    return srv, url, hashlib.md5(data).hexdigest(), hits


# What miIO.info reports on the lamp these images were built for and tested on (YLXD17YL).
# The images replace the whole application and Wi-Fi core firmware of version 2.0.2_0048;
# every address they patch is specific to that build.
EXPECTED = {
    'model': 'yeelink.light.ceiling3',
    'fw_ver': '2.0.2_0048',
    'hw_ver': 'MTK7697',
    'wifi_fw_ver': '20190110183129',
}


def mismatches(info):
    """[(field, reported, expected)] for every field of miIO.info that differs from EXPECTED."""
    return [(k, info.get(k), v) for k, v in EXPECTED.items() if str(info.get(k)) != v]


def check():
    info = Miio(IP, token()).call('miIO.info').get('result', {})
    for k, v in EXPECTED.items():
        ok = str(info.get(k)) == v
        print(f'  {"OK      " if ok else "MISMATCH"} {k}: {info.get(k)!r}' + ('' if ok else f' (expected {v!r})'))
    bad = mismatches(info)
    print('this lamp matches the tested configuration' if not bad else
          'this is NOT the configuration the images were made for: do not flash')
    return not bad


def flash(path):
    sys.path.insert(0, HERE)
    import patch_ota  # reuse the container validator
    data = open(path, 'rb').read()
    patch_ota.parse(data)  # raises on any header/SHA-1/CRC/LZMA problem
    say(f'image OK: {os.path.basename(path)} {len(data)} bytes md5 {hashlib.md5(data).hexdigest()}')
    dev = Miio(IP, token())
    info = dev.call('miIO.info').get('result', {})
    life0 = info.get('life') or 0
    say(f'lamp: model={info.get("model")} fw_ver={info.get("fw_ver")} hw_ver={info.get("hw_ver")} '
        f'wifi_fw_ver={info.get("wifi_fw_ver")} life={life0}')
    bad = mismatches(info)
    if bad:
        why = '; '.join(f'{k} is {got!r}, expected {want!r}' for k, got, want in bad)
        if 'force' not in OPTS:
            raise SystemExit(f'refusing to flash: {why}. This is not the lamp/firmware the image was made for.')
        say(f'WARNING: --force given, flashing despite: {why}')
    srv, url, md5, hits = serve(path)
    say(f'serving {url}')
    if 'yes' not in OPTS:
        input('press Enter to send miIO.ota to the lamp (Ctrl+C to abort) ')
    params = {'mode': 'normal', 'install': '1', 'app_url': url, 'file_md5': md5, 'proc': 'dnld install'}
    if 'cloud' in OPTS:
        # do_ota (0x100f8304) drops miIO.ota that did not arrive via the cloud ("ota is from local,
        # dropped" -> -30020), so relay the same command through Xiaomi's /home/rpc/<did>.
        # The lamp still downloads the image from this PC over the LAN.
        import requests
        import xiaomi_qr_token as xq
        s = requests.Session()
        s.headers['User-Agent'] = xq.AGENT
        user_id, ssec, stok = xq.login(s)
        server = OPTS['cloud'] or 'de'
        lst = xq.api(s, user_id, ssec, stok, '/home/device_list',
                     {'data': '{"getVirtualModel":false,"getHuamiDevices":0}'}, server)
        devices = (lst.get('result') or {}).get('list') or []
        match = [d for d in devices if d.get('model') == 'yeelink.light.ceiling3' and d.get('localip') == IP] or \
                [d for d in devices if d.get('model') == 'yeelink.light.ceiling3']
        if not match:
            raise SystemExit('ceiling3 device not found in Xiaomi Cloud device_list')
        did = match[0]['did']
        r = xq.api(s, user_id, ssec, stok, f'/home/rpc/{did}',
                   {'data': json.dumps({'method': 'miIO.ota', 'params': params})}, server)
    else:
        r = dev.call('miIO.ota', params)
    say(f'miIO.ota -> {r}')
    if r.get('result') not in (['ok'], 'ok'):
        raise SystemExit('lamp did not accept the OTA request')
    # Watch until the lamp actually reboots (uptime drops) or 10 minutes pass.
    # A single dropped UDP packet while idle is NOT a reboot; require life < life0 - 5.
    t0, last, rebooted, info = time.time(), None, False, {}
    while time.time() - t0 < 600:
        time.sleep(3)
        try:
            st = dev.call('miIO.get_ota_state', retries=1).get('result')
            pr = dev.call('miIO.get_ota_progress', retries=1).get('result')
            info = dev.call('miIO.info', retries=1).get('result') or {}
            life = info.get('life') or 0
            cur = (st, pr)
            if life < life0 - 5:
                rebooted = True
            else:
                life0 = max(life0, life)
        except (TimeoutError, OSError, ValueError) as e:
            cur = ('no reply', type(e).__name__)  # expected while it reboots or on a dropped UDP packet
        if cur != last:
            say(f'  {int(time.time() - t0):4d}s state={cur[0]} progress={cur[1]} downloaded_by={sorted(set(hits))}')
            last = cur
        if rebooted:
            break
    srv.shutdown()
    if not rebooted:
        say(f'NO REBOOT within 10 min. download confirmed for: {sorted(set(hits)) or "nobody"}; '
            'the lamp is still on the old firmware (nothing was installed).')
        return
    say(f'lamp rebooted; back online: fw_ver={info.get("fw_ver")} life={info.get("life")}')


def main():
    cmd = ARGS[0] if ARGS else 'info'
    if cmd == 'info':
        reply = Miio(IP, token()).call('miIO.info')
        if isinstance(reply.get('result'), dict) and 'token' in reply['result']:
            reply['result']['token'] = '<hidden>'           # the device token is a secret
        print(json.dumps(reply, indent=1))
    elif cmd == 'check':
        sys.exit(0 if check() else 1)
    elif cmd == 'state':
        dev = Miio(IP, token())
        print(dev.call('miIO.get_ota_state'), dev.call('miIO.get_ota_progress'))
    elif cmd == 'flash' and len(ARGS) > 1:
        flash(ARGS[1])
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
