"""Consumers of invalid values: SunSpec, counters, eval (and the logger)."""
import os
import tempfile
import time

from uvrgwtest import Env, S, free_port, sunspec_power, wait_until

LOGGER = '''mqtt {{
  host = "127.0.0.1"
  port = {port}
  qos = 1
  {outs}
  logger log {{
    topic = "log"
    interval = 1
    {vals}
  }}
}}
'''


def outputs(port, names):
    outs = ' '.join('value %s { dir = out  type = number  topic = "out/%s"  fmt = "%%.6g" }' % (n, n) for n in names)
    return LOGGER.format(port=port, outs=outs, vals=' '.join('value %s { }' % n for n in names))


def test_sunspec():
    """Exception 4 while the power source is invalid; a non-power source
    without timeout keeps its value."""
    with Env() as env:
        js = env.json()
        port = free_port()
        u = env.uvrgw('json {\n  url = "%s"\n  interval = 200\n  value p { path = "p"  stale_timeout = 1500 }\n  value i { path = "i" }\n}\n'
                      'sunspec_server {\n  bind = "127.0.0.1"\n  port = %d\n  meter m { unit_id = 1  power = "p"  current_l1 = "i" }\n}\n' % (js.url, port))
        # the state is logged on requests
        assert wait_until(lambda: sunspec_power(port, 1) == 4, 5, 'exception 4 before data')
        u.wait_log("sunspec meter 'm' unavailable")
        js.data = dict(p=1000, i=5)
        assert wait_until(lambda: sunspec_power(port, 1) == 1000, 5, 'power 1000')
        u.wait_log("sunspec meter 'm' available")
        lm = u.mark()
        js.data = None
        u.wait_log(r"value 'p' timed out", since=lm)
        assert wait_until(lambda: sunspec_power(port, 1) == 4, 3, 'exception 4 after the timeout')
        u.wait_log(r"sunspec meter 'm' unavailable: value 'p' is invalid", since=lm)
        js.data = dict(p=500, i=5)
        assert wait_until(lambda: sunspec_power(port, 1) == 500, 5, 'power 500')
        u.stop()


def test_power_counter():
    """Integration stops at the timeout of the power source, the total
    stays valid and frozen, and integration resumes with new data."""
    with Env() as env:
        broker = env.broker()
        sub = env.mqtt(broker.port, '#')
        js = env.json()
        u = env.uvrgw('json {\n  url = "%s"\n  interval = 200\n  value p { path = "p"  stale_timeout = 1500 }\n}\n'
                      'counter pc { source = p  integrate_power = true }\n' % js.url + outputs(broker.port, ['pc']))
        js.data = dict(p=3600000)       # 1000 Wh per s
        sub.wait('out/pc', lambda v: float(v) > 2000, timeout=5)
        lm = u.mark()
        js.data = None
        u.wait_log(r"value 'p' timed out", since=lm)
        # the total integrated up to the reset is published with the next tick (1 s)
        time.sleep(S.t(1.2))
        m = sub.mark()
        total = sub.snapshot('log', lambda s: s['pc'] is not None, since=m)['pc']
        later = sub.snapshot('log', lambda s: True, since=sub.mark())
        assert later['pc'] == total, 'integrated after the timeout: %s -> %s' % (total, later['pc'])
        js.data = dict(p=3600000)
        sub.wait('out/pc', lambda v: float(v) > total + 1000, since=sub.mark())
        u.stop()


def test_device_counter():
    """A timeout of the source is not a reading: no reset is detected, the
    total stays valid, the next reading continues without a step."""
    with Env() as env:
        broker = env.broker()
        sub = env.mqtt(broker.port, '#')
        js = env.json(dict(m=100))
        u = env.uvrgw('json {\n  url = "%s"\n  interval = 200\n  value m { path = "m"  stale_timeout = 1000 }\n}\n'
                      'counter mc { source = m }\n' % js.url + outputs(broker.port, ['mc']))
        sub.wait_value('out/mc', 100)
        js.data = None
        u.wait_log(r"value 'm' timed out")
        sub.snapshot('log', lambda s: s['mc'] == 100, since=sub.mark())
        js.data = dict(m=105)
        sub.wait_value('out/mc', 105, since=sub.mark())
        assert u.find('reset detected|implausible') == []
        u.stop()


def test_counter_state_published_at_startup():
    """A counter loaded from its state file is valid without source data."""
    with Env() as env:
        broker = env.broker()
        sub = env.mqtt(broker.port, '#')
        js = env.json()
        state = tempfile.mkdtemp(dir=S.tmp, prefix='state-')
        os.makedirs(os.path.join(state, 'counters'))
        with open(os.path.join(state, 'counters', 'mc'), 'w') as f:
            f.write('100 50\n')
        u = env.uvrgw('json {\n  url = "%s"\n  interval = 200\n  value m { path = "m" }\n}\n'
                      'counter mc { source = m }\n' % js.url + outputs(broker.port, ['mc']), state_dir=state)
        sub.wait_value('out/mc', 100)
        sub.snapshot('log', lambda s: s['mc'] == 100)
        u.stop()


def test_eval():
    """An eval is published with the numbers it reads and is invalid while
    an input is invalid or its result is not finite."""
    with Env() as env:
        broker = env.broker()
        sub = env.mqtt(broker.port, '#')
        js = env.json()
        u = env.uvrgw('json {\n  url = "%s"\n  interval = 200\n  value p { path = "p"  stale_timeout = 1500 }\n  value t { path = "t" }\n}\n'
                      'eval e {\n  value sum { expr = "p + t" }\n  value q { expr = "1 / (t - 20)" }\n}\n'
                      'eval n {\n  period = 500\n  value x { expr = "x + 1"  init_value = 10 }\n}\n'
                      % js.url + outputs(broker.port, ['sum', 'q', 'x']))
        # before data: published from the init values, logged as null
        sub.wait_value('out/sum', 0)
        sub.wait_value('out/x', 11)
        sub.snapshot('log', lambda s: s['sum'] is None and s['q'] is None and s['x'] is not None)
        js.data = dict(p=1, t=25)
        m = sub.mark()
        sub.wait_value('out/sum', 26, since=m)
        sub.snapshot('log', lambda s: (s['sum'], s['q']) == (26, 0.2), since=m)
        # not finite: previous number kept, invalid, logged once
        js.data = dict(p=1, t=20)
        u.wait_log(r"eval 'e': value 'q' invalid: result is not finite")
        m = sub.mark()
        sub.snapshot('log', lambda s: s['q'] is None and s['sum'] == 21, since=m)
        assert set(sub.payloads('out/q', m)) <= {'0.2'}, sub.payloads('out/q', m)
        js.data = dict(p=1, t=25)
        u.wait_log(r"eval 'e': value 'q' finite again")
        assert len(u.find('result is not finite')) == 1
        # input timed out: published with its init_value, logged as null
        m = sub.mark()
        js.data = None
        u.wait_log(r"value 'p' timed out")
        sub.wait_value('out/sum', 25, since=m)
        sub.snapshot('log', lambda s: s['sum'] is None and s['q'] == 0.2, since=m)
        u.stop()
