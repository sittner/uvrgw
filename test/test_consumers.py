# SPDX-License-Identifier: GPL-3.0-or-later
"""Consumers: SunSpec meters, counters, eval (and the logger)."""
import math
import os
import socket
import struct
import subprocess
import tempfile
import time

from uvrgwtest import Env, S, free_port, mb_read, mb_request, regs_f32, sunspec_power, wait_until

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


def test_sunspec_map():
    """The whole register map of a three-phase meter: common model
    strings, measured and derived quantities, explicit total, end model;
    unknown unit IDs are not answered, other functions and addresses are
    refused."""
    phases = [dict(i=10, u=230, p=2000, imp=100, exp=10), dict(i=20, u=230, p=4000, imp=200, exp=20),
              dict(i=30, u=230, p=6000, imp=300, exp=30)]
    data = dict(pf1=0.9, f=50, pt=11000)
    for n, ph in enumerate(phases, 1):
        data.update({'%s%d' % (k, n): v for k, v in ph.items()})
    with Env() as env:
        js = env.json(data)
        port = free_port()
        u = env.uvrgw('json {\n  url = "%s"\n  interval = 200\n%s}\n'
                      'sunspec_server {\n  bind = "127.0.0.1"\n  port = %d\n'
                      '  meter m {\n    unit_id = 5\n    manufacturer = "modusoft"\n    model = "SmartMeterGW"\n    serial = "00000001"\n'
                      '    current_l1 = "i1"  voltage_l1 = "u1"  power_l1 = "p1"  pf_l1 = "pf1"  energy_import_l1 = "imp1"  energy_export_l1 = "exp1"\n'
                      '    current_l2 = "i2"  voltage_l2 = "u2"  power_l2 = "p2"  energy_import_l2 = "imp2"  energy_export_l2 = "exp2"\n'
                      '    current_l3 = "i3"  voltage_l3 = "u3"  power_l3 = "p3"  energy_import_l3 = "imp3"  energy_export_l3 = "exp3"\n'
                      '    power = "pt"  frequency = "f"\n  }\n}\n'
                      % (js.url, ''.join('  value %s { path = "%s" %s }\n' % (k, k, 'stale_timeout = 5000' if k[0] == 'p' and k != 'pf1' else '')
                                         for k in data), port))
        assert wait_until(lambda: sunspec_power(port, 5) == 11000, 5, 'meter data')
        regs = mb_read(port, 5, 40000, 125) + mb_read(port, 5, 40125, 72)
        assert len(regs) == 197

        def text(off, n):
            return b''.join(struct.pack('>H', r) for r in regs[off:off + n]).rstrip(b'\0').decode()

        def floats(off, n):
            return [regs_f32(regs[off + 2 * k:off + 2 * k + 2]) for k in range(n)]

        def close(got, exp):
            assert len(got) == len(exp) and all(math.isclose(g, e, rel_tol=1e-6, abs_tol=1e-9) for g, e in zip(got, exp)), (got, exp)

        assert text(0, 2) == 'SunS'
        assert regs[2:4] == [1, 65]
        assert (text(4, 16), text(20, 16), text(36, 8), text(44, 8), text(52, 16)) == ('modusoft', 'SmartMeterGW', '', '', '00000001')
        assert regs[68] == 5
        assert regs[69:71] == [213, 124]
        s = [ph['u'] * ph['i'] for ph in phases]
        q = [math.sqrt(s[k] ** 2 - phases[k]['p'] ** 2) for k in range(3)]
        pf = [0.9] + [phases[k]['p'] / s[k] for k in (1, 2)]
        vpp = 230 * math.sqrt(3)
        close(floats(71, 4), [60, 10, 20, 30])                                      # A
        close(floats(79, 4), [230, 230, 230, 230])                                  # V
        close(floats(87, 4), [vpp] * 4)                                             # V phase-phase
        close(floats(95, 1), [50])                                                  # Hz
        close(floats(97, 4), [11000, 2000, 4000, 6000])                             # W (explicit total)
        close(floats(105, 4), [sum(s)] + s)                                         # VA
        close(floats(113, 4), [sum(q)] + q)                                         # VAr
        close(floats(121, 4), [11000 / sum(s)] + pf)                                # PF
        close(floats(129, 4), [60, 10, 20, 30])                                     # Wh export
        close(floats(137, 4), [600, 100, 200, 300])                                 # Wh import
        assert regs[145:195] == [0] * 50                                            # VAh, VArh, events
        assert regs[195:197] == [0xffff, 0]
        # unknown unit: no answer; function 6: exception 1; outside the map: exception 2
        assert mb_request(port, 6, struct.pack('>BHH', 3, 40000, 2), timeout=0.5) is None
        assert mb_request(port, 5, struct.pack('>BHH', 6, 40000, 0)) == b'\x86\x01'
        assert mb_read(port, 5, 40196, 2) == 2
        u.stop()


def test_sunspec_clients():
    """At most 8 clients; further connections are rejected and logged
    once, until a client disconnects.  Keepalive is enabled on the
    connections, so a vanished client does not block a slot forever."""
    with Env() as env:
        js = env.json(dict(p=100))
        port = free_port()
        u = env.uvrgw('json {\n  url = "%s"\n  interval = 200\n  value p { path = "p"  stale_timeout = 5000 }\n}\n'
                      'sunspec_server {\n  bind = "127.0.0.1"\n  port = %d\n  meter m { unit_id = 1  power = "p" }\n}\n' % (js.url, port))
        assert wait_until(lambda: sunspec_power(port, 1) == 100, 5, 'meter data')
        idle = [socket.create_connection(('127.0.0.1', port)) for _ in range(8)]

        def rejected():
            try:
                sunspec_power(port, 1)
                return False
            except (OSError, struct.error):
                return True

        assert wait_until(rejected, 5, 'rejection with 8 clients')
        u.wait_log('sunspec server: too many clients, connections rejected')
        assert rejected()
        ss = subprocess.run(['ss', '-tno', 'state', 'established', '( sport = :%d )' % port], capture_output=True, text=True).stdout
        assert ss.count('keepalive') == 8, ss
        idle.pop().close()
        assert wait_until(lambda: not rejected() and sunspec_power(port, 1) == 100, 5, 'accepted after a disconnect')
        u.wait_log('sunspec server: accepting connections again')
        assert len(u.find('too many clients')) == 1
        for c in idle:
            c.close()
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
