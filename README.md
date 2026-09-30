# uvrgw — UVR Gateway

`uvrgw` is a Linux daemon written in C that bridges multiple industrial and home-automation protocols.  It reads a single configuration file and sets up bidirectional data routing between **CAN bus** (UVR protocol), **Modbus** (RTU and TCP), **MQTT**, and **REST/JSON** data sources.

---

## Features

- **CAN bus** (Linux SocketCAN) — receive and transmit CAN frames with typed values (`bit`, `u8`, `s8`, `u16`, `s16`, `u32`, `s32`).  Supports periodic NTP-synced timestamp injection on CAN ID `0x100`.  UVR-specific `ANA:node:chan` and `DIG:node` shorthand for CAN IDs.
- **Modbus RTU and TCP** — poll Modbus slaves on configurable intervals; read holding registers, input registers, coils and discrete inputs; write holding registers and coils.  Supports 16/32-bit integer (`s16`, `u16`, `s32`, `u32`), 32-bit float (`f32`), bit and bitmask value types with an optional per-value scale-factor register.
- **MQTT** (via libmosquitto) — publish and subscribe with configurable topics, QoS, retain flag and a last-will state topic.  Value types: `number` (printf-style format string), `switch` (`ON`/`OFF`), `contact` (`OPEN`/`CLOSED`).
- **REST/JSON** (via libcurl + json-c) — periodically HTTP-GET a JSON endpoint and extract values by dot-separated JSON path (arrays by numeric index).  Input only.
- **SunSpec smart meter emulation** — Modbus TCP server serving virtual three-phase meters (SunSpec models 1 + 213, float), e.g. as secondary meters for a Fronius inverter.  Meter quantities are taken from any input by value name; apparent/reactive power, phase-to-phase voltages and totals are derived.
- **Central value dispatch** — values are linked across all protocols by *name*.  When a value arrives on any input it is automatically forwarded to every registered output with the same name, enabling CAN→MQTT, Modbus→MQTT, REST→CAN, etc. without any custom glue code.
- **NTP synchronisation guard** — CAN timestamp frames are only sent when the local NTP daemon reports a synchronised clock.
- Single configuration file, libconfuse-based syntax.
- Clean shutdown on `SIGINT` / `SIGTERM`.

---

## Build prerequisites

The following development libraries must be installed before building:

| Library | Package (Debian/Ubuntu) |
|---------|------------------------|
| libconfuse (config-file parsing) | `libconfuse-dev` |
| libmodbus (Modbus protocol) | `libmodbus-dev` |
| libmosquitto (MQTT client) | `libmosquitto-dev` |
| libcurl (HTTP client) | `libcurl4-openssl-dev` |
| json-c (JSON parsing) | `libjson-c-dev` |
| pthreads, libm | `build-essential` |

```bash
sudo apt install build-essential libconfuse-dev libmodbus-dev \
                 libmosquitto-dev libcurl4-openssl-dev libjson-c-dev
```

---

## Building

```bash
make
```

Install to `/usr/bin/uvrgw` and the systemd unit to `/lib/systemd/system/uvrgw.service` (requires root):

```bash
sudo make install
```

---

## Running as service

