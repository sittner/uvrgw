# uvrgw — UVR Gateway

`uvrgw` is a Linux daemon written in C that bridges multiple industrial and home-automation protocols.  It reads a single configuration file and sets up bidirectional data routing between **CAN bus** (UVR protocol), **Modbus** (RTU and TCP), **MQTT**, and **REST/JSON** data sources.

---

## Features

- **CAN bus** (Linux SocketCAN) — receive and transmit CAN frames with typed values (`bit`, `u8`, `s8`, `u16`, `s16`, `u32`, `s32`).  Supports periodic timestamp injection on CAN ID `0x100`.  UVR-specific `ANA:node:chan` and `DIG:node` shorthand for CAN IDs.
- **Modbus RTU and TCP** — poll Modbus slaves on configurable intervals; read holding registers, input registers, coils and discrete inputs; write holding registers and coils.  Supports 16/32-bit integer (`s16`, `u16`, `s32`, `u32`), 32-bit float (`f32`), bit and bitmask value types with an optional per-value scale-factor register.
- **MQTT** (via libmosquitto) — publish and subscribe with configurable topics, QoS, retain flag and a last-will state topic.  Value types: `number` (printf-style format string), `switch` (`ON`/`OFF`), `contact` (`OPEN`/`CLOSED`).
- **REST/JSON** (via libcurl + json-c) — periodically HTTP-GET a JSON endpoint and extract values by dot-separated JSON path (arrays by numeric index).  Input only.
- **SunSpec smart meter emulation** — Modbus TCP server serving virtual three-phase meters (SunSpec models 1 + 213, float), e.g. as secondary meters for a Fronius inverter.  Meter quantities are taken from any input by value name; apparent/reactive power, phase-to-phase voltages and totals are derived.
- **Persistent counters** — monotonic energy counters from device counters (with reset detection and plausibility check) or by integrating power; state survives restarts and device counter resets.
- **Calculated values and control logic** — `eval` sections define values by expressions over other values (arithmetic, comparison, logic, hysteresis, ...), evaluated periodically or when input values arrive.
- **Central value dispatch** — values are linked across all protocols by *name*.  When a value arrives on any input it is automatically forwarded to every registered output with the same name, enabling CAN→MQTT, Modbus→MQTT, REST→CAN, etc. without any custom glue code.
- **Stalled data handling** — an input value can fall back to a configured value when its source stops delivering (`stale_timeout`); the logger writes `null` and SunSpec meters report a failure for such values.
- **Clock synchronisation guard** — CAN timestamp frames are only sent and logger snapshots only taken while the system clock is synchronised (kernel time status, maintained by ntpd, chrony or systemd-timesyncd).
- **Raspberry Pi image** — Buildroot SD card image for a Pi Zero W with RS485/CAN HAT: read-only root, WiFi, A/B root slots with RAUC updates and U-Boot fallback.
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

## Testing

```bash
make test
```

The test suite (`test/`, Python 3 standard library only) runs the built binary against local fakes of every device type: a mosquitto broker, a JSON HTTP server, Modbus TCP and Modbus RTU (on a pty) devices, CAN on a `vcan0` interface and Modbus TCP reads of the SunSpec meters.  It covers the config checks, `stale_timeout` / `init_value` inheritance of all input sections, timeouts and recovery of every input type, the consumers (logger, SunSpec register map and client limit, counters, eval), outputs, MQTT loopback, device failures and resets racing with real data.  If `uvrgw.conf` exists in the source directory, it is loaded and run as well, with all connections redirected to the local fakes.

Requirements: `gcc`, `python3`, `mosquitto` (the broker binary, not running as a service is fine), `valgrind` for `--valgrind`, and for the CAN tests a virtual CAN interface (skipped without it):

```bash
sudo ip link add vcan0 type vcan
sudo ip link set vcan0 up
```

Options (`python3 test/run.py --help`): `--valgrind` runs uvrgw under valgrind (memory errors and leaks fail the test), `--slow` runs the race test for 5 minutes instead of 30 s, `-k PATTERN` selects tests by name, and a binary path can be given instead of `./uvrgw`.  The logs of all uvrgw runs are kept in the printed temp directory if a test fails.

