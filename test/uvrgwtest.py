# SPDX-License-Identifier: GPL-3.0-or-later
"""Helpers for the uvrgw test suite (Python standard library only).

Every test runs uvrgw as a subprocess with a generated config and talks to
it through the real protocols: a local mosquitto broker and a raw MQTT
client, a fake JSON server, fake Modbus TCP and RTU (pty) servers, vcan0
for CAN, and Modbus TCP reads of the SunSpec meters.  syslog() is
redirected to stderr by shim.c, so the tests can follow the log.
"""
import json
import os
import pty
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Skip(Exception):
    """Raised by a test that cannot run on this machine."""


class Settings:
    binary = None       # uvrgw binary
    shim = None         # compiled shim.so
    tmp = None          # temp directory of this run
    valgrind = False
    slow = False

    def t(self, seconds):
        """Scale a timeout for valgrind."""
        return seconds * 4 if self.valgrind else seconds


S = Settings()


def wait_until(pred, timeout, what, step=0.05):
    """Poll @p pred until it returns a true value; a connection error
    (server not up yet) counts as false."""
    end = time.monotonic() + S.t(timeout)
    while True:
        try:
            r = pred()
        except OSError:
            r = None
        if r:
            return r
        if time.monotonic() > end:
            raise AssertionError('timeout waiting for ' + what)
        time.sleep(step)


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Env:
    """Collects the resources of a test and closes them at the end."""

    def __init__(self):
        self.res = []

    def add(self, r):
        self.res.append(r)
        return r

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        for r in reversed(self.res):
            try:
                r.close()
            except Exception:
                pass
        return False

    def broker(self):
        return self.add(Broker())

    def mqtt(self, port, subscribe=None):
        return self.add(MqttClient(port, subscribe))

    def json(self, data=None):
        return self.add(JsonServer(data))

    def modbus_tcp(self):
        return self.add(ModbusTcpServer())

    def modbus_rtu(self):
        return self.add(ModbusRtuServer())

    def can(self):
        return self.add(CanBus())

    def uvrgw(self, conf, **kw):
        return self.add(Uvrgw(conf, **kw))


# ---------------------------------------------------------------- uvrgw

class Uvrgw:
    """uvrgw process; its stderr (log) is collected as (ms, text) lines."""

    def __init__(self, conf, pub=False, state_dir=None):
        self.dir = tempfile.mkdtemp(dir=S.tmp, prefix='uvrgw-')
        self.state_dir = state_dir or os.path.join(self.dir, 'state')
        os.makedirs(self.state_dir, exist_ok=True)
        self.conf = os.path.join(self.dir, 'uvrgw.conf')
        with open(self.conf, 'w') as f:
            f.write('state_dir = "%s"\n%s' % (self.state_dir, conf))
        cmd = [S.binary, self.conf]
        self.vg_log = os.path.join(self.dir, 'valgrind.txt')
        if S.valgrind:
            supp = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'valgrind.supp')
            cmd = ['valgrind', '--leak-check=full', '--errors-for-leak-kinds=definite,indirect',
                   '--error-exitcode=99', '--suppressions=' + supp, '--log-file=' + self.vg_log] + cmd
        env = dict(os.environ, LD_PRELOAD=S.shim)
        if pub:
            env['UVRGW_SHIM_PUB'] = '1'
        self.lines = []
        self.p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, env=env)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.p.stderr:
            m = re.match(r'\[(\d+)\] (.*)', line.rstrip('\n'))
            self.lines.append((int(m.group(1)), m.group(2)) if m else (None, line.rstrip('\n')))

    def text(self):
        return [t for _, t in list(self.lines)]

    def mark(self):
        """Position in the log, for wait_log(since=...)."""
        return len(self.lines)

    def find(self, pattern, since=0):
        return [t for _, t in list(self.lines)[since:] if re.search(pattern, t)]

    def wait_log(self, pattern, timeout=5, since=0):
        return wait_until(lambda: self.find(pattern, since), timeout, 'log ' + repr(pattern))[0]

    def errors(self):
        return [t[4:] for t in self.text() if t.startswith('<3> ')]

    def wait_exit(self, timeout):
        try:
            return self.p.wait(timeout=S.t(timeout))
        except subprocess.TimeoutExpired:
            return None

    def stop(self):
        """Stop with SIGINT; fails on a hang, a non-zero exit or valgrind errors."""
        if self.p.poll() is None:
            self.p.send_signal(signal.SIGINT)
        try:
            rc = self.p.wait(timeout=S.t(15))
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
            raise AssertionError('shutdown hang')
        self.reader.join(5)
        self.check_valgrind(rc)
        assert rc == 0, 'exit code %s, errors: %s' % (rc, self.errors())
        return rc

    def check_valgrind(self, rc):
        if S.valgrind:
            summary = open(self.vg_log).read()
            assert rc != 99 and 'ERROR SUMMARY: 0 errors' in summary, 'valgrind errors, see ' + self.vg_log

    def close(self):
        if self.p.poll() is None:
            self.p.kill()
            self.p.wait()
        self.reader.join(5)
        with open(os.path.join(self.dir, 'log.txt'), 'w') as f:
            f.write('\n'.join('[%s] %s' % l for l in self.lines) + '\n')


