"""Dispatch of resets: outputs, loopback, reset racing with real data."""
import json
import re
import struct
import time

from uvrgwtest import Env, S, ana_id, wait_until

TIMEOUT = 600     # ms, race test


def test_outputs_get_init_value():
    """A timeout reset is written to every output type as init_value."""
    with Env() as env:
        bus = env.can()
        broker = env.broker()
        sub = env.mqtt(broker.port, '#')
        js = env.json()
        dev = env.modbus_tcp()
        u = env.uvrgw('''json {{
  url = "{url}"
  interval = 200
  value p {{ path = "p"  stale_timeout = 1500  init_value = -5 }}
}}
mqtt {{
  host = "127.0.0.1"
  port = {mqtt}
  value p {{ dir = out  type = number  topic = "out/p"  fmt = "%.0f" }}
}}
modbus_tcp {{
  ip = "127.0.0.1"
  port = {mb}
  slave {{
    id = 1
    interval = 200
    block {{
      dir = out
      regtype = reg
      addr = 10
      count = 1
      value p {{ reg = 0  type = s16 }}
    }}
  }}
}}
can {{
  interface = "vcan0"
  send_timeout = 100
  frame {{
    can_id = "ANA:2:0"
    dir = out
    value p {{ type = s16  pos = 0 }}
  }}
}}
'''.format(url=js.url, mqtt=broker.port, mb=dev.port))

        def can_value():
            return struct.unpack('<h', bus.recv(ana_id(2, 0))[:2])[0]

        def mb_value():
            return struct.unpack('>h', struct.pack('>H', dev.holding.get(10, 0)))[0]

        js.data = dict(p=7)
        sub.wait_value('out/p', 7)
        assert can_value() == 7
        assert mb_value() == 7
        js.data = None
        u.wait_log(r"value 'p' timed out, reset to -5\.")
        sub.wait_value('out/p', -5)
        assert can_value() == -5
        assert mb_value() == -5
        u.stop()


def test_outputs_round():
    """Integer outputs are rounded, not truncated (0.3 / 0.1 is 2.99...)."""
    with Env() as env:
        bus = env.can()
        js = env.json(dict(x=0.3, y=-0.3))
        dev = env.modbus_tcp()
        u = env.uvrgw('''json {{
  url = "{url}"
  interval = 200
  value x {{ path = "x" }}
  value y {{ path = "y" }}
}}
modbus_tcp {{
  ip = "127.0.0.1"
  port = {mb}
  slave {{
    id = 1
    interval = 200
    block {{
      dir = out
      regtype = reg
      addr = 10
      count = 2
      value x {{ reg = 0  type = u16  scale = 0.1 }}
      value y {{ reg = 1  type = s16  scale = 0.1 }}
    }}
  }}
}}
can {{
  interface = "vcan0"
  send_timeout = 100
  frame {{
    can_id = "ANA:2:0"
    dir = out
    value x {{ type = u16  pos = 0  scale = 0.1 }}
    value y {{ type = s16  pos = 2  scale = 0.1 }}
  }}
}}
'''.format(url=js.url, mb=dev.port))
        assert struct.unpack('<Hh', bus.recv(ana_id(2, 0))[:4]) == (3, -3)
        wait_until(lambda: 10 in dev.holding and 11 in dev.holding, 5, 'modbus writes')
        assert (dev.holding[10], dev.holding[11]) == (3, 0xfffd), dev.holding
        u.stop()


def test_mqtt_loopback():
    """x is an MQTT input on one connection and an output on another: the
    resets are sent to the output topic only, never to the input's topic."""
    with Env() as env:
        broker = env.broker()
        sub = env.mqtt(broker.port, '#')
        pub = env.mqtt(broker.port)
        u = env.uvrgw('''mqtt {{
  host = "127.0.0.1"
  port = {p}
  value x {{ dir = in  type = number  topic = "in/x"  stale_timeout = 1500 }}
}}
mqtt {{
  host = "127.0.0.1"
  port = {p}
  value x {{ dir = out  type = number  topic = "out/x"  fmt = "%.1f" }}
}}
'''.format(p=broker.port))
        u.wait_log(r"value 'x' never received, set to 0\.")
        sub.wait_value('out/x', 0)
        pub.pub('in/x', '5')
        sub.wait_value('out/x', 5)
        m = sub.mark()
        u.wait_log(r"value 'x' timed out, reset to 0\.")
        sub.wait_value('out/x', 0, since=m)
        pub.pub('in/x', '6')
        sub.wait_value('out/x', 6, since=m)
        u.stop()
        assert sub.payloads('in/x') == ['5', '6'], 'uvrgw published on the input topic: %s' % sub.payloads('in/x')