---

## Running as service

The systemd unit runs uvrgw as unprivileged user `uvrgw` (group `dialout` for the Modbus RTU serial port) and grants `CAP_NET_BIND_SERVICE`, so the SunSpec meter emulation can listen on port 502.  systemd creates the state directory `/var/lib/uvrgw` (`StateDirectory=`) for the counter states.  The configuration file contains credentials, so make it readable for the service user only:

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin uvrgw
sudo chown root:uvrgw /etc/uvrgw.conf
sudo chmod 640 /etc/uvrgw.conf
sudo systemctl daemon-reload
sudo systemctl enable --now uvrgw
```

---

## Raspberry Pi image

`br/` builds a complete SD card image with Buildroot (submodule `br/buildroot`, LTS) for a Raspberry Pi Zero W with the [Waveshare RS485 CAN HAT](https://www.waveshare.com/wiki/RS485_CAN_HAT): systemd, WiFi, read-only squashfs root, no graphics.  The uvrgw package (`br/external`) is built from the working tree.

```bash
git submodule update --init
make -C br                  # -> br/output/images/sdcard.img
make -C br uvrgw-rebuild all  # after changing uvrgw sources
```

Any other Buildroot target can be given the same way (`make -C br menuconfig`, `linux-menuconfig`, ...); `O=<dir>` puts the build elsewhere.

**Hardware** (`br/external/board/rpi0w/config.txt`):
- CAN: MCP2515 on SPI0, interrupt GPIO25, `can0` at 50 kbit/s (`can0.network`).  The `oscillator` of the `mcp2515-can0` overlay must match the crystal on the HAT (12 MHz; 8 MHz on boards before 08/2019).
- RS485: `/dev/ttyAMA0` (PL011 on GPIO14/15, Bluetooth disabled).  Direction (RSE) on the UART0 RTS signal (GPIO17, overlay `uart-rts-overlay.dts`), switched by uvrgw (`mode = rs485`, `rts`).
- Console: USB serial gadget on the USB OTG port (`ttyGS0`, kernel messages and login as `root` without password); power the board through the PWR port.  SSH (dropbear) with key login for `root`.  U-Boot has no console (its UART is the RS485 bus).
- Hardware watchdog: started by U-Boot, kept by the kernel until systemd takes over (`RuntimeWatchdogSec=14`).

**SD card layout** (`genimage.cfg`):

| Partition | Content |
|---|---|
| 0–4 MiB | MBR, U-Boot environment (two copies at 1 MiB and 2 MiB) |
| p1 `boot` (vfat) | RPi firmware, `config.txt`, DT and overlays, U-Boot, `boot.scr`; not written in operation |
| p2 / p3 | root slots A / B (squashfs, kernel in `/boot`); the image fills both |
| p4 `data` (ext4) | `/data`: configuration, credentials, state |

**Slot selection** (`boot.cmd`, the RAUC U-Boot scheme): `BOOT_ORDER` (default `A B`) lists the slots, `BOOT_A_LEFT` / `BOOT_B_LEFT` (default 3) the remaining attempts.  Each boot uses one attempt of the first slot with attempts left and passes `root=` and `rauc.slot=` to the kernel; after three failed boots (panic, watchdog reset, missing kernel) U-Boot boots the other slot.  Once uvrgw is started, `uvrgw-mark-good.service` marks the booted slot good (`rauc status mark-good`, resets its attempts).  The environment is written power-safe (redundant copies); `fw_printenv` / `fw_setenv` access it from Linux.

**Updates** (RAUC, `br/external/board/rpi0w/rootfs-overlay/etc/rauc/system.conf`): the build also creates the signed bundle `br/output/images/uvrgw-rpi0w.raucb` (root filesystem, version from `git describe`).  RAUC writes it to the inactive slot and makes that slot the first in `BOOT_ORDER`; if it does not come up three times, U-Boot boots the old slot again.

```bash
scp br/output/images/uvrgw-rpi0w.raucb root@uvrgw:/tmp/
ssh root@uvrgw 'rauc install /tmp/uvrgw-rpi0w.raucb && reboot'
ssh root@uvrgw rauc status            # booted slot, versions, boot status
```

The signing key and certificate are in `br/keys/` (not in git; the build fails without them).  The certificate is the keyring in the image, so bundles are only accepted from the same key:

```bash
mkdir -p br/keys
openssl req -x509 -newkey rsa:4096 -nodes -days 36500 \
  -keyout br/keys/key.pem -out br/keys/cert.pem -subj "/O=uvrgw/CN=uvrgw update signing"
