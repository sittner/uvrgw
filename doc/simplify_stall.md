# Implementation guide: simplify stalled data handling

Status: proposal, not implemented.

## 1. Goal

Stalled data (a source that stopped delivering) is handled in one place:
at the inputs.  Each consumer's own staleness handling is removed.  No
value is treated specially by the code; whatever a value needs is
defined in the configuration.

Rules:

- Every input value has an `init_value` (default 0, `nan` allowed).  It
  is the value before the first data arrives.
- An input value may have a `timeout` (ms, default 0 = off).  If no data
  arrives within `timeout`, the value is **reset to `init_value`** and
  published once.  Real data takes over again as soon as it arrives.
  Without `timeout` the last value is kept forever.
- NaN is the only "no valid value" state.  It only exists where the
  configuration asks for it (`init_value = nan`).  Consumers handle NaN in
  one obvious way each (section 4); there is no other invalid state, no
  timestamps and no "never received" logic.

Typical configuration:

- Power, flags, setpoints, control inputs: `timeout` with the default
  `init_value = 0`, so a dead source falls back to 0 (e.g. power stops
  being integrated, a pump enable switches off).
- Energy counters and measurements (temperatures, ...) consumed by
  counters, the MQTT logger or SunSpec: `init_value = nan`, with or
  without `timeout`.  Device counters then skip the value, the logger
  writes `null`, SunSpec answers with exception 4.

## 2. Current state (what is removed)

| Module | Today | After |
|---|---|---|
| dispatcher (`uvrgw_conf.c`) | stores value + update time + `updated` flag, drops NaN | stores value only, starts at the producer's start value, forwards NaN, owns the timeout watchdog |
| mqtt_logger | `stale_timeout` (s, default 600), "never received" warning | NaN → `null` |
| sunspec | `stale_timeout` (ms, default 30000), stale source → exception 4 | NaN source → exception 4 |
| counter | `max_gap` (ms, default 60000), publish only while power is fresh | NaN is ignored |
| eval | `max_age`, dependency closure, per-value skipping, stale/never received logging | computes as tinyexpr does |
| outputs (Modbus, CAN, MQTT) | no staleness handling | NaN is not written |

The config options `stale_timeout` (logger, SunSpec), `max_gap` and
`max_age` are removed without a compatibility fallback: a config still
using them fails to load (unknown option), and the README says so.

Invalid source data stays ignored as today and is not turned into NaN:
invalid MQTT payloads, JSON data failing `valid_if`, NaN readings from
Modbus (e.g. "not implemented" float registers).  Invalid data counts as
no data; the input's `timeout` decides what happens.  Since the
dispatcher no longer drops NaN, each input drops NaN readings itself
before dispatching.

## 3. Configuration

New options in the `value` sections of all inputs (`mqtt` with
`dir = in`, `json`, `can` frames with `dir = in`, Modbus blocks with
`dir = in`), per value only (no defaults per frame, block or section):

| Option | Default | Description |
|---|---|---|
| `init_value` | 0 | Value before the first data and after `timeout`; `nan` = no valid value. |
| `timeout` | 0 | Max. time (ms) without data before the value is reset to `init_value`; 0 = never. |

```
modbus_tcp 192.168.1.10 {
  slave 1 {
    block {
      value grid_power      { ...  timeout = 10000 }        # dead meter -> 0 W
      value grid_energy_imp { ...  init_value = nan }       # never fake a counter
    }
  }
}
```

Config errors:

- `timeout < 0`.
- For polled inputs (`json`, Modbus): `timeout` not larger than the poll
  interval, since it would expire on every cycle.
- A value name that no module publishes (moved here from the runtime
  "never received" warnings of the logger and eval, which mostly caught
  typos).
- The source of a device counter (counter without `integrate_power`)
  has a `timeout` and a numeric `init_value`: the reset would read as a
  counter reset and the next real reading would be counted again
  (double counting).  `init_value = nan` with `timeout` is allowed, the
  counter ignores NaN.  Power integrating counters are not checked;
  falling back to 0 W just stops integration.

`nan` is parsed by libconfuse's `CFG_FLOAT` (strtod); verify, otherwise
accept it explicitly.

## 4. Design

### 4.1 Dispatcher (`uvrgw_conf.c/h`)

- `last_update` and `updated` are removed.  `last_value` starts at the
  start value given by the producer.  `uvrgw_conf_get_val()` returns the
  value (`double uvrgw_conf_get_val(dp)`).
- `uvrgw_conf_set_producer()` additionally takes the producer's `val`
  pointer (source identity for loopback suppression), the start value
  and the timeout.  Producers:
  - inputs: `init_value`, `timeout`
  - eval values: `init_value`, no timeout
  - counters: value loaded from the state file, NaN if none or invalid,
    no timeout
- Names without a producer keep NaN; this is the config error of
  section 3.
- `uvrgw_conf_disp_val()` no longer drops NaN.
- Start values are not dispatched; outputs only receive real data or a
  reset by the watchdog.

### 4.2 Watchdog

One thread for all names with a timeout.  It sleeps until the earliest
deadline (`deadline = last data + timeout`; at startup, startup time +
timeout).  For an expired name it sets the value to `init_value` and
dispatches it once with the producer's `val` as source.  The deadline
is then cleared until the next real data; a reset is not repeated.

Each reset is logged once ("value 'x' timed out, reset to <init_value>")
and the next real data once ("value 'x' received again").  This replaces
the per-consumer stale logging.

### 4.3 Locking

A real value and a reset may happen at the same moment.  If the watchdog
checks the deadline, releases the lock and then dispatches, its reset can
overwrite the newer real value, in the dispatcher and in the outputs.