def load(conf):
    """Start uvrgw with @p conf and return its config errors.

    A config that loads keeps running and is stopped after a moment (an
    empty list is returned); a config error exits with code 1 and its
    LOG_ERR messages are returned.
    """
    with Env() as env:
        u = env.uvrgw(conf)
        rc = u.wait_exit(2)
        if rc is None:
            u.stop()
            return []
        u.reader.join(5)
        u.check_valgrind(rc)
        assert rc == 1, 'exit code %s' % rc
        return u.errors() or ['(exit 1 without message)']


# ---------------------------------------------------------------- MQTT

class Broker:
    """Local mosquitto broker on a free port."""

    def __init__(self):
        self.port = free_port()
        self.dir = tempfile.mkdtemp(dir=S.tmp, prefix='broker-')
        conf = os.path.join(self.dir, 'mosquitto.conf')
        with open(conf, 'w') as f:
            f.write('listener %d 127.0.0.1\nallow_anonymous true\n' % self.port)
        exe = shutil.which('mosquitto') or '/usr/sbin/mosquitto'
        self.p = subprocess.Popen([exe, '-c', conf], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        def up():
            try:
                socket.create_connection(('127.0.0.1', self.port), timeout=0.2).close()
                return True
            except OSError:
                return False
        wait_until(up, 5, 'mosquitto')

    def close(self):
        self.p.terminate()
        self.p.wait()


def _rl(n):
    out = b''
    while True:
        d, n = n % 128, n // 128
        out += bytes([d | (0x80 if n else 0)])
        if not n:
            return out


def _s16(b):
    return struct.pack('>H', len(b)) + b


def _pkt(t, body):
    return bytes([t]) + _rl(len(body)) + body


class MqttClient:
    """Minimal MQTT 3.1.1 client (QoS 0) collecting received messages."""

    def __init__(self, port, subscribe=None):
        self.msgs = []      # (ms, topic, payload)
        self.sock = socket.create_connection(('127.0.0.1', port))
        cid = 'test-%d-%d' % (os.getpid(), id(self))
        self.sock.sendall(_pkt(0x10, _s16(b'MQTT') + b'\x04\x02\x00\x00' + _s16(cid.encode())))
        self._recv_packet()
        if subscribe:
            self.sock.sendall(_pkt(0x82, b'\x00\x01' + _s16(subscribe.encode()) + b'\x00'))
            self._recv_packet()
        threading.Thread(target=self._rx, daemon=True).start()

    def _recv(self, n):
        b = b''
        while len(b) < n:
            d = self.sock.recv(n - len(b))
            if not d:
                raise EOFError
            b += d
        return b

    def _recv_packet(self):
        h = self._recv(1)[0]
        n, m = 0, 1
        while True:
            b = self._recv(1)[0]
            n += (b & 127) * m
            m *= 128
            if not b & 128:
                break
        return h, self._recv(n)

    def _rx(self):
        try:
            while True:
                h, body = self._recv_packet()
                if h >> 4 == 3:
                    n = struct.unpack('>H', body[:2])[0]
                    pos = 2 + n + (2 if h & 0x06 else 0)
                    self.msgs.append((int(time.monotonic() * 1000), body[2:2 + n].decode(), body[pos:].decode()))
        except (EOFError, OSError):
            pass

    def pub(self, topic, payload):
        self.sock.sendall(_pkt(0x30, _s16(topic.encode()) + payload.encode()))

    def mark(self):
        return len(self.msgs)

    def payloads(self, topic, since=0):
        return [p for _, t, p in self.msgs[since:] if t == topic]

    def wait(self, topic, pred=lambda p: True, timeout=5, since=0):
        """Wait for a message on @p topic whose payload matches @p pred."""
        def find():
            for p in self.payloads(topic, since):
                if pred(p):
                    return [p]
        return wait_until(find, timeout, 'mqtt %s' % topic)[0]

    def wait_value(self, topic, value, timeout=5, since=0):
        return self.wait(topic, lambda p: float(p) == value, timeout, since)

    def snapshot(self, topic, pred, timeout=5, since=0):
        """Wait for a logger snapshot (JSON) matching @p pred."""
        return json.loads(self.wait(topic, lambda p: pred(json.loads(p)), timeout, since))

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()


# ---------------------------------------------------------------- JSON

class JsonServer:
    """HTTP server returning @c data as JSON; data None answers 404."""

    def __init__(self, data=None):
        self.data = data
        server = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                d = server.data
                if d is None:
                    self.send_error(404)
                    return
                b = json.dumps(d).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(b)))
                self.end_headers()
                self.wfile.write(b)

            def log_message(self, *a):
                pass

        self.httpd = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.url = 'http://127.0.0.1:%d/d.json' % self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


