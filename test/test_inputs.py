"""Timeouts at runtime for every input type, with device section inheritance.

Each input section sets stale_timeout = 1500 and init_value = -1 and has
three values:
  a: inherits both,
  b: stale_timeout = 0 (keeps its last value and stays valid),
  c: stale_timeout = 1000 and init_value = -2 of its own.
The values are forwarded to MQTT outputs out/<name> and logged by a
logger (1 s) on the same broker.
"""
import struct
import threading
import time

from uvrgwtest import Env, S, ana_id, wait_until

VALS = '{a} value b {{ {b} stale_timeout = 0 }} value c {{ {c} stale_timeout = 1000  init_value = -2 }}'
SECTION = 'stale_timeout = 1500\n  init_value = -1\n'

OUTPUTS = '''mqtt {{
  host = "127.0.0.1"
  port = {port}
  qos = 1
  value a {{ dir = out  type = number  topic = "out/a"  fmt = "%.0f" }}
  value b {{ dir = out  type = number  topic = "out/b"  fmt = "%.0f" }}
  value c {{ dir = out  type = number  topic = "out/c"  fmt = "%.0f" }}
  logger log {{
    topic = "log"
    interval = 1
    value a {{ }}
    value b {{ }}
    value c {{ }}
  }}
}}
'''


class Feeder:
    """Calls send() every 200 ms while running (event driven inputs)."""

    def __init__(self, send):
        self.send = send
        self.values = None
        self.running = True
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while self.running:
            if self.values is not None:
                self.send(self.values)
            time.sleep(0.2)

    def close(self):
        self.running = False


def scenario(env, broker, input_conf, feed, stop, fail_log=None, ok_log=None):
    """feed(dict) delivers values from now on, stop() makes the source dead.

    A polled source is dead twice (at startup and later): its failure
    (@p fail_log) and recovery (@p ok_log) must be logged once each time,
    not once per poll."""
    sub = env.mqtt(broker.port, '#')
    u = env.uvrgw(input_conf + OUTPUTS.format(port=broker.port))

    # nothing received: a and c are set to their init_value at their timeout
    u.wait_log(r"value 'c' never received, set to -2\.")
    u.wait_log(r"value 'a' never received, set to -1\.")
    sub.wait_value('out/a', -1)
    sub.wait_value('out/c', -2)
    assert sub.payloads('out/b') == [], 'b sent before its first data'
    sub.snapshot('log', lambda s: s == dict(time=s['time'], a=None, b=None, c=None))

    # data
    m = sub.mark()
    feed(dict(a=10, b=20, c=30))
    for k, v in dict(a=10, b=20, c=30).items():
        sub.wait_value('out/' + k, v, since=m)
    u.wait_log(r"value 'a' received again\.")
    sub.snapshot('log', lambda s: (s['a'], s['b'], s['c']) == (10, 20, 30), since=m)

    # source dead: a and c time out, b keeps its value and stays valid
    m, lm = sub.mark(), u.mark()
    stop()
    t0 = time.monotonic()
    u.wait_log(r"value 'c' timed out, reset to -2\.", since=lm)
    u.wait_log(r"value 'a' timed out, reset to -1\.", since=lm)
    elapsed = time.monotonic() - t0
    assert S.valgrind or 1.4 < elapsed < 3.5, 'timeout of a after %.1f s' % elapsed
    sub.wait_value('out/a', -1, since=m)
    sub.wait_value('out/c', -2, since=m)
    sub.snapshot('log', lambda s: (s['a'], s['b'], s['c']) == (None, 20, None), since=sub.mark())
    assert u.find(r"value 'b' timed out") == []

    # data again
    m = sub.mark()
    feed(dict(a=11, b=21, c=31))
    for k, v in dict(a=11, b=21, c=31).items():
        sub.wait_value('out/' + k, v, since=m)
    sub.snapshot('log', lambda s: (s['a'], s['b'], s['c']) == (11, 21, 31), since=m)
    u.stop()
    if fail_log:
        assert len(u.find(fail_log)) == 2, u.find(fail_log)
        assert len(u.find(ok_log)) == 2, u.find(ok_log)


def test_json():
    with Env() as env:
        broker = env.broker()
        js = env.json()
        conf = 'json {\n  url = "%s"\n  interval = 200\n  %s  %s\n}\n' % (
            js.url, SECTION, VALS.format(a='value a { path = "a" }', b='path = "b"', c='path = "c"'))

        def feed(d):
            js.data = d

        def stop():
            js.data = None

        scenario(env, broker, conf, feed, stop, 'Failed to perform GET', r"Data of url '.*' received again")


def test_mqtt():
    with Env() as env:
        broker = env.broker()
        pub = env.mqtt(broker.port)
        conf = 'mqtt {\n  host = "127.0.0.1"\n  port = %d\n  %s  %s\n}\n' % (broker.port, SECTION, VALS.format(
            a='value a { dir = in  type = number  topic = "in/a" }',
            b='dir = in  type = number  topic = "in/b"', c='dir = in  type = number  topic = "in/c"'))
        f = env.add(Feeder(lambda d: [pub.pub('in/' + k, str(v)) for k, v in d.items()]))

        def feed(d):
            f.values = d

        def stop():
            f.values = None

        scenario(env, broker, conf, feed, stop)