The systemd unit runs uvrgw as unprivileged user `uvrgw` (group `dialout` for the Modbus RTU serial port) and grants `CAP_NET_BIND_SERVICE`, so the SunSpec meter emulation can listen on port 502.  The configuration file contains credentials, so make it readable for the service user only:

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin uvrgw
sudo chown root:uvrgw /etc/uvrgw.conf
sudo chmod 640 /etc/uvrgw.conf
sudo systemctl daemon-reload
sudo systemctl enable --now uvrgw
```

---

## Usage

```
uvrgw [config-file]
```

If no config file is given the default path `/etc/uvrgw.conf` is used.

---

## Configuration

The configuration file uses [libconfuse](https://github.com/libconfuse/libconfuse) syntax.  Top-level sections are `mqtt {}`, `json {}`, `can {}`, `modbus_rtu {}` and `modbus_tcp {}`.

Values across different protocol sections are linked by **name**: giving two values the same name in any combination of sections causes the value dispatcher to forward every received value to all registered outputs with that name.

### MQTT

```
mqtt {
  host           = "mqtt.example.com"
  port           = 1883
  client_id      = "uvrgw"
  user           = "myuser"       # optional
  pwd            = "mypassword"   # optional
  state_topic    = "uvrgw/state"  # publishes ON/OFF as last-will
  keepalive_period = 300
  qos            = 0
  retain         = false

  # Publish a numeric value (CAN or Modbus input → MQTT output)
  value "outdoor_temp" {
    dir   = out
    type  = number
    topic = "sensors/outdoor/temperature"
    fmt   = "%.1f"
  }

  # Subscribe and forward to CAN/Modbus (MQTT input → other output)
  value "heating_setpoint" {
    dir   = in
    type  = number
    topic = "heating/setpoint"
  }

  # Switch-type value: publishes "ON" or "OFF"
  value "pump_state" {
    dir   = out
    type  = switch
    topic = "heating/pump"
  }

  # Contact-type value: publishes "OPEN" or "CLOSED"
  value "window_contact" {
    dir   = out
    type  = contact
    topic = "sensors/window"
  }
}
```

### REST / JSON

```
json {
  url      = "http://192.168.1.10/api/status"
  interval = 5000    # poll every 5 000 ms
  timeout  = 3000    # HTTP timeout in ms
  user     = "admin" # optional Basic-Auth
  pwd      = "secret"

  # Extract a nested value: {"sensors": {"temp": 21.5}}
  value "outdoor_temp" {
    path   = "sensors.temp"
    scale  = 1.0
    offset = 0.0
  }

  # Extract from an array: {"readings": [0, 42.0]}
  value "flow_rate" {
    path  = "readings.1"
    scale = 0.1
  }
}
```

### CAN bus

```
can {
  interface        = "can0"
  timestamp_period = 60000   # send NTP timestamp every 60 s (0 = disabled)
  send_timeout     = 1000    # coalesce output writes within 1 000 ms

  # Receive frame (CAN → value dispatcher)
  frame {
    can_id = "ANA:1:1"   # UVR analogue node 1, channel 1  (or 0x201, or decimal)
    dir    = in

    value "outdoor_temp" {
      type   = s16
      pos    = 0       # byte offset in the 8-byte CAN data field
      scale  = 0.1
      offset = 0.0
    }
  }

  # Transmit frame (value dispatcher → CAN)
  frame {
    can_id = "DIG:2"   # UVR digital node 2  (or 0x182, or decimal)
    dir    = out

    value "pump_state" {
      type = bit
      pos  = 0         # bit position (0–63) within the 8-byte data field
    }
  }
}
```

**CAN ID shorthands:**

| Syntax | Meaning | Example |
|--------|---------|---------|
| `ANA:node:chan` | UVR analogue frame | `ANA:1:1` → `0x201` |
| `DIG:node` | UVR digital frame | `DIG:2` → `0x182` |
| `0x…` | Hexadecimal literal | `0x1FF` |
| decimal | Decimal literal | `511` |

### Modbus RTU

```
modbus_rtu {
  interface       = "/dev/ttyUSB0"
  baud            = 19200
  parity          = none        # none / even / odd
  data_bits       = 8
  stop_bits       = 1
  mode            = rs485        # rs232 / rs485
  rts             = none         # none / up / down
  rts_delay       = -1
  separation_time = 100          # ms between transactions
  timeout         = 250          # ms per transaction

  slave {
    id       = 1
    interval = 1000   # poll every 1 000 ms

    block {
      dir     = in
      regtype = inreg    # inreg / reg / inbit / bit
      addr    = 0
      count   = 10

      value "outdoor_temp" {
        reg    = 0       # register index within the block
        type   = s16     # s16 / u16 / s32 / u32 / f32 / bitmask (bit in inbit/bit blocks)
        scale  = 0.1
        offset = 0.0
      }

      # scale_factor: use another register of the same block to supply
      # the decimal exponent
      value "energy" {
        reg          = 2       # 32-bit values occupy 2 registers (2, 3)
        type         = u32
        scale_factor = "energy_exp"
      }

      value "energy_exp" {
        reg  = 4
        type = s16
      }

      # 32-bit float, low word first
      value "power" {
        reg       = 5          # registers 5, 6
        type      = f32
        word_swap = true
      }

      # bitmask: single bit (0-15) of a register
      value "pump_on" {
        reg  = 7
        type = bitmask
        bit  = 0
      }
    }
  }
}
```

### Modbus TCP

```
modbus_tcp {
  ip              = "192.168.1.20"
  port            = 502
  separation_time = 0
  timeout         = 250

  slave {
    id       = 1
    interval = 2000

    block {
      dir     = out
      regtype = reg
      addr    = 100
      count   = 1

      value "heating_setpoint" {
        reg   = 0
        type  = u16
        scale = 10.0   # stored as integer × 10
      }
    }
  }
}
```

**Modbus notes:**

- Each `block` is read with a single request (`count` registers/bits starting at `addr`).
- 32-bit types (`s32`, `u32`, `f32`) occupy two registers, high word first (SunSpec order); set `word_swap = true` for devices sending the low word first.  Outputs of these types are written with function code 16 (write multiple registers).
- `scale_factor` must reference an `s16` value in the same block and is allowed for integer types.
- A float NaN (SunSpec "not implemented") is treated as invalid reading and not forwarded.
- `bit` values are only allowed in `inbit`/`bit` blocks, all other types only in `inreg`/`reg` blocks.  Output blocks must use `bit` or `reg`.
- If a block read fails with a Modbus exception, polling continues with the next block; if the slave does not respond at all, the remaining blocks are skipped until the next poll interval.
- Modbus TCP connections are opened on demand.  If the server is unreachable, the connection is retried every second; after a timeout or I/O error the connection is closed and re-established before the next request.
- Bitmask outputs are written from a local register image that starts at 0 and is not read back from the device.  Writing one bit therefore also writes all other bits of that register — map every relevant bit of such a register as an output.

### SunSpec meter emulation

```
sunspec_server {
  bind          = "0.0.0.0"
  port          = 502      # needs CAP_NET_BIND_SERVICE (or root) for ports < 1024
  stale_timeout = 30000    # ms; must be larger than the poll interval of the sources

  meter heatpump {
    unit_id      = 84
    manufacturer = "modusoft"      # max. 32 chars
    model        = "SmartMeterGW"  # max. 32 chars
    serial       = "00000001"      # max. 32 chars
    # options, version: max. 16 chars (optional)

    # per phase sources (value names), all optional
    current_l1       = "hp_i1"     # A
    voltage_l1       = "hp_u1"     # V (phase to neutral)
    power_l1         = "hp_p1"     # W
    pf_l1            = "hp_pf1"    # power factor -1..1 (not percent)
    energy_import_l1 = "hp_imp1"   # Wh
    energy_export_l1 = "hp_exp1"   # Wh
    # ... same for _l2 and _l3

    # totals (optional, default: sum of the phases)
    power         = "hp_p"         # W
    energy_import = "hp_imp"       # Wh
    energy_export = "hp_exp"       # Wh
    frequency     = "grid_freq"    # Hz (optional)
  }
}
```

**SunSpec notes:**

- Each meter answers on its own `unit_id`; requests for other unit IDs are not answered.  Only function code 3 (read holding registers) is supported, registers 40000–40196: `SunS` marker, common model 1, float meter model 213, end model.
- Derived per phase: apparent power `S = V × I`, reactive power `Q = sqrt(|S² − P²|)`, power factor `P / S` if no `pf` source is given, phase-to-phase voltages from the phase-to-neutral voltages (120° phase shift).
- Totals: sum of currents, powers and energies (unless given explicitly), average voltages, total power factor `P / S`.
- Quantities that are neither configured nor derivable (e.g. frequency, VAh, VArh) are served as 0.
- If a configured source value has not been updated within `stale_timeout`, requests for that meter are answered with exception 4 (server device failure), so the client keeps its last data instead of seeing wrong values (e.g. energy counters dropping to 0).  State changes are logged.
- Energy counters are served as float32 (SunSpec model 213), exact to 1 Wh up to 16.7 MWh.

---

## Architecture overview

```
         ┌─────────────────────────────────────────────┐
         │                 uvrgw process                │
         │                                              │
         │  main thread                                 │
         │  ┌──────────────────────────────────────┐   │
         │  │  select() event loop                 │   │
         │  │  • exit eventfd (SIGINT/SIGTERM)      │   │
         │  │  • CAN socket(s) RX                  │   │
         │  └──────────────────────────────────────┘   │
         │                                              │
         │  per-CAN-interface TX thread                 │
         │  per-Modbus-master polling thread            │
         │  per-REST-endpoint polling thread            │
         │  per-SunSpec-server thread                   │
         │  per-MQTT-connection background thread       │
         │         (managed by libmosquitto)            │
         │                                              │
         │  Value dispatch (uvrgw_conf)                 │
         │  ┌──────────────────────────────────────┐   │
         │  │  name → [cb1, cb2, …]                │   │
         │  │  any input fires all registered       │   │
         │  │  output callbacks with same name      │   │
         │  └──────────────────────────────────────┘   │
         └─────────────────────────────────────────────┘