# ---------------------------------------------------------------- Modbus

def f32_regs(f):
    hi, lo = struct.unpack('>HH', struct.pack('>f', f))
    return [hi, lo]


def regs_f32(regs):
    return struct.unpack('>f', struct.pack('>HH', *regs))[0]


def crc16(data):
    crc = 0xffff
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xa001 if crc & 1 else crc >> 1
    return struct.pack('<H', crc)


class ModbusRegs:
    """Register image of a fake Modbus device (any unit id).

    @c fail makes the device fail every request (TCP: exception 4, RTU:
    no answer).  Writes are recorded in @c writes as (addr, [values]).
    """

    def __init__(self):
        self.holding = {}
        self.inputs = {}
        self.writes = []
        self.fail = False
        self.lock = threading.Lock()

    def set(self, addr, values, inputs=False):
        with self.lock:
            regs = self.inputs if inputs else self.holding
            for i, v in enumerate(values):
                regs[addr + i] = v & 0xffff

    def handle(self, pdu):
        fc = pdu[0]
        with self.lock:
            if fc in (3, 4):
                addr, count = struct.unpack('>HH', pdu[1:5])
                regs = self.holding if fc == 3 else self.inputs
                vals = [regs.get(addr + i, 0) for i in range(count)]
                return bytes([fc, 2 * count]) + struct.pack('>%dH' % count, *vals)
            if fc == 6:
                addr, v = struct.unpack('>HH', pdu[1:5])
                self.holding[addr] = v
                self.writes.append((addr, [v]))
                return pdu[:5]
            if fc == 16:
                addr, count = struct.unpack('>HH', pdu[1:5])
                vals = list(struct.unpack('>%dH' % count, pdu[6:6 + 2 * count]))
                for i, v in enumerate(vals):
                    self.holding[addr + i] = v
                self.writes.append((addr, vals))
                return pdu[:5]
        return bytes([fc | 0x80, 1])


class ModbusTcpServer(ModbusRegs):
    def __init__(self):
        super().__init__()
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.conns = []
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            self.conns.append(c)
            threading.Thread(target=self._serve, args=(c,), daemon=True).start()

    def _serve(self, c):
        try:
            while True:
                hdr = c.recv(7, socket.MSG_WAITALL)
                if len(hdr) < 7:
                    return
                tid, pid, n, unit = struct.unpack('>HHHB', hdr)
                pdu = c.recv(n - 1, socket.MSG_WAITALL)
                resp = bytes([pdu[0] | 0x80, 4]) if self.fail else self.handle(pdu)
                c.sendall(struct.pack('>HHHB', tid, 0, len(resp) + 1, unit) + resp)
        except OSError:
            pass

    def close(self):
        self.sock.close()
        for c in self.conns:
            c.close()