```

Only the root slots are updated; p1 (firmware, U-Boot, `config.txt`, DT overlays, `boot.scr`) and `/data` stay as they are.

**Data partition:** configuration, credentials and state live on `/data`, so the image contains no site-specific data.  After flashing, mount p4 and add:

| File | Content |
|---|---|
| `uvrgw.conf` | uvrgw configuration (set to `root:uvrgw 0640` at boot) |
| `wpa_supplicant-wlan0.conf` | WiFi (`ctrl_interface=/run/wpa_supplicant`, `country=DE`, `network={...}`) |
| `ssh/authorized_keys` | SSH public keys for `root` |

`/data/uvrgw` (counter states) and `/data/dropbear` (SSH host keys) are created at boot.  uvrgw starts after the first NTP sync (`time-sync.target`, `systemd-time-wait-sync`), as the Pi has no RTC.

---

## Usage

```
uvrgw [config-file]
```

If no config file is given the default path `/etc/uvrgw.conf` is used.

---

## Configuration

The configuration file uses [libconfuse](https://github.com/libconfuse/libconfuse) syntax.  Top-level sections are `mqtt {}`, `json {}`, `can {}`, `modbus_rtu {}`, `modbus_tcp {}`, `sunspec_server {}`, `counter` and `eval`.

Values across different protocol sections are linked by **name**: giving two values the same name in any combination of sections causes the value dispatcher to forward every received value to all registered outputs with that name.  Each name may be published by only one input, counter or eval; a second one is a configuration error naming both (e.g. `value 'x' of eval 'a' is already produced by modbus_tcp '192.168.1.10'`).  A name that no input, counter or eval produces is a configuration error as well (`value 'x' is not produced by any module.`), which catches misspelled names.

### Stalled data (`init_value`, `stale_timeout`)

Every value has a number and a *valid* flag.  A value is invalid before its first data and after a timeout, valid otherwise.  Outputs (MQTT, CAN, Modbus) and expressions always use the number; the MQTT logger writes `null` for an invalid value, and a SunSpec meter with an invalid source answers with exception 4.

Input values (`mqtt` with `dir = in`, `json`, `can` frames with `dir = in`, Modbus blocks with `dir = in`) have two options:

| Option | Default | Description |
|--------|---------|-------------|
| `init_value` | 0 | Value before the first data and after a timeout. |
| `stale_timeout` | 0 | Max. time (ms) without data before the value is reset to `init_value` and marked invalid; 0 = never. |

Both may be given per value, or once in the device section (`mqtt`, `json`, `can`, Modbus `slave`), where they apply to all input values of that section; a value setting wins over the section setting.  When a timeout expires, the value is reset to `init_value`, marked invalid and dispatched once, so outputs receive `init_value` (e.g. 0 W, a pump enable switched off).  Real data takes over again as soon as it arrives.  Timeouts and recoveries are logged once (`value 'x' timed out, reset to 0.`, `value 'x' never received, set to 0.`, `value 'x' received again.`).  Invalid readings (unparsable MQTT payloads, `valid_if` failing, SunSpec "not implemented" values, non-finite floats) are dropped and count as no data.

Without `stale_timeout` a value keeps its last data forever and stays valid, so the logger writes the last value and a SunSpec meter serves it.  A `stale_timeout` is **required** where a frozen value would be used as current power (configuration error otherwise, if the source is an input):

- the `source` of a counter with `integrate_power` (the last power would be integrated forever),
- the power sources of a SunSpec meter (`power_l1` … `power_l3`, `power`); the meter then becomes unavailable when its power source dies.

Everywhere else it is optional: set it where falling back to `init_value` is the wanted reaction (flags, setpoints, control inputs read by expressions or written to an output), or where a dead sensor should be logged as `null`.  Do not set it on device counter readings (energy registers): a dead meter then leaves the reading and the counter total frozen, like the register in the device.  A `stale_timeout` set in the device section also applies to them, so override it with `stale_timeout = 0`:

```
modbus_tcp {
  ip = "192.168.1.10"
  slave {
    id = 1
    interval = 1000
    stale_timeout = 10000             # all input values of this slave
    block {
      dir = in
      regtype = reg
      addr = 0
      count = 4
      value grid_power      { reg = 0  type = f32 }
      value grid_energy_imp { reg = 2  type = f32  stale_timeout = 0 }   # counter reading: none
    }
  }
}
```

**Removed options:** `stale_timeout` of the MQTT logger and of `sunspec_server`, `max_gap` of counters and `max_age` of evals are gone, and the `init` of eval values is now `init_value`.  A configuration still using them fails to load; configure `stale_timeout` on the inputs instead.

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

**MQTT connection:** if the broker is not reachable (also at startup), the connection is retried automatically; connection changes are logged.

**QoS:** `qos` of the section is the default of its values (and of the state topic).  With `qos` ≥ 1, every value published while the broker is not connected is kept in memory by libmosquitto and delivered after reconnect; the memory is not limited, so a long outage with many values can exhaust it.  Use `qos = 0` for values (the next update replaces a lost one) and leave QoS 1 to the loggers, which have their own `qos` (default 1).

#### Logger (value snapshots for databases)

A `logger` inside an `mqtt` section publishes a JSON snapshot of selected values every `interval` seconds, aligned to the clock (e.g. xx:00, xx:05, ...):

```
mqtt {
  host = "10.0.0.2"

  logger energy {
    topic         = "uvrgw/log/energy"   # last topic level can be used as table name
    interval      = 300                  # s, must divide a day (86400 s); default 300
    qos           = 1                    # default 1

    value pv_carport_energy { }
    value hp_heat_energy { scale = 0.001 }                      # e.g. Wh -> kWh
    value firstfloor_office_env_temp { field = "office_temp" }  # JSON field / column name
  }
}
```

```json
{"time":1790762700,"pv_carport_energy":16852887.05,"hp_heat_energy":1234.567,"office_temp":21.0}
```

- `time` is the snapshot time (Unix seconds, aligned).  Values are the current (instantaneous) values; numbers always contain a decimal point, so consumers infer a float type.
- Invalid values (no data yet, or reset after the `stale_timeout` of their input) are `null`, see [Stalled data](#stalled-data-init_value-stale_timeout).  A value without `stale_timeout` is logged with its last value, also after its source died.
- Snapshots are only taken while the system clock is synchronised (kernel time status, maintained by ntpd, chrony or systemd-timesyncd).
- Broker outages: with `qos` ≥ 1, snapshots published while the broker is not connected are kept in memory by libmosquitto and delivered after reconnect (no gaps, no duplicates; lost if uvrgw is restarted meanwhile).  With `qos = 0` they are lost.
- Field names may only contain `A-Z a-z 0-9 _` (not `time`) and must be unique within a logger.

**Writing snapshots to PostgreSQL with Telegraf** (on the broker node; tested with Telegraf 1.40.1 and PostgreSQL 15):

```toml
[agent]
  omit_hostname = true                 # no "host" tag column