```

- **Event loop** (`main.c`): a single `select()` call waits on all CAN sockets and the exit eventfd.  CAN RX is handled in the main thread; everything else runs in dedicated threads.
- **Value dispatch** (`uvrgw_conf.c`): a linked list of named dispatchers, each holding an array of `(val, callback)` pairs registered during startup.  When a value arrives the dispatcher stores it with its update time (readable via `uvrgw_conf_get_val()`, e.g. to detect stale values) and calls every callback whose `val` pointer differs from the source, preventing loopback.  NaN values are dropped.
- **CAN TX thread** (`can.c`): wakes every 20 ms, checks for pending outbound frames and the timestamp timer.
- **Modbus thread** (`mb.c`): wakes every 10 ms, polls slaves round-robin and writes queued output values.
- **REST thread** (`rest.c`): wakes every 100 ms, checks each endpoint's poll interval and performs HTTP GET.
- **SunSpec server thread** (`sunspec.c`): waits on the listening socket and client connections (`select()` with 100 ms timeout) and answers requests from a register image rebuilt from the dispatcher's last values.
- **MQTT** (`mqtt.c`): libmosquitto manages its own background thread for connection, keep-alive and message delivery.

---

## Signal handling

| Signal | Effect |
|--------|--------|
| `SIGINT` | Clean shutdown (drains all threads, closes sockets) |
| `SIGTERM` | Clean shutdown (same as SIGINT) |
| `SIGHUP` | Currently a no-op (reserved for future config reload) |

Shutdown is triggered by writing to a Linux `eventfd`, which is monitored by the main `select()` loop.

---

## License

No license is currently specified in this repository.
