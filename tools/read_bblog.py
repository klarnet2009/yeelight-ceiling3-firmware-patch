"""Read and decode the lamp's black box ("bblog" property, firmware/src/blackbox_monitor.c).

Run: python tools/read_bblog.py [--ip=192.168.0.20]        read from the lamp
     python tools/read_bblog.py --text="B0;N4,0;..."       decode a saved string

Token format (see blackbox_monitor.c):
  B<t>[,u<m>][,pc<x>,lr<x>,cf<x>][,t<x>]   boot; T<x> boot UTC; N<s>,<m> network state;
  R<r><n>,<m>[,t<x>] monitor reboot.
Times: a boot section with a T token gets wall-clock times for all its events; without it the
section start is estimated from the next boot record (t - u) or shown as minutes of uptime.
"""
import bisect
import csv
import datetime
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NET = {0: 'не настроена', 1: 'точка доступа (настройка)', 2: 'нет Wi-Fi/IP', 3: 'локальная сеть, без облака',
       4: 'облако', 5: 'обновление прошивки'}
BOOT = {0: 'включение питания или сброс без записи', 1: 'программная перезагрузка', 2: 'СБОЙ ПРОШИВКИ'}
REASON = {'D': 'нет Wi-Fi', 'A': 'роутер не отвечает', 'L': 'завис сетевой стек'}
EXT = (0x10150650, 0x10151650)


def functions():
    rows = []
    try:
        for r in csv.reader(open(os.path.join(ROOT, 'decomp', 'functions.csv'))):
            if r and r[0][:2] == '10':
                rows.append((int(r[0], 16), r[1], r[4]))
    except OSError:
        pass
    rows.sort()
    return rows


def where(addr, rows):
    a = addr & ~1
    if EXT[0] <= a < EXT[1]:
        return 'blackbox_monitor.c (наш код)'
    if not rows or not 0x10071000 <= a < 0x10150650:
        return 'вне прошивки'
    f = rows[bisect.bisect_right([r[0] for r in rows], a) - 1]
    return f'{f[1]}+0x{a - f[0]:x} ({f[2]})'


def fields(parts):
    """['u12', 'pc100..', 'lr..', 'cf..', 't69..'] -> dict"""
    out = {}
    for p in parts:
        for key in ('pc', 'lr', 'cf', 'u', 't'):
            if p.startswith(key):
                out[key] = int(p[len(key):], 10 if key == 'u' else 16)
                break
    return out


def fmt(ts):
    return datetime.datetime.fromtimestamp(ts).strftime('%d.%m %H:%M')


def decode(raw, life=None, now=None):
    rows = functions()
    toks = [t for t in raw.split(';') if t]
    # split into boot sections
    sections, cur = [], {'boot': None, 'events': [], 'epoch': None}
    for t in toks:
        if t[0] == 'B':
            if cur['boot'] is not None or cur['events']:
                sections.append(cur)
            cur = {'boot': t, 'events': [], 'epoch': None}
        elif t[0] == 'T':
            cur['epoch'] = int(t[1:], 16)
        else:
            cur['events'].append(t)
    sections.append(cur)
    # section start times: own T, else next boot's (t - u), else now - life for the last one
    for i, s in enumerate(sections):
        if s['epoch'] is None and i + 1 < len(sections) and sections[i + 1]['boot']:
            f = fields(sections[i + 1]['boot'][2:].split(',')[1:])
            if 't' in f and 'u' in f:
                s['epoch'] = f['t'] - f['u'] * 60
    if sections and sections[-1]['epoch'] is None and life is not None and now is not None:
        sections[-1]['epoch'] = int(now - life)

    for i, s in enumerate(sections):
        if s['boot'] is None:
            print('=== (начало записи вытеснено из журнала)')
        else:
            kind = int(s['boot'][1])
            f = fields(s['boot'][2:].split(',')[1:])
            line = f'=== ЗАГРУЗКА: {BOOT.get(kind, kind)}'
            if s['epoch'] is not None:
                line += f', {fmt(s["epoch"])}'
            if i == len(sections) - 1:
                line += '  ← текущая'
            print(line)
            if 'u' in f:
                died = f', последний раз был жив {fmt(f["t"])}' if 't' in f else ''
                print(f'      прошлый запуск проработал {f["u"]} мин{died}')
            if 'pc' in f:
                print(f'      место сбоя: PC=0x{f["pc"]:08x} {where(f["pc"], rows)}')
                print(f'                  LR=0x{f["lr"]:08x} {where(f["lr"], rows)}')
                print(f'                  CFSR=0x{f.get("cf", 0):08x}')
        for t in s['events']:
            parts = t.split(',')
            minute = int(parts[1])
            when = fmt(s['epoch'] + minute * 60) if s['epoch'] is not None else f'+{minute} мин'
            if t[0] == 'N':
                print(f'  {when}  сеть: {NET.get(int(parts[0][1:]), parts[0][1:])}')
            elif t[0] == 'R':
                f = fields(parts[2:])
                when = fmt(f['t']) if 't' in f else when
                print(f'  {when}  ПЕРЕЗАГРУЗКА МОНИТОРОМ: {REASON.get(t[1], t[1])} (№{t[2:].split(",")[0]} подряд)')


def main():
    text = next((a[7:] for a in sys.argv[1:] if a.startswith('--text=')), None)
    if text is not None:
        decode(text)
        return
    sys.path.insert(0, ROOT)
    sys.argv = sys.argv[:1] + [a for a in sys.argv[1:] if a.startswith('--ip')]
    import miio_ota as m
    cls = [v for v in m.__dict__.values() if isinstance(v, type) and hasattr(v, 'call')][0]
    dev = cls(m.IP, m.token())
    dev.hello()
    raw = dev.call('get_prop', ['bblog'])['result'][0]       # alone: see blackbox_monitor.c
    life = dev.call('miIO.info', [])['result']['life']
    print(f'raw: {raw}\n')
    if not raw:
        print('журнал пуст или свойство отсутствует (прошивка без «чёрного ящика» или он в пассивном режиме)')
        return
    decode(raw, life, datetime.datetime.now().timestamp())
    print(f'\nсейчас: {life // 3600} ч {life % 3600 // 60} мин с последней загрузки')


if __name__ == '__main__':
    main()