def modbus_conf(head):
    return '%s\n  slave {\n    id = 1\n    interval = 200\n    %s    block {\n      dir = in\n      regtype = reg\n      addr = 0\n      count = 3\n      %s\n    }\n  }\n}\n' % (
        head, SECTION, VALS.format(a='value a { reg = 0  type = s16 }', b='reg = 1  type = s16', c='reg = 2  type = s16'))


def modbus_scenario(env, broker, dev, head):
    def feed(d):
        dev.set(0, [d['a'], d['b'], d['c']])
        dev.fail = False

    def stop():
        dev.fail = True

    dev.fail = True
    scenario(env, broker, modbus_conf(head), feed, stop, 'Failed to read MODBUS block', r'MODBUS block of slave 1 \(start 0\) read again')


def test_modbus_tcp():
    with Env() as env:
        broker = env.broker()
        dev = env.modbus_tcp()
        modbus_scenario(env, broker, dev, 'modbus_tcp {\n  ip = "127.0.0.1"\n  port = %d\n  timeout = 200' % dev.port)


def test_modbus_rtu():
    with Env() as env:
        broker = env.broker()
        dev = env.modbus_rtu()
        modbus_scenario(env, broker, dev, 'modbus_rtu {\n  interface = "%s"\n  baud = 115200\n  timeout = 200' % dev.path)


def test_modbus_rtu_late_response():
    """A response arriving after the timeout is not taken as the response
    to the next request."""
    with Env() as env:
        broker = env.broker()
        sub = env.mqtt(broker.port, '#')
        dev = env.modbus_rtu()
        dev.set(100, [111])
        dev.set(200, [222])
        u = env.uvrgw('''modbus_rtu {{
  interface = "{path}"
  baud = 115200
  timeout = 200
  slave {{
    id = 1
    interval = 300
    block {{ dir = in  regtype = reg  addr = 100  count = 1  value a {{ reg = 0  type = u16 }} }}
    block {{ dir = in  regtype = reg  addr = 200  count = 1  value b {{ reg = 0  type = u16 }} }}
  }}
}}
mqtt {{
  host = "127.0.0.1"
  port = {mqtt}
  value a {{ dir = out  type = number  topic = "out/a"  fmt = "%.0f" }}
  value b {{ dir = out  type = number  topic = "out/b"  fmt = "%.0f" }}
}}
'''.format(path=dev.path, mqtt=broker.port))
        sub.wait_value('out/a', 111)
        sub.wait_value('out/b', 222)
        dev.late = S.t(0.4)
        u.wait_log('Failed to read MODBUS block')
        u.wait_log('read again')
        m = sub.mark()
        wait_until(lambda: len(sub.payloads('out/a', m)) >= 3 and len(sub.payloads('out/b', m)) >= 3, 5, 'polls after the late response')
        assert set(sub.payloads('out/a', m)) == {'111'}, sub.payloads('out/a', m)
        assert set(sub.payloads('out/b', m)) == {'222'}, sub.payloads('out/b', m)
        u.stop()


def test_can():
    with Env() as env:
        bus = env.can()
        broker = env.broker()
        conf = 'can {\n  interface = "vcan0"\n  %s  frame {\n    can_id = "ANA:1:0"\n    dir = in\n    %s\n  }\n}\n' % (SECTION, VALS.format(
            a='value a { type = s16  pos = 0 }', b='type = s16  pos = 2', c='type = s16  pos = 4'))
        f = env.add(Feeder(lambda d: bus.send(ana_id(1, 0), struct.pack('<hhh', d['a'], d['b'], d['c']))))

        def feed(d):
            f.values = d

        def stop():
            f.values = None

        scenario(env, broker, conf, feed, stop)


def test_json_missing_path():
    """A missing path is logged once until it is found again."""
    with Env() as env:
        js = env.json(dict(a=1))
        u = env.uvrgw('json {\n  url = "%s"\n  interval = 200\n  value a { path = "a" }\n  value b { path = "x.b" }\n}\n'
                      'eval e {\n  value s { expr = "a + b" }\n}\n' % js.url)
        u.wait_log(r"Failed lookup json path 'x\.b'")
        time.sleep(S.t(1))
        js.data = dict(a=1, x=dict(b='text'))      # still no number: same state
        time.sleep(S.t(1))
        js.data = dict(a=1, x=dict(b=2))
        u.wait_log(r"Json path 'x\.b' for url '.*' found again")
        time.sleep(S.t(1))
        u.stop()
        # path missing and wrong type are one state: logged once
        assert len(u.find("Failed lookup json path|Invalid value type")) == 1, u.find("json path|value type")
        assert len(u.find("found again")) == 1