[[inputs.mqtt_consumer]]
  servers = ["tcp://127.0.0.1:1883"]
  topics = ["uvrgw/log/#"]
  qos = 1
  persistent_session = true            # broker keeps snapshots while Telegraf is down
  client_id = "telegraf-uvrgw"
  topic_tag = ""                       # no "topic" tag column
  data_format = "json_v2"

  [[inputs.mqtt_consumer.topic_parsing]]
    topic = "uvrgw/log/+"
    measurement = "_/_/measurement"    # table name = last topic level

  [[inputs.mqtt_consumer.json_v2]]
    [[inputs.mqtt_consumer.json_v2.object]]
      path = "@this"
      timestamp_key = "time"
      timestamp_format = "unix"

[[outputs.postgresql]]
  connection = "host=localhost user=telegraf password=secret dbname=ems sslmode=disable"
  timestamp_column_type = "timestamp with time zone"   # timestamptz (default: without time zone, UTC)
  # one wide table per logger (measurement); columns are added automatically
```

Without `timestamp_column_type`, Telegraf creates the `time` column as `timestamp without time zone` holding UTC, which is easily misread as local time.  Existing tables can be converted with `ALTER TABLE <table> ALTER COLUMN time TYPE timestamptz USING time AT TIME ZONE 'UTC';`.

**MQTT input payloads** are validated: `number` accepts only a finite number (surrounding whitespace allowed), `switch` only `ON`/`OFF` and `contact` only `CLOSED`/`OPEN` (case insensitive).  Other payloads (e.g. `unknown`, `unavailable`, `nan`) are ignored and logged once per value; they count as no data, so `stale_timeout` decides what happens instead of a wrong value.  Each value name may only be used once per `mqtt` section.

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
  timestamp_period = 60000   # send timestamp every 60 s (0 = disabled), only while the clock is synchronised
  send_timeout     = 1000    # coalesce output writes within 1 000 ms

  # Receive frame (CAN → value dispatcher)
  frame {
    can_id = "ANA:1:0"   # UVR analogue node 1, frame starting at channel 0  (or 0x201, or decimal)
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
| `ANA:node:chan` | UVR analogue frame; `chan` is the first channel of the frame (0, 4, … 28), a frame has 4 channels | `ANA:1:0` → `0x201`, `ANA:1:4` → `0x281` |
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
- `sunspec_na = true` (input register blocks) drops SunSpec "not implemented" values: `s16` 0x8000, `u16`/`bitmask` 0xffff, `s32` 0x80000000, `u32` 0xffffffff, and values whose scale factor register is 0x8000.  Note: SunSpec `acc32` counters use 0 as marker, which is not dropped (use a `counter` with `max_power` for such values).
- `expect_reg` / `expect_value` (input register blocks) check a register of the block (index like `reg`) for an expected value, e.g. a SunSpec model ID.  On mismatch the whole block is ignored and a message is logged once.  This protects against shifted register maps, e.g. Fronius inverters shift the SunSpec models by 10 registers when switching between float and int+SF mode.
- A float NaN (SunSpec "not implemented") is treated as invalid reading and not forwarded.
- `bit` values are only allowed in `inbit`/`bit` blocks, all other types only in `inreg`/`reg` blocks.  Output blocks must use `bit` or `reg`.
- If a block read fails with a Modbus exception, polling continues with the next block; if the slave does not respond at all, the remaining blocks are skipped until the next poll interval.
- `interval` is required for every slave (also for slaves with output blocks only) and must be positive.
- A failed write is kept and retried after the `interval` of the slave, until it succeeds or a newer value arrives.
- Modbus TCP connections and Modbus RTU serial ports are opened on demand.  If the server is unreachable or the serial device missing (also at startup), opening is retried every second.  A TCP connection is closed and re-established after a timeout or I/O error, a serial port after an I/O error (e.g. USB adapter unplugged).
- Bitmask outputs are written from a local register image that starts at 0 and is not read back from the device.  Writing one bit therefore also writes all other bits of that register — map every relevant bit of such a register as an output.

### Persistent counters

```
state_dir = "/var/lib/uvrgw"   # optional; default $STATE_DIRECTORY (systemd) or /var/lib/uvrgw

