"""Config checks at load time, and stale_timeout inheritance of all inputs.

A SunSpec power source must have a stale_timeout if it is an input, so
the load result shows whether a value got a timeout from its device
section: this tests the inheritance of every input type without data.
"""
import os
import re

from uvrgwtest import CanBus, Env, Skip, free_port, load

NO_TIMEOUT = r"value 'p' of .* has no stale_timeout, but is read by "


def meter(power='p', extra=''):
    return 'sunspec_server {\n  bind = "127.0.0.1"\n  port = %d\n  meter m { unit_id = 1  power = "%s"  %s }\n}\n' % (free_port(), power, extra)


# input sections with a value p; {sec} and {val} take the options under test
INPUTS = {
    'json': 'json {{\n  url = "http://127.0.0.1:1/x"\n  interval = 1000\n  {sec}\n  value p {{ path = "p"  {val} }}\n}}\n',
    'mqtt': 'mqtt {{\n  host = "127.0.0.1"\n  port = 1\n  {sec}\n  value p {{ dir = in  type = number  topic = "t/p"  {val} }}\n}}\n',
    'can': 'can {{\n  interface = "vcan0"\n  {sec}\n  frame {{\n    can_id = 0x201\n    dir = in\n    value p {{ type = s16  pos = 0  {val} }}\n  }}\n}}\n',
    'modbus_tcp': 'modbus_tcp {{\n  ip = "127.0.0.1"\n  port = 1\n  slave {{\n    id = 1\n    interval = 1000\n    {sec}\n'
                  '    block {{\n      dir = in\n      regtype = reg\n      addr = 0\n      count = 1\n      value p {{ reg = 0  type = s16  {val} }}\n    }}\n  }}\n}}\n',
    'modbus_rtu': 'modbus_rtu {{\n  interface = "{tty}"\n  slave {{\n    id = 1\n    interval = 1000\n    {sec}\n'
                  '    block {{\n      dir = in\n      regtype = reg\n      addr = 0\n      count = 1\n      value p {{ reg = 0  type = s16  {val} }}\n    }}\n  }}\n}}\n',
}


def check_inheritance(kind):
    with Env() as env:
        tty = ''
        if kind == 'can':
            env.add(CanBus())       # skips without vcan0
        if kind == 'modbus_rtu':
            tty = env.modbus_rtu().path

        def conf(sec, val):
            return INPUTS[kind].format(sec=sec, val=val, tty=tty) + meter()

        assert load(conf('stale_timeout = 1000', '')) == [], 'section timeout not inherited'
        errs = load(conf('stale_timeout = 1000', 'stale_timeout = 0'))
        assert re.search(NO_TIMEOUT, errs[0]), 'value 0 does not override the section: %s' % errs
        assert load(conf('', 'stale_timeout = 1000')) == [], 'value timeout not used'
        errs = load(conf('', ''))
        assert re.search(NO_TIMEOUT, errs[0]), errs
        errs = load(conf('', 'stale_timeout = -1'))
        assert "stale_timeout invalid" in errs[0], errs
        errs = load(conf('stale_timeout = -1', ''))
        assert "stale_timeout invalid" in errs[0], 'negative section timeout not inherited: %s' % errs


def test_inheritance_json():
    check_inheritance('json')


def test_inheritance_mqtt():
    check_inheritance('mqtt')


def test_inheritance_can():
    check_inheritance('can')


def test_inheritance_modbus_tcp():
    check_inheritance('modbus_tcp')


def test_inheritance_modbus_rtu():
    check_inheritance('modbus_rtu')


JSON = 'json {\n  url = "http://127.0.0.1:1/x"\n  interval = 1000\n  value p { path = "p"  stale_timeout = 1000 }\n  value t { path = "t" }\n}\n'
MQTT = 'mqtt {\n  host = "127.0.0.1"\n  port = 1\n  %s\n}\n'


