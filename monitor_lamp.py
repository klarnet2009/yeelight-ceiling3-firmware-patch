"""Watch the ceiling lamp and record HOW it drops off the network.

Run: python monitor_lamp.py [ip] [interval_s]     (default 192.168.0.20, 30 s)
Log: monitor_lamp.log next to this script; a line is written on every state change
and a heartbeat every 30 minutes. Stop with Ctrl+C. --once runs a single probe and exits.

Four read-only probes, from lowest to highest layer:
  arp   - the lamp answers ARP for its IP (Wi-Fi associated AND IP configured)
  ping  - ICMP echo reply (lwIP alive)
  miio  - UDP 54321 hello reply (miio SDK task alive)
  lan   - TCP 55443 get_prop reply (Yeelight app task alive)
Reading: all down -> no association / no IP (the OTA patch covers this);
arp up but the rest down -> IP kept, stack/firmware stuck (patch does NOT cover this).
"""
import datetime
import json
import os
import re
import socket
import subprocess
import sys
import time

IP = next((a for a in sys.argv[1:] if a.count('.') == 3), '192.168.0.20')
NUMS = [a for a in sys.argv[1:] if a.isdigit()]
INTERVAL = int(NUMS[0]) if NUMS else 30
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'monitor_lamp.log')
NOWIN = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0


def ping():
    for _ in range(2):
        r = subprocess.run(['ping', '-n', '1', '-w', '1500', IP], capture_output=True, text=True,
                           creationflags=NOWIN)
        if 'TTL=' in r.stdout.upper():  # Windows prints "Reply from <ip>: ... TTL=" only on success
            return True
    return False


def arp():
    # `arp -a` keeps showing stale entries for minutes; the neighbor cache state does not lie.
    # Called last in probe(), seconds after ping, so Windows has finished re-resolving the IP:
    # 'Reachable' (or localized equivalent) means the lamp answered ARP.
    r = subprocess.run(['netsh', 'interface', 'ipv4', 'show', 'neighbors'], capture_output=True,
                       text=True, creationflags=NOWIN)
    m = re.search(r'^' + re.escape(IP) + r'\s+([0-9a-f-]{17})\s+([^\r\n]+)', r.stdout, re.I | re.M)
    return bool(m) and m.group(2).strip().lower() in ('reachable', 'достижимо', 'доступен')


def miio():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(2)
    try:
        s.sendto(bytes.fromhex('21310020' + 'ff' * 28), (IP, 54321))
        data = s.recv(64)
        return len(data) == 32 and data[:2] == b'\x21\x31'
    except OSError:
        return False
    finally:
        s.close()


def lan():
    try:
        with socket.create_connection((IP, 55443), 3) as s:
            s.settimeout(3)
            s.sendall(b'{"id":1,"method":"get_prop","params":["power","bright"]}\r\n')
            reply = json.loads(s.recv(512).decode().splitlines()[0])
            return 'result' in reply, reply.get('result')
    except (OSError, ValueError, IndexError):
        return False, None


def probe():
    p = ping()
    m = miio()
    l, props = lan()
    a = arp()  # last: gives the OS time to re-resolve after ping
    return {'arp': a, 'ping': p, 'miio': m, 'lan': l}, props


def verdict(st):
    if all(st.values()):
        return 'OK'
    if not any(st.values()):
        return 'OFFLINE: no association/IP (patch covers this)'
    if st['arp'] and not (st['ping'] or st['miio'] or st['lan']):
        return 'ZOMBIE: answers ARP, IP stack silent (patch may NOT cover this)'
    if st['ping'] and not (st['miio'] or st['lan']):
        return 'ZOMBIE: pings, but miio and LAN tasks dead (patch does NOT cover this)'
    return 'PARTIAL: ' + ' '.join(k for k, v in st.items() if not v) + ' down'


def log(line):
    stamp = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    with open(LOG, 'a', encoding='utf-8') as f:
        f.write(f'{stamp} {line}\n')
    print(f'{stamp} {line}', flush=True)


def main():
    if '--once' in sys.argv:
        st, props = probe()
        print(st, props, verdict(st))
        return
    log(f'start: watching {IP} every {INTERVAL}s')
    last, last_beat, down_since = None, 0.0, None
    while True:
        st, props = probe()
        v = verdict(st)
        now = time.time()
        if v != last:
            extra = f' (was down {int(now - down_since)}s)' if v == 'OK' and down_since else ''
            log(f'{v} | ' + ' '.join(f'{k}={int(x)}' for k, x in st.items()) + f' | props={props}{extra}')
            down_since = None if v == 'OK' else (down_since or now)
            last = v
        elif now - last_beat > 1800:
            log(f'heartbeat: {v}')
            last_beat = now
        time.sleep(INTERVAL)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        log('stopped')