# device counter (e.g. Shelly energy counter, reset on reboot)
counter hp_energy_imp1 {
  source    = "hp_energy_imp1_raw"   # device counter reading (Wh)
  max_power = 11000                  # W; plausibility limit (0 = off, default)
}

# power integration (e.g. heating rod)
counter heating_rod_energy {
  source          = "heating_rod_power"  # W; the input needs a stale_timeout
  integrate_power = true
}

# heat pump: separate heating and cooling counters from one signed kW value
counter hp_heat_energy {
  source          = "power_hp"           # kW, > 0 heating, < 0 cooling
  integrate_power = true
  scale           = 1000                 # kW -> W
  sign            = positive             # count positive power (default)
}
counter hp_cool_energy {
  source          = "power_hp"
  integrate_power = true
  scale           = 1000
  sign            = negative             # count negative power as positive energy
}
```

`scale` (default 1.0, must be positive) multiplies the source value first, for both counter types (e.g. 1000 for kW or kWh sources).

A counter listens to its `source` value and publishes the accumulated value under its own name (e.g. for MQTT, SunSpec meters or logging).  The total is valid once the counter has one (state file loaded or first reading taken) and stays valid when the source dies: log and serve the counter total, not the raw reading.  An invalid source value (reset after a timeout) is not data for the counter.

**Device counter:** on the first reading ever, the counter starts at the device value.  Then it is increased by the difference to the last reading.  A reading lower than the last one is taken as device counter reset and the reading itself is added.  With `max_power`, an increase larger than `max_power` × time since the last change is treated as glitch (e.g. a device reporting 0 for a moment): nothing is added and the last reading is resynchronised (the real increase during the glitch is lost).  Negative readings and invalid source values are ignored, so a timeout on the source is not taken as a reset.  After a uvrgw restart, the increase since the last saved reading is added without plausibility check.

**Power integration:** the power is integrated to Wh.  Each power value is held until the next one arrives; integration stops when the source becomes invalid (after its `stale_timeout`) and resumes with the next power value, and the time uvrgw was not running is not counted.  A power source that is an input therefore needs a `stale_timeout` (configuration error otherwise); a power computed by an eval is held until the eval runs again.  With `sign = positive` (default) only positive power is counted, with `sign = negative` only negative power (as positive energy); the other part is counted as 0.  The counter is advanced and published every second, so power values that are only sent on change (e.g. CAN) are handled correctly.  A switching load that only provides an on/off signal can be integrated by scaling the signal to its rated power in the source (e.g. `scale = 3000`).

**State files:** each counter stores `<accumulated> <last reading>` in `<state_dir>/counters/<name>` (written atomically every 5 minutes if changed, and on shutdown).  A missing file starts a new counter.  An unreadable or invalid file disables the counter (logged, value stays invalid) instead of silently starting from 0.  To set a counter (e.g. take over a value from another system), stop uvrgw, write the file and start uvrgw again.  Counter names may only contain `A-Z a-z 0-9 _ . -`; the source of a counter must not be another counter.

**Gating sources:** a `json` source can be ignored as a whole while the device data is not valid, e.g. a Shelly with unsynchronised clock (whose counters are not valid then):

```
json {
  url      = "http://shelly/status"
  valid_if = "unixtime"   # poll is ignored if this path is missing, 0, false or ""
  ...
}
```

### Calculated values and control logic (eval)

```
eval solar {
  period  = 1000                # evaluate every 1000 ms
  value diff       { expr = "collector_temp - tank_temp"  local = true }
  value solar_pump { expr = "hyst(solar_pump, diff, 8, 4) && tank_temp < 90" }
  value pump_on_s  { expr = "if(solar_pump, pump_on_s + dt, 0)" }
}