def test_power_sources_need_timeout():
    for key in ('power_l1', 'power_l2', 'power_l3', 'power'):
        errs = load(JSON + meter('p', '%s = "t"' % key))
        assert re.search(r"value 't' of json .* has no stale_timeout, but is read by sunspec meter 'm'", errs[0]), (key, errs)
    errs = load(JSON + 'counter c { source = t  integrate_power = true }\n')
    assert re.search(r"value 't' of json .* has no stale_timeout, but is read by counter 'c'", errs[0]), errs


def test_other_readers_need_no_timeout():
    others = ' '.join('%s = "t"' % k for k in ('current_l1', 'voltage_l2', 'pf_l3', 'energy_import', 'energy_export_l1', 'frequency'))
    assert load(JSON + meter('p', others)) == []
    logger = 'logger log {\n    topic = "log"\n    value t { }\n  }'
    assert load(JSON + MQTT % logger) == []
    assert load(JSON + 'counter c { source = t }\n') == []
    # computed power sources are not checked
    ev = 'eval e {\n  value s { expr = "t * 2" }\n}\n'
    assert load(JSON + ev + meter('s')) == []
    assert load(JSON + ev + 'counter c { source = s  integrate_power = true }\n') == []


def test_config_errors():
    errs = load(JSON + MQTT % 'value nope { dir = out  type = number  topic = "o"  fmt = "%.1f" }')
    assert errs[0] == "value 'nope' is not produced by any module.", errs
    errs = load(JSON.replace('value t { path = "t" }', 'value t { path = "t"  init_value = nan }'))
    assert "init_value invalid" in errs[0], errs
    errs = load(JSON.replace('interval = 1000', 'interval = 0'))
    assert re.search(r"json '.*': interval invalid\.", errs[0]), errs
    errs = load(JSON + 'modbus_tcp {\n  ip = "127.0.0.1"\n  slave {\n    id = 3\n    block { dir = out  regtype = reg  addr = 0  count = 1  value t { reg = 0  type = s16 } }\n  }\n}\n')
    assert errs[0] == "modbus slave 3: interval not given or invalid.", errs
    errs = load(JSON + 'eval e {\n  value t { expr = "1" }\n}\n')
    assert re.search(r"value 't' of eval 'e' is already produced by json", errs[0]), errs