def test_race():
    """Resets racing with real data (stale_timeout just below the poll
    interval of 700 ms): the output, the stored value (logger) and the counter
    always agree, and a reset never overtakes fresher data.  30 s, 5 min
    with --slow."""
    duration = 300 if S.slow else 30
    with Env() as env:
        broker = env.broker()
        js = env.json(dict(p=1000, m=1000))
        u = env.uvrgw('''json {{
  url = "{url}"
  interval = 700
  stale_timeout = {timeout}
  value p {{ path = "p" }}
  value m {{ path = "m" }}
}}
mqtt {{
  host = "127.0.0.1"
  port = {port}
  value p  {{ dir = out  type = number  topic = "out/p"  fmt = "%.0f" }}
  value mc {{ dir = out  type = number  topic = "out/mc"  fmt = "%.0f" }}
  logger log {{
    topic = "log"
    interval = 1
    value p {{ }}
    value mc {{ }}
  }}
}}
counter mc {{ source = m }}
'''.format(url=js.url, port=broker.port, timeout=TIMEOUT), pub=True)
        n = 1000
        end = time.monotonic() + duration
        while time.monotonic() < end:
            n += 1
            js.data = dict(p=n, m=n)
            time.sleep(0.3)
        time.sleep(S.t(3))              # the last value is held
        u.stop()

        events = []                     # in log order: ('p', v) / ('mc', v) / ('log', snapshot) / ('reset', ms)
        last_data_ms = None
        errors = []
        for ms, t in u.lines:
            m = re.match(r'PUB out/(p|mc) (\S+)$', t)
            if m:
                v = float(m.group(2))
                if m.group(1) == 'p':
                    if v == 0:
                        if last_data_ms is not None and ms - last_data_ms < TIMEOUT:
                            errors.append('reset %d ms after data' % (ms - last_data_ms))
                    else:
                        last_data_ms = ms
                events.append((m.group(1), v))
            elif t.startswith('PUB log '):
                events.append(('log', json.loads(t[8:])))

        out_p = [v for k, v in events if k == 'p']
        data = [v for v in out_p if v != 0]
        assert data == sorted(data), 'data went back'
        resets = out_p.count(0)
        # each snapshot agrees with the last output (or the one in flight)
        for i, (k, v) in enumerate(events):
            if k != 'log':
                continue
            want = 0 if v['p'] is None else v['p']
            before = [x for kk, x in events[:i] if kk == 'p']
            after = [x for kk, x in events[i + 1:i + 3] if kk == 'p']
            # before the first data nothing is sent, the logger writes null
            last = before[-1] if before else 0
            if last != want and want not in after:
                errors.append('snapshot p=%s, output %s' % (v['p'], before[-1] if before else None))
        mcs = [v for k, v in events if k == 'mc']
        snaps = [v for k, v in events if k == 'log']
        assert data[-1] == n and mcs[-1] == n and snaps[-1]['p'] == n and snaps[-1]['mc'] == n, \
            'end state: output %s, counter %s, snapshot %s, source %d' % (data[-1], mcs[-1], snaps[-1], n)
        assert u.find('reset detected|implausible') == []
        assert not errors, errors[:5]
        assert resets >= (1 if not S.valgrind else 0), 'no reset, no race tested'


def test_modbus_write_failure():
    """A failing Modbus write is logged once until a write succeeds again."""
    with Env() as env:
        js = env.json()
        dev = env.modbus_tcp()
        dev.fail = True
        u = env.uvrgw('''json {{
  url = "{url}"
  interval = 200
  value p {{ path = "p" }}
}}
modbus_tcp {{
  ip = "127.0.0.1"
  port = {mb}
  timeout = 200
  slave {{
    id = 1
    interval = 200
    block {{
      dir = out
      regtype = reg
      addr = 10
      count = 1
      value p {{ reg = 0  type = s16 }}
    }}
  }}
}}
'''.format(url=js.url, mb=dev.port))
        n = 0
        end = time.monotonic() + S.t(2)
        while time.monotonic() < end:       # a new value (and write) every poll
            n += 1
            js.data = dict(p=n)
            time.sleep(0.25)
        u.wait_log(r"Failed to write MODBUS value 'p' to slave 1")
        dev.fail = False
        n += 1
        js.data = dict(p=n)
        u.wait_log(r"MODBUS value 'p' written to slave 1 again")
        # the retried old value may be written first
        wait_until(lambda: len(dev.writes) > 0 and dev.writes[-1] == (10, [n]), 5, 'write of the new value')
        u.stop()
        assert len(u.find('Failed to write MODBUS value')) == 1, u.find('Failed to write MODBUS value')


def test_modbus_write_retry():
    """A failed Modbus write is retried without a new value."""
    with Env() as env:
        broker = env.broker()
        pub = env.mqtt(broker.port)
        dev = env.modbus_tcp()
        dev.fail = True
        u = env.uvrgw('''mqtt {{
  host = "127.0.0.1"
  port = {mqtt}
  value p {{ dir = in  type = number  topic = "in/p" }}
}}
modbus_tcp {{
  ip = "127.0.0.1"
  port = {mb}
  timeout = 200
  slave {{
    id = 1
    interval = 200
    block {{
      dir = out
      regtype = reg
      addr = 10
      count = 1
      value p {{ reg = 0  type = s16 }}
    }}
  }}
}}
'''.format(mqtt=broker.port, mb=dev.port))
        u.wait_log('mqtt connected')
        time.sleep(0.2)     # subscription
        pub.pub('in/p', '42')
        u.wait_log(r"Failed to write MODBUS value 'p' to slave 1")
        assert dev.writes == []
        dev.fail = False
        u.wait_log(r"MODBUS value 'p' written to slave 1 again")
        assert dev.writes == [(10, [42])], dev.writes
        time.sleep(S.t(0.5))
        assert dev.writes == [(10, [42])], dev.writes
        u.stop()
        assert len(u.find('Failed to write MODBUS value')) == 1, u.find('Failed to write MODBUS value')