Each dispatcher gets a dispatch mutex, held from storing the value until
all callbacks have returned.  Both `uvrgw_conf_disp_val()` and the
watchdog take it; the watchdog re-checks the deadline under it.
`last_lock` stays for readers (`uvrgw_conf_get_val()`), so slow callbacks
(MQTT publish) do not block the eval thread, the logger or SunSpec.

Lock order: dispatch mutex before module locks.  Callbacks must not
dispatch another name synchronously.  The counter holds its own lock
while dispatching its own name; this is fine since no callback of a
counter's name leads back into that counter.  Document this in the
header.

### 4.4 Inputs

`mqtt.c`, `rest.c`, `can.c`, `mb.c`: parse `init_value` and `timeout`,
pass them to `uvrgw_conf_set_producer()`, drop NaN readings before
dispatching.  The watchdog needs nothing else from them.

### 4.5 Outputs

Modbus, CAN and MQTT outputs do not write NaN; the destination keeps its
last value.  A fallback value at an output is configured on the input
(`init_value` as a number).

### 4.6 MQTT logger

`stale_timeout`, `received` and the "never received" warning are
removed.  A non-finite value (after `scale`) is logged as `null`.

### 4.7 SunSpec

`stale_timeout`, `src_get()`'s age check and `min_ts` are removed.  If a
configured source is NaN, the meter answers with exception 4.  The
"unavailable"/"available" state logging stays, naming the first NaN
source.

### 4.8 Counter

- Device counter: a NaN reading is ignored (no reading).  A source with
  `timeout` and a numeric `init_value` is a config error (section 3).
  The check runs after all sections are configured, when the producer of
  the source name and its timeout are known.
- Power integration: each power value is held until the next one; NaN
  stops integration until the next number.  `max_gap` and
  `power_fresh()` are removed; the counter publishes its total every
  tick.  A dead source with `timeout` falls back to 0 W (or NaN), which
  stops integration.  Without `timeout` the last power is integrated
  forever, as configured.
- The counter's own value is its total and is always valid once loaded
  or initialised.

### 4.9 Eval

- Expressions are computed as tinyexpr computes them; tinyexpr is not
  changed.  `if`, `min`, `max`, `clamp` and `hyst` keep returning NaN for
  NaN arguments.
- New built-ins: `isnan(x)` and the constant `nan`, so expressions can
  handle inputs configured with `init_value = nan`.
- The result is stored and published as computed; a non-finite result
  (inf) is stored as NaN, so a value is always a finite number or NaN.
- Removed: `max_age`, the dependency closure (input lists per value,
  `deps`), skipping values, `updated` per value, the stale / never
  received / valid again / not finite logging, `MISSING_REPORT_MS`,
  `start_ticks`.
- Eval's `init` is renamed to `init_value`, matching the inputs (eval is
  not released yet, so no compatibility issue).
- Unchanged: `dt`, `period`, `triggers`, evaluation order, `local`.

NaN behaviour of the tinyexpr operators, for the README:

| Expression with a NaN operand | Result |
|---|---|
| `+ - * / ^`, `min`, `max`, `clamp`, `if` (condition), `hyst` | NaN |
| `> >= < <= ==` | 0 |
| `!=` | 1 |
| `&&`, `\|\|` | NaN counts as true |
| `!x` | 0 |

So an expression reading a value with `init_value = nan` should check it
with `isnan()` where the result matters.

## 5. Testing

- Input without `timeout`: value keeps the last data forever; before the
  first data `init_value` is read by eval, logger and SunSpec, and
  nothing is sent to outputs.
- Input with `timeout`: reset to `init_value` after `timeout`, dispatched
  once, logged once; real data takes over; never-received input is reset
  after `timeout` from startup.
- `init_value = nan`: logger `null`, SunSpec exception 4, device counter
  ignores it, outputs do not write it, eval `isnan()` works.
- Race: real data and timeout at the same moment (short timeout, data at
  the interval boundary); the dispatcher and outputs end with real data.
- Loopback: a name that is an MQTT input on one connection and an output
  on another; the reset is not sent back to the input's topic.
- Power counter with timeout on the source: integration stops after the
  timeout.
- Config errors: `timeout` <= poll interval, name without producer,
  device counter source with `timeout` and numeric `init_value` (but not
  with `init_value = nan`, and not for power integrating counters),
  removed options.

## 6. Plan

One commit each:

1. Dispatcher: start values from producers, NaN forwarded, `updated` and
   timestamps removed, timeout watchdog, dispatch lock.
2. Inputs: `init_value` and `timeout` for MQTT, JSON, Modbus and CAN;
   inputs drop NaN themselves.
3. Consumers: outputs skip NaN; logger and SunSpec handle NaN,
   `stale_timeout` removed; counter handles NaN, `max_gap` removed.
4. Eval: `isnan()` and `nan`, non-finite results stored as NaN, `init`
   renamed to `init_value`; removal of `max_age`, closure, skipping and
   logging.
5. Config checks: names without producer, device counter source with
   `timeout` and numeric `init_value`.
6. Docs: README (options, removed options, NaN table, config advice) and
   `doc/eval.md`.

## 7. Decisions

- Eval's `init` is renamed to `init_value`.
- `timeout` and `init_value` are set per value only; there are no
  defaults per CAN frame, Modbus block or JSON section.
- No compatibility fallback for removed options; breaking existing
  configs is accepted.
- Invalid source data (invalid MQTT payload, `valid_if` failing, NaN
  readings) is ignored, not turned into NaN.
- A device counter source with `timeout` and numeric `init_value` is a
  config error; power integrating counters are not checked.