def test_config_checks():
    """Missing or invalid options are refused, naming the section or value."""
    def tcp(slave):
        return 'modbus_tcp {\n  ip = "127.0.0.1"\n  slave {\n    %s\n  }\n}\n' % slave

    def block(val, head='dir = out  regtype = reg  addr = 0  count = 1'):
        return 'id = 1\n    interval = 1000\n    block { %s  %s }' % (head, val)

    def frame(body):
        return 'can {\n  interface = "vcan0"\n  frame {\n    %s\n  }\n}\n' % body

    cases = [
        (MQTT % 'value t { type = number  topic = "o"  fmt = "%.1f" }', r"mqtt value 't': dir not given\."),
        (MQTT % 'value t { dir = out  topic = "o" }', r"mqtt value 't': type not given\."),
        (MQTT % 'value t { dir = out  type = switch }', r"mqtt value 't': topic not given\."),
        (MQTT % 'value t { dir = out  type = switch  topic = "o"  qos = 3 }', r"mqtt value 't': qos invalid\."),
        (MQTT % 'qos = 3', r"mqtt '127\.0\.0\.1': qos invalid\."),
        (frame('dir = out\n    value t { type = u8  pos = 0 }'), r"can 'vcan0': frame can_id not given or invalid"),
        (frame('can_id = 0x800\n    dir = out\n    value t { type = u8  pos = 0 }'), r"can 'vcan0': frame can_id not given or invalid"),
        (frame('can_id = 0x123\n    value t { type = u8  pos = 0 }'), r"can 'vcan0': frame 0x123: dir not given\."),
        (frame('can_id = 0x123\n    dir = out\n    value t { type = u8  pos = 0  scale = 0 }'), r"CAN value 't': scale of an output must not be 0\."),
        (frame('can_id = "ANA:64:0"\n    dir = out\n    value t { type = u8  pos = 0 }'), r"Failed to parse config file"),
        (frame('can_id = "ANA:1:32"\n    dir = out\n    value t { type = u8  pos = 0 }'), r"Failed to parse config file"),
        (frame('can_id = "ANA:1:5"\n    dir = out\n    value t { type = u8  pos = 0 }'), r"Failed to parse config file"),
        (frame('can_id = "DIG:64"\n    dir = out\n    value t { type = bit  pos = 0 }'), r"Failed to parse config file"),
        (JSON.replace('interval = 1000', 'timeout = 0'), r"json '.*': timeout invalid\."),
        (tcp(block('value t { reg = 0  type = s16  scale = 0 }')), r"modbus value 't': scale of an output must not be 0\."),
        (tcp(block('value t { reg = 0  type = s16 }', 'dir = out  addr = 7  count = 1')), r"modbus slave 1 block \(addr 7\): regtype not given\."),
        (tcp(block('value t { reg = 0  type = s16 }').replace('id = 1', 'id = 248')), r"modbus slave id 248 not given or invalid\."),
        ('modbus_rtu {\n  interface = "/dev/null"\n  slave {\n    %s\n  }\n}\n' % block('value t { reg = 0  type = s16 }').replace('id = 1', 'id = 0'),
         r"modbus slave id 0 not given or invalid\."),
        # required options of every module
        (MQTT.replace('host = "127.0.0.1"\n', '') % '', r"mqtt host name not given\."),
        (MQTT % 'value t { dir = out  type = number  topic = "o" }', r"mqtt value 't': fmt not given\."),
        (MQTT % 'logger log {\n    value t { }\n  }', r"mqtt logger 'log': topic not given\."),
        ('can {\n  frame {\n    can_id = 0x123\n    dir = out\n    value t { type = u8  pos = 0 }\n  }\n}\n', r"CAN interface name not given\."),
        ('modbus_rtu {\n  slave {\n    %s\n  }\n}\n' % block('value t { reg = 0  type = s16 }'), r"modbus_rtu interface name not given\."),
        (tcp(block('value t { reg = 0  type = s16 }')).replace('ip = "127.0.0.1"\n', ''), r"modbus_tcp ip not given\."),
        (tcp(block('value t { reg = 0  type = s16 }', 'dir = out  regtype = reg  count = 1')), r"modbus slave 1: block addr not given\."),
        (tcp(block('value t { reg = 0  type = s16 }', 'regtype = reg  addr = 0  count = 1')), r"modbus slave 1 block \(addr 0\): dir not given\."),
        (tcp(block('value t { reg = 0  type = s16 }', 'dir = out  regtype = reg  addr = 0')), r"modbus slave 1 block \(addr 0\): count not given or invalid\."),
        (tcp(block('value t { reg = 0 }')), r"modbus value 't' type not given\."),
        (meter().replace('unit_id = 1  ', ''), r"sunspec meter 'm' unit_id not given or invalid\."),
        ('counter c { }\n', r"counter 'c': source not given\."),
        ('eval e {\n  value s { }\n}\n', r"eval 'e': value 's': expr not given\."),
    ]
    for conf, pattern in cases:
        errs = load(JSON + conf)
        assert re.search(pattern, errs[0]), (conf, errs)
    errs = load(JSON.replace('url = "http://127.0.0.1:1/x"\n', ''))
    assert re.search(r"json url name not given\.", errs[0]), errs
    errs = load(JSON.replace('value t { path = "t" }', 'value t { }'))
    assert re.search(r"json value 't': path not given\.", errs[0]), errs


