"""Get the ceiling lamp's miio token from Xiaomi cloud via QR login (scan with the Mi Home app).

Run: python xiaomi_qr_token.py [server]     (default server: de)
1. Writes the login QR to xiaomi_login_qr.png next to this script and waits (long poll).
2. After the scan, lists devices and writes the token of yeelink.light.ceiling3 / 192.168.0.20
   to token.txt. The token itself is never printed; the cloud session is kept in memory only.
Flow follows PiotrMachowski/Xiaomi-cloud-tokens-extractor (QR connector + RC4-encrypted API).
"""
import base64
import hashlib
import json
import os
import sys
import time

import qrcode
import requests

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = sys.argv[1] if len(sys.argv) > 1 else 'de'
LAMP_IP, LAMP_MODEL = '192.168.0.20', 'yeelink.light.ceiling3'
AGENT = 'Android-7.1.1-1.0.0-ONEPLUS A3010-136-ABCDEFGHIJKLMN APP/xiaomi.smarthome APPV/62830'


def js(text):
    return json.loads(text.replace('&&&START&&&', ''))


def rc4(key, data):
    s, j = list(range(256)), 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xFF
        s[i], s[j] = s[j], s[i]
    i = j = 0
    out = bytearray()
    for n in range(1024 + len(data)):  # Xiaomi drops the first 1024 keystream bytes
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        if n >= 1024:
            out.append(data[n - 1024] ^ s[(s[i] + s[j]) & 0xFF])
    return bytes(out)


def login(session):
    r = session.get('https://account.xiaomi.com/longPolling/loginUrl', params={
        '_qrsize': '480', 'qs': '%3Fsid%3Dxiaomiio%26_json%3Dtrue', 'callback': 'https://sts.api.io.mi.com/sts',
        '_hasLogo': 'false', 'sid': 'xiaomiio', 'serviceParam': '', '_locale': 'en_GB',
        '_dc': str(int(time.time() * 1000))})
    d = js(r.text)
    png = os.path.join(HERE, 'xiaomi_login_qr.png')
    qrcode.make(d['loginUrl']).save(png)
    print(f'QR ready: {png}  (valid ~{d.get("timeout", 300)}s) -- scan it in the Mi Home app', flush=True)
    r = session.get(d['lp'], timeout=int(d.get('timeout', 300)) + 30)
    d = js(r.text)
    if 'ssecurity' not in d:
        raise SystemExit(f'login not completed: {d.get("desc") or d.get("code")}')
    r = session.get(d['location'], headers={'content-type': 'application/x-www-form-urlencoded'})
    token = r.cookies.get('serviceToken') or session.cookies.get('serviceToken')
    if not token:
        raise SystemExit('no serviceToken after login')
    print('login OK', flush=True)
    return str(d['userId']), d['ssecurity'], token


def api(session, user_id, ssecurity, service_token, path, params, server):
    url = f'https://{"" if server == "cn" else server + "."}api.io.mi.com/app{path}'
    nonce = base64.b64encode(os.urandom(8) + (int(time.time() * 1000) // 60000).to_bytes(4, 'big')).decode()
    signed = base64.b64encode(hashlib.sha256(base64.b64decode(ssecurity) + base64.b64decode(nonce)).digest()).decode()
    key = base64.b64decode(signed)

    def sign(p):
        parts = ['POST', url.split('com', 1)[1].replace('/app/', '/')] + [f'{k}={v}' for k, v in p.items()] + [signed]
        return base64.b64encode(hashlib.sha1('&'.join(parts).encode()).digest()).decode()

    p = dict(params)
    p['rc4_hash__'] = sign(p)
    p = {k: base64.b64encode(rc4(key, v.encode())).decode() for k, v in p.items()}
    p.update({'signature': sign(p), 'ssecurity': ssecurity, '_nonce': nonce})
    r = session.post(url, params=p, headers={
        'Accept-Encoding': 'identity', 'Content-Type': 'application/x-www-form-urlencoded',
        'x-xiaomi-protocal-flag-cli': 'PROTOCAL-HTTP2', 'MIOT-ENCRYPT-ALGORITHM': 'ENCRYPT-RC4'},
        cookies={'userId': user_id, 'yetAnotherServiceToken': service_token, 'serviceToken': service_token,
                 'locale': 'en_GB', 'timezone': 'GMT+01:00', 'is_daylight': '0', 'dst_offset': '0',
                 'channel': 'MI_APP_STORE'})
    return json.loads(rc4(key, base64.b64decode(r.text)).decode())


def main():
    s = requests.Session()
    s.headers['User-Agent'] = AGENT
    user_id, ssecurity, service_token = login(s)
    match = []
    for server in [SERVER] + [x for x in ('cn', 'de', 'us', 'ru', 'tw', 'sg', 'in', 'i2') if x != SERVER]:
        try:
            res = api(s, user_id, ssecurity, service_token, '/home/device_list',
                      {'data': '{"getVirtualModel":false,"getHuamiDevices":0}'}, server)
        except (ValueError, requests.RequestException) as e:
            print(f'server "{server}": error {type(e).__name__}', flush=True)
            continue
        devices = (res.get('result') or {}).get('list') or []
        print(f'{len(devices)} devices on server "{server}":', flush=True)
        for dv in devices:
            print(f'  {dv.get("model")}  {dv.get("localip")}  {dv.get("name")}')
        match = [dv for dv in devices if dv.get('model') == LAMP_MODEL and dv.get('localip') == LAMP_IP] or \
                [dv for dv in devices if dv.get('model') == LAMP_MODEL]
        if match:
            break
    if not match:
        raise SystemExit('lamp not found on any server for this account')
    with open(os.path.join(HERE, 'token.txt'), 'w') as f:
        f.write(match[0]['token'])
    print(f'token for {match[0].get("name")} ({match[0].get("localip")}) written to token.txt', flush=True)


if __name__ == '__main__':
    main()