class ModbusRtuServer(ModbusRegs):
    """Modbus RTU device on a pty; uvrgw opens @c path as its interface.

    @c late delays the next answer once by that many seconds.
    """

    def __init__(self):
        super().__init__()
        self.late = 0
        self.master, self.slave = pty.openpty()
        self.path = os.ttyname(self.slave)
        self.running = True
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        buf = b''
        while self.running:
            try:
                if not select.select([self.master], [], [], 0.05)[0]:
                    continue
                d = os.read(self.master, 256)
            except OSError:
                return
            buf += d
            while len(buf) >= 8:
                fc = buf[1]
                n = 9 + buf[6] if fc == 16 else 8
                if len(buf) < n:
                    break
                frame, buf = buf[:n], buf[n:]
                if crc16(frame[:-2]) != frame[-2:]:
                    buf = b''
                    break
                if self.fail:
                    continue
                resp = frame[:1] + self.handle(frame[1:-2])
                late, self.late = self.late, 0
                time.sleep(late)
                os.write(self.master, resp + crc16(resp))

    def close(self):
        if self.running:
            self.running = False
            self.thread.join(5)
            os.close(self.master)
            os.close(self.slave)


def mb_request(port, unit, pdu, timeout=2):
    """Send one Modbus TCP request; returns the response PDU, or None if
    it is not answered within @p timeout."""
    with socket.create_connection(('127.0.0.1', port), timeout=S.t(timeout)) as s:
        s.sendall(struct.pack('>HHHB', 1, 0, len(pdu) + 1, unit) + pdu)
        try:
            hdr = s.recv(7, socket.MSG_WAITALL)
        except socket.timeout:
            return None
        n = struct.unpack('>HHHB', hdr)[2]
        return s.recv(n - 1, socket.MSG_WAITALL)


def mb_read(port, unit, addr, count):
    """Read holding registers; returns the list, or the exception code."""
    pdu = mb_request(port, unit, struct.pack('>BHH', 3, addr, count))
    if pdu[0] & 0x80:
        return pdu[1]
    return list(struct.unpack('>%dH' % count, pdu[2:2 + 2 * count]))


SUNSPEC_W = 40097       # total power W of model 213 (0-based address)


def sunspec_power(port, unit):
    """Total power of a SunSpec meter, or the Modbus exception code (int)."""
    r = mb_read(port, unit, SUNSPEC_W, 2)
    return r if isinstance(r, int) else regs_f32(r)


# ---------------------------------------------------------------- CAN

CAN_IFACE = 'vcan0'


def ana_id(node, chan):
    return 0x200 | (node & 0x3f) | ((chan & 0x0c) << 5) | ((chan & 0x10) << 2)


def dig_id(node):
    return 0x180 | (node & 0x3f)


class CanBus:
    """Raw CAN socket on vcan0 (test skipped if it does not exist)."""

    def __init__(self):
        if not os.path.exists('/sys/class/net/' + CAN_IFACE):
            raise Skip(CAN_IFACE + ' not available')
        self.sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        self.sock.bind((CAN_IFACE,))

    def send(self, can_id, data):
        self.sock.send(struct.pack('=IB3x8s', can_id, len(data), bytes(data).ljust(8, b'\0')))

    def recv(self, can_id, timeout=5):
        """Next frame with @p can_id (data bytes)."""
        end = time.monotonic() + S.t(timeout)
        while True:
            left = end - time.monotonic()
            if left <= 0:
                raise AssertionError('timeout waiting for CAN id 0x%x' % can_id)
            self.sock.settimeout(left)
            try:
                f = self.sock.recv(16)
            except socket.timeout:
                continue
            cid, n, data = struct.unpack('=IB3x8s', f)
            if cid == can_id:
                return data[:n]

    def close(self):
        self.sock.close()