def test_duplicate_titles():
    """A title used twice in one section is refused instead of merged."""
    cases = [
        JSON.replace('value t { path = "t" }', 'value t { path = "t" }\n  value t { path = "u" }'),
        JSON + MQTT % 'value t { dir = out  type = switch  topic = "a" }\n  value t { dir = out  type = switch  topic = "b" }',
        JSON + 'counter c { source = t }\ncounter c { source = p  integrate_power = true }\n',
        JSON + meter().replace('meter m {', 'meter m {\n    unit_id = 2\n  }\n  meter m {'),
        JSON + 'can {\n  interface = "vcan0"\n  frame {\n    can_id = 0x123\n    dir = out\n    value t { type = u8  pos = 0 }\n    value t { type = u8  pos = 1 }\n  }\n}\n',
        JSON + 'modbus_tcp {\n  ip = "127.0.0.1"\n  slave {\n    id = 1\n    interval = 1000\n    block {\n      dir = out  regtype = reg  addr = 0  count = 2\n'
               '      value t { reg = 0  type = s16 }\n      value t { reg = 1  type = s16 }\n    }\n  }\n}\n',
    ]
    for c in cases:
        errs = load(c)
        assert errs and errs[0].startswith('Failed to parse config file'), (c, errs)

    # the same output name in different blocks is no duplicate
    errs = load(JSON + 'modbus_tcp {\n  ip = "127.0.0.1"\n  port = 1\n  slave {\n    id = 1\n    interval = 1000\n'
                '    block { dir = out  regtype = reg  addr = 0  count = 1  value t { reg = 0  type = s16 } }\n'
                '    block { dir = out  regtype = reg  addr = 5  count = 1  value t { reg = 0  type = s16 } }\n  }\n}\n')
    assert errs == [], errs


def test_removed_options():
    """Removed options fail to load (libconfuse: unknown option)."""
    cases = [
        JSON + 'counter c { source = p  integrate_power = true  max_gap = 1000 }\n',
        JSON + MQTT % 'logger log {\n    topic = "log"\n    stale_timeout = 600\n    value t { }\n  }',
        JSON + meter().replace('port =', 'stale_timeout = 10000\n  port ='),
        JSON + 'eval e {\n  max_age = 1000\n  value s { expr = "t" }\n}\n',
        JSON + 'eval e {\n  value s { expr = "t"  init = 1 }\n}\n',
    ]
    for c in cases:
        errs = load(c)
        assert errs and errs[0].startswith('Failed to parse config file'), errs


def test_production_config():
    """../uvrgw.conf (untracked) loads and runs, with all connections
    redirected to local fakes: MQTT to a local broker, HTTP and Modbus
    TCP to closed local ports, CAN to vcan0, Modbus RTU to a pty (RS232
    mode, a pty has no RS485), SunSpec to free local ports."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'uvrgw.conf')
    if not os.path.exists(path):
        raise Skip('no uvrgw.conf')
    with Env() as env:
        env.add(CanBus())
        broker = env.broker()
        rtu = env.modbus_rtu()
        out, section = [], None
        for line in open(path).read().split('\n'):
            m = re.match(r'^(\w+)\b.*\{', line)
            if m:
                section = m.group(1)
            if re.match(r'^\s*(user|pwd)\s*=', line):
                continue
            line = re.sub(r'^(\s*host\s*=\s*).*', r'\1"127.0.0.1"', line)
            line = re.sub(r'^(\s*(ip|bind)\s*=\s*).*', r'\1"127.0.0.1"', line)
            line = re.sub(r'^(\s*url\s*=\s*).*', r'\1"http://127.0.0.1:1/"', line)
            line = re.sub(r'^(\s*interface\s*=\s*)"can\w*"', r'\1"vcan0"', line)
            line = re.sub(r'^(\s*interface\s*=\s*)"/dev/\w+"', r'\1"%s"' % rtu.path, line)
            line = re.sub(r'^(\s*mode\s*=\s*)rs485', r'\1rs232', line)
            line = re.sub(r'^(\s*rts\s*=\s*)\w+', r'\1none', line)
            if section == 'mqtt':
                line = re.sub(r'^(\s*port\s*=\s*)\d+', r'\g<1>%d' % broker.port, line)
            elif section in ('sunspec_server', 'modbus_tcp'):
                line = re.sub(r'^(\s*port\s*=\s*)\d+', lambda m: '%s%d' % (m.group(1), free_port() if section == 'sunspec_server' else 1), line)
            out.append(line)
        conf = '\n'.join(out)
        code = re.sub(r'#.*', '', conf)
        assert not re.search(r'\d+\.\d+\.\d+\.\d+', code.replace('127.0.0.1', '')), 'production address left in the test config'
        u = env.uvrgw(conf)
        u.wait_log('mqtt connected')
        assert u.wait_exit(2) is None, 'exited: %s' % u.errors()
        u.stop()