# heat pump total energy: evaluated once per poll, after the last phase arrived
eval hp_energy {
  triggers = {"hp_energy_imp3"}
  value hp_energy_imp { expr = "hp_energy_imp1 + hp_energy_imp2 + hp_energy_imp3" }
}
```

| Option | Default | Description |
|--------|---------|-------------|
| `period` | — | Evaluate every `period` ms. |
| `triggers` | — | Evaluate when one of these values is updated. |
| `value <name>` | | One or more values, evaluated in config order. |
| `expr` | (required) | Expression of the value. |
| `local` | false | Value is private to the eval (not published); the name can be reused in other evals. |
| `init_value` | 0 | Value before the first evaluation (for values that read themselves or are read by a value further up). |

An eval publishes its non-local values under their names, so they can be used like any input (MQTT, CAN, Modbus outputs, counters, SunSpec, logger, other evals).  Every evaluation publishes all values, also unchanged ones.

**Evaluation modes** (exactly one):

- `period`: every `period` ms on a fixed schedule; missed evaluations are skipped, not caught up.
- `triggers`: when one of the listed values is updated.
- neither: when one of the outside values read by the expressions is updated (default triggers).  With several of them arriving together (e.g. all values of one Modbus block or JSON poll) this may evaluate up to once per value; use `triggers` with the last one to evaluate once.

`period` and `triggers` together are an error.  Triggers arriving while an evaluation is pending cause one evaluation.  All evals are evaluated once at startup.  Evals that trigger each other in a cycle (an eval publishes a trigger of the next one, and the last one a trigger of the first) are a configuration error, because they would evaluate forever; periodic evals may read each other's values (they see the result of the other's last evaluation).  Cycles through other modules (e.g. eval → counter → eval) are not detected.

**Names in expressions** are resolved in this order:

1. `dt`: seconds since the previous update of the value being evaluated; 0 for its first evaluation and after a non-finite result.
2. A value of the same eval: a value defined further up has the result of this evaluation; the value itself and values further down have the result of the previous one (or `init_value`).
3. An outside value: any other value name used in the configuration (inputs, counters, values of other evals, ...).
4. An expression function (see below).

Value names of an eval must start with a letter, followed by letters, digits and `_`, and must not be `dt` or the name of an expression function.  Outside values whose name does not follow this rule (e.g. contains `.` or `-`), is `dt` or equals the name of a function cannot be used in expressions.  Unknown names and syntax errors are reported at config load with their position.  A local value must not have the name of a value used outside of the eval (it would hide it).  An eval without `period` and `triggers` must read at least one outside value.

**Validity.**  Every value is evaluated and published in every evaluation, with the current numbers of the outside values: before their first data and after a timeout, that is the `init_value` of the input.  An eval feeding an output therefore needs sensible `init_value`s on its inputs.

- A value is valid if all outside values it reads are valid and its result is finite.  This includes the outside values read through other values of the eval (further up or further down, also indirectly), so a value reading the previous state of a value further down is invalid together with it.
- A non-finite result (NaN, inf) keeps the previous number of the value, which is published as invalid; this is logged once, and the recovery as well.
- Invalid values are published with their number: outputs and other evals use it, the logger writes `null`, SunSpec meters answer with exception 4 and counters ignore it (no data).
- Validity changes only when the eval runs.  With explicit `triggers` that do not include a value the expressions read, it follows that value's timeout at the next evaluation.
- A value reading a value further down gets its result of the previous evaluation, i.e. one evaluation late: in the first evaluation it gets `init_value`, and after a non-finite result of that value its last finite one.

**Expressions** ([tinyexpr](https://github.com/codeplea/tinyexpr)).  All values are doubles; 0 is false, everything else is true.  Comparisons and logic operators return 1 or 0.

| Precedence (highest first) | Operators |
|---|---|
| unary | `-` `+` `!` |
| power | `^` (right associative: `2^3^2` = 512, `-2^2` = -4) |
| multiplicative | `*` `/` `%` |
| additive | `+` `-` |
| relational | `<` `<=` `>` `>=` |
| equality | `==` `!=` |
| logical and | `&&` |
| logical or | `\|\|` |

Note: `!` binds tighter than `+` (`!0+1` = 2).  `&&` and `||` evaluate both sides.

| Function | Result |
|---|---|
| `if(c, a, b)` | `a` if `c` is true, else `b` (both are evaluated) |
| `min(a, b)`, `max(a, b)` | smaller / larger value |
| `clamp(x, lo, hi)` | `x` limited to `[lo, hi]` |
| `hyst(prev, x, on, off)` | 1 if `x >= on`, 0 if `x <= off`, else `prev` (as 1/0); with `on < off` inverted: 1 if `x <= on`, 0 if `x >= off` |
| `abs`, `floor`, `ceil`, `sqrt`, `pow(x, y)`, `exp`, `ln`, `log10` | as in C (`ln` is the natural logarithm) |
| `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y, x)`, `sinh`, `cosh`, `tanh` | trigonometric functions (radians) |
| `pi`, `e` | constants |

Only the final result of a value is checked: division by zero gives inf (the value is invalid), but `if(x != 0, a / x, 0)` is a working guard, although both branches are evaluated.

**State** is explicit: a value that needs memory reads itself, e.g. `hyst(solar_pump, ...)` or the run time counter `if(solar_pump, pump_on_s + dt, 0)`.  Time dependent expressions use `dt` instead of assuming a period, so they work in all modes and do not jump after a gap.  State is kept in memory only: after a restart values start at `init_value`.  If uvrgw stops, outputs keep their last state, so safety limits (e.g. tank over-temperature) belong in the controller.

### SunSpec meter emulation

```
sunspec_server {
  bind          = "0.0.0.0"
  port          = 502      # needs CAP_NET_BIND_SERVICE (or root) for ports < 1024

  meter heatpump {
    unit_id      = 84
    manufacturer = "modusoft"      # max. 32 chars
    model        = "SmartMeterGW"  # max. 32 chars
    serial       = "00000001"      # max. 32 chars
    # options, version: max. 16 chars (optional)

    # per phase sources (value names), all optional
    current_l1       = "hp_i1"     # A
    voltage_l1       = "hp_u1"     # V (phase to neutral)
    power_l1         = "hp_p1"     # W; the input needs a stale_timeout
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
- If a configured source value is invalid (no data yet, or reset after the `stale_timeout` of its input), requests for that meter are answered with exception 4 (server device failure), so the client keeps its last data instead of seeing wrong values (e.g. energy counters dropping to 0).  State changes are logged.  The power sources (`power_l1` … `power_l3`, `power`) need a `stale_timeout` if they are inputs (configuration error otherwise), so a dead meter makes the meter unavailable instead of serving its last power; other sources may keep their last value.
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
         │  counter thread                              │
         │  eval thread                                 │
         │  watchdog thread (stale_timeout)             │
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
- **Value dispatch** (`uvrgw_conf.c`): a linked list of named dispatchers, each holding an array of `(val, callback)` pairs registered during startup.  When a value arrives the dispatcher stores its number and valid flag (readable via `uvrgw_conf_get_val()`) and calls every callback whose `val` pointer differs from the source, preventing loopback; a mutex per name keeps storing and calling the callbacks atomic.  Each name starts at its producer's `init_value`, invalid.  Names, producers and the required timeouts are checked after the configuration is loaded.
- **Watchdog thread** (`uvrgw_conf.c`): runs only if a value has a `stale_timeout`; checks the deadlines every second and dispatches the reset to `init_value` (invalid) once per timeout.  Started after the outputs and stopped together with the other value sources.
- **CAN TX thread** (`can.c`): wakes every 20 ms, checks for pending outbound frames and the timestamp timer.
- **Modbus thread** (`mb.c`): wakes every 10 ms, polls slaves round-robin and writes queued output values.
- **REST thread** (`rest.c`): wakes every 100 ms, checks each endpoint's poll interval and performs HTTP GET.
- **Counter thread** (`counter.c`): advances power integration counters every second and saves changed counter states every 5 minutes.  Counters are started before all other modules, so states are loaded before the first value arrives.  On shutdown the thread is stopped together with the other value sources (see below); the states are saved last, after the last value.
- **Eval thread** (`eval.c`): evaluates all evals.  Trigger callbacks (in the thread of the source) only mark an eval as pending, so expressions are never evaluated in a source thread.  Started after the outputs and stopped together with the other value sources.
- **SunSpec server thread** (`sunspec.c`): waits on the listening socket and client connections (`select()` with 100 ms timeout) and answers requests from a register image rebuilt from the dispatcher's last values.
- **MQTT** (`mqtt.c`): libmosquitto manages its own background thread for connection, keep-alive and message delivery.
- **MQTT logger thread** (`mqtt_logger.c`): takes and publishes the logger snapshots at the aligned times.

---

## Signal handling

| Signal | Effect |
|--------|--------|
| `SIGINT` | Clean shutdown (drains all threads, closes sockets) |
| `SIGTERM` | Clean shutdown (same as SIGINT) |
| `SIGHUP` | Currently a no-op (reserved for future config reload) |

Shutdown is triggered by writing to a Linux `eventfd`, which is monitored by the main `select()` loop.  Output modules (MQTT, CAN) are started before the value sources, and on shutdown all threads producing values are stopped before the outputs are destroyed, so no value is sent to a closed connection.

---

## License

No license is currently specified in this repository.

The bundled [tinyexpr](https://github.com/codeplea/tinyexpr) (`tinyexpr/`, used by `eval`, slightly modified as described in `tinyexpr/README.uvrgw`) is licensed under the zlib license, see `tinyexpr/LICENSE`.
