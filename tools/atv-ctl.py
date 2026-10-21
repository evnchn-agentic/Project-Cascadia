#!/usr/bin/env python3
"""Apple TV bench levers: AC power (Tuya energy plug) and micro-USB VBUS (WMWL relay).

  atv-ctl.py vbus on|off|status
  atv-ctl.py power on|off|status        # status prints watts
  atv-ctl.py ios-boot                   # vbus off, power cycle, wait for LAN, vbus on

The Apple TV goes to Recovery if USB is attached at power-on, so iOS boots with VBUS off
and the cable is switched in afterwards.
"""
import json, os, socket, sys, termios, time

RELAY = os.environ.get('ATV_RELAY', '/dev/cu.usbserial-1340')
PLUG_ID = os.environ.get('ATV_PLUG_ID', '')   # your plug's Tuya device id
PLUG_IP = os.environ.get('ATV_PLUG_IP', '192.168.50.186')   # DHCP; was .184
ATV_IP = '192.168.50.138'


def relay(cmd):
    fd = os.open(RELAY, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        a = termios.tcgetattr(fd)
        a[4] = a[5] = termios.B9600
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[0] = a[1] = a[3] = 0
        termios.tcsetattr(fd, termios.TCSANOW, a)
        time.sleep(0.2)
        os.write(fd, bytes.fromhex(cmd))
        time.sleep(0.4)
        try:
            return os.read(fd, 16).hex()
        except BlockingIOError:
            return ''
    finally:
        os.close(fd)


def vbus(arg):
    if arg in ('on', 'off'):
        relay('A00101A2' if arg == 'on' else 'A00100A1')
    r = relay('A00105A6')
    return {'a00101a2': 'on', 'a00100a1': 'off'}.get(r, f'unknown ({r})')


def plug():
    import tinytuya
    x = next(d for d in json.load(open(os.path.expanduser('~/devices.json'))) if d['id'] == PLUG_ID)
    d = tinytuya.Device(PLUG_ID, PLUG_IP, x['key'], version=3.4)
    d.set_socketTimeout(6)
    return d


def power(arg):
    d = plug()
    if arg in ('on', 'off'):
        d.set_value('1', arg == 'on')
        time.sleep(1)
    s = d.status()['dps']
    return f"{'on' if s.get('1') else 'off'}, {s.get('19', 0) / 10:.1f} W"


def lan_up(timeout=1.5):
    try:
        socket.create_connection((ATV_IP, 22), timeout).close()
        return True
    except OSError:
        return False


def ios_boot(tries=2):
    # The first iOS boot after a Linux session has twice come up without the jailbreak
    # (home screen and AirPlay fine, sshd refusing); a second power cycle fixed it both times.
    for attempt in range(1, tries + 1):
        print('vbus  :', vbus('off'))
        print('power :', power('off'))
        time.sleep(5)
        print('power :', power('on'))
        t0 = time.time()
        while not lan_up() and time.time() - t0 < 300:
            time.sleep(3)
        if lan_up():
            print(f'lan   : ssh port up after {time.time() - t0:.0f} s (attempt {attempt})')
            print('vbus  :', vbus('on'))
            return
        print(f'lan   : no ssh after 300 s (attempt {attempt}): jailbreak not applied?')
    print('vbus  :', vbus('on'))
    sys.exit(f'no ssh on the Apple TV after {tries} power cycles')


if __name__ == '__main__':
    what, arg = (sys.argv[1:] + ['status', 'status'])[:2]
    if what == 'vbus':
        print(vbus(arg))
    elif what == 'power':
        print(power(arg))
    elif what == 'ios-boot':
        ios_boot()
    else:
        sys.exit(__doc__)
