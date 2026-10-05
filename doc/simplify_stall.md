# Implementation guide: simplify stalled data handling

Status: proposal, not implemented.

## 1. Goal

Stalled data (a source that stopped delivering) is handled in one place:
at the inputs.  Each consumer's own staleness handling is removed.  No
value is treated specially by the code; whatever a value needs is
defined in the configuration.

Every dispatched name has two independent properties:

- its **value**, always a finite number, and
- a **valid flag**, saying whether that number is current data.

Rules:

- Every input value has an `init_value` (default 0).  It is the value
  before the first data arrives and after a timeout.
- An input value may have a `stale_timeout` (ms, default 0 = off).  If no
  data arrives within that time, the value is **reset to `init_value`**,
  marked invalid and dispatched once.  Real data takes over again as soon
  as it arrives.  Without `stale_timeout` the last value is kept forever
  and stays valid.
- A name is invalid before its first data and after a timeout, valid
  otherwise.  Invalid never means "no number": the number is always
  there, and consumers that cannot say "unknown" (outputs, expressions)
  simply use it.
- Consumers that can say "unknown" use the flag: the MQTT logger writes
  `null`, a SunSpec meter answers with Modbus exception 4.
- NaN never enters the system.  Invalid readings (unparsable MQTT
  payloads, `valid_if` failing, Modbus "not implemented" floats) are
  dropped at the input as today; they count as no data, and
  `stale_timeout` decides what happens.

Typical configuration:

- Power, flags, setpoints, control inputs: `stale_timeout` with the
  default `init_value = 0`, so a dead source falls back to 0 (power stops
  being integrated, a pump enable switches off) and is logged as `null`.
- Temperatures and other measurements: `stale_timeout` as well; the
  logger writes `null`, the reset value only matters for outputs and
  expressions.
- Device counter readings: no `stale_timeout`.  A dead meter keeps its
  last reading, exactly as the hardware register does.

## 2. Current state (what is removed)

| Module | Today | After |
|---|---|---|
| dispatcher (`uvrgw_conf.c`) | stores value + update time + `updated` flag, drops NaN | stores value + valid flag, starts at the producer's `init_value`, owns the timeout watchdog |
| mqtt_logger | `stale_timeout` (s, default 600), "never received" warning | invalid → `null` |
| sunspec | `stale_timeout` (ms, default 30000), stale source → exception 4 | invalid source → exception 4 |
| counter | `max_gap` (ms, default 60000), publish only while power is fresh | publishes every tick; integration follows the source value |
| eval | `max_age`, per-value skipping, per-input stale/never received logging | always computes; validity is the AND of the values read |
| outputs (Modbus, CAN, MQTT) | no staleness handling | unchanged: they always get the number |

The config options `stale_timeout` (logger, SunSpec), `max_gap` and
`max_age` are removed without a compatibility fallback: a config still
using them fails to load (unknown option), and the README says so.  The
name `stale_timeout` returns with the same meaning (maximum age of data)
at the place that produces the data.

## 3. Configuration

New options for input values (`mqtt` with `dir = in`, `json`, `can`
frames with `dir = in`, Modbus blocks with `dir = in`):

| Option | Default | Description |
|---|---|---|
| `init_value` | 0 | Value before the first data and after a timeout. |
| `stale_timeout` | 0 | Max. time (ms) without data before the value is reset to `init_value` and marked invalid; 0 = never. |

Both may be given per value, or once per device section (`mqtt`, `json`,
`can`, Modbus `slave`), where they apply to all input values of that
section.  A value setting wins over the section setting.  This follows
the existing `qos`/`retain` pattern of `mqtt`: `CFGF_NODEFAULT` on the
value option, `cfg_size()` to detect that it was given.

```
modbus_tcp 192.168.1.10 {
  timeout = 250                       # Modbus response timeout, unrelated
  slave {
    id = 1
    interval = 1000
    stale_timeout = 10000             # all input values of this slave
    block {
      dir = in
      value grid_power      { reg = 0  type = f32 }
      value grid_energy_imp { reg = 2  type = f32  stale_timeout = 0 }
    }
  }
}
```

Config errors, checked after all sections are configured (the producer of
every name and its `stale_timeout` are known then; eval is configured
last):

- `stale_timeout < 0`.
- A value name that no module publishes.  This replaces the runtime
  "never received" warnings of the logger and eval, which mostly caught
  typos.
- A name read by an MQTT logger or a SunSpec meter whose producer is an
  input without a `stale_timeout`: it would be logged or served frozen
  forever.  Eval and counter producers are not checked (see section 5).
- The source of a device counter (counter without `integrate_power`) has
  a `stale_timeout`: the reset to `init_value` would read as a counter
  reset and the next real reading would be counted again.
- The power source of a counter with `integrate_power` has no
  `stale_timeout`, if its producer is an input: the last power would be
  integrated forever.  A power produced by an eval is not checked; its
  own inputs are.

## 4. Design

### 4.1 Dispatcher (`uvrgw_conf.c/h`)

Per name, replacing `last_update` and `updated`:

- `last_value`, starting at the producer's `init_value`,
- `valid`, starting false,
- `last_data`, monotonic time (ms) of the last real data (for the
  watchdog only),
- `init_value`, `stale_timeout` and the producer's `val` pointer.

```c
int uvrgw_conf_set_producer(UVRGW_CONF_VAL_DISPATCH_T *dp, const char *module,
                            const char *instance, void *val,
                            double init_value, int stale_timeout);
void uvrgw_conf_disp_val(UVRGW_CONF_VAL_DISPATCH_T *dp, void *val, double f, bool valid);
bool uvrgw_conf_get_val(UVRGW_CONF_VAL_DISPATCH_T *dp, double *f);
```

- `uvrgw_conf_set_producer()` additionally takes the producer's `val`
  pointer (source identity for the watchdog's dispatch), the start value
  and the timeout.  Producers: inputs pass their `init_value` and
  `stale_timeout`, eval values their `init_value` and 0, counters 0 and
  0.
- `uvrgw_conf_disp_val()` stores value and validity under `last_lock`,
  sets `last_data` when `valid` is true, then fires the callbacks as
  today.  Inputs pass `true`, the watchdog `false`, eval the validity it
  computed, the counter its own validity.
- `uvrgw_conf_get_val()` always returns the number; the return value says
  whether it is valid.  The `ts` parameter is gone.
- The existing guard against non-finite values in `uvrgw_conf_disp_val()`
  stays, so the invariant "a stored value is always a finite number"
  holds even if a module misses a check.
- Start values are not dispatched; outputs only receive real data, a
  computed value or a reset by the watchdog.

### 4.2 Watchdog

One thread for all names with `stale_timeout > 0`.  It wakes once per
second (the timeouts are seconds to minutes; no deadline scheduling).
For each such name, under `last_lock`: if `valid` and
`now - last_data >= stale_timeout`, set `last_value = init_value` and
`valid = false`.  The lock is released, then the reset is dispatched once
with the producer's `val` as source.  Only valid names can expire, so a
reset is never repeated.

Each reset is logged once ("value 'x' timed out, reset to <init_value>")
and the next real data once ("value 'x' received again").  This replaces
the per-consumer stale logging of logger, SunSpec and eval.

### 4.3 Locking

The watchdog is the first dispatcher of a name that is not the name's
producer, so a reset and real data can happen at the same moment.  The
lock is not held across the callbacks (the counter already dispatches its
total from two threads under its own lock; a lock held across callbacks
would deadlock against that).  The resulting lost update is accepted and
documented: a reset may overwrite data that arrived in the same instant,
or the other way round.  The next data corrects it, and no consumer is
damaged by one stray value — device counters are the only consumer that
could be, and their sources may not have a `stale_timeout` (section 3).

### 4.4 Inputs

`mqtt.c`, `rest.c`, `can.c`, `mb.c`: parse `init_value` and
`stale_timeout` (value, else device section), pass them to
`uvrgw_conf_set_producer()`, dispatch with `valid = true` and keep
dropping invalid readings (unparsable payloads, `valid_if`, `sunspec_na`,
non-finite floats) so they count as no data.

### 4.5 Outputs

Unchanged.  Modbus, CAN and MQTT outputs always write the number they are
given, whether it is valid or not: a timeout reset is how a dead source
reaches the controller as `init_value`.  A fallback value at an output is
configured as `init_value` on the input.

### 4.6 MQTT logger

`stale_timeout`, `received` and the "never received" warning are removed.
A value is logged as `null` if it is invalid or if it is not finite after
`scale`, and as a number otherwise.  A device that is off but alive
therefore logs its real 0, a dead device logs `null`.

### 4.7 SunSpec

`stale_timeout`, `src_get()`'s age check and `min_ts` are removed.
`src_get()` reports a configured source as missing if
`uvrgw_conf_get_val()` returns false, so the meter answers with exception
4.  The "unavailable"/"available" state logging stays, naming the first
invalid source.

### 4.8 Counter

- The counter's own value is its total.  It is valid once the counter has
  a total (state file loaded or first reading taken) and is dispatched
  with that validity.  A counter disabled by an unreadable state file
  never dispatches and stays invalid, so the logger writes `null` and a
  SunSpec meter answers with exception 4.
- Device counter: unchanged.  A source that stops delivering keeps its
  last reading (no `stale_timeout` allowed, section 3), so the total
  stays valid and frozen, like the register in the device.
- Power integration: `max_gap` and `power_fresh()` are removed.  Each
  power value is integrated until the next one arrives and the total is
  published every tick.  A dead source with `stale_timeout` falls back to
  0 W, which stops integration; without one the last power is integrated
  forever, as configured.

### 4.9 Eval

- Every value is evaluated and published on every evaluation; there is no
  skipping.  A value's validity is the AND of the validity of all outside
  values it reads, using the existing input closure (`v->ins`, which
  already covers values read through other values of the same eval).  The
  number is published either way.
- A non-finite result is not published, the previous value and validity
  are kept, and the problem is logged once, as today.
- `init` is renamed to `init_value`, matching the inputs (eval is not
  released yet, so no compatibility issue).
- Removed: `max_age`, per-value skipping (`v->updated`, `v->deps`), the
  per-input stale / never received / valid again logging, `in->logged`,
  `MISSING_REPORT_MS`, `start_ticks`.
- Unchanged: `dt`, `period`, `triggers`, evaluation order, `local`,
  `refs` and the load-time checks (cycles, unknown names, shadowing).
- Expressions keep working on plain numbers; tinyexpr is not changed and
  needs no NaN handling, no `isnan()` and no new built-ins.

## 5. Consequences

Behaviour changes worth stating in the README:

- A counter total whose source died is logged as its last value forever,
  where today it becomes `null` after 600 s.  For a cumulative register
  the frozen value is the honest answer.
- An eval publishes from the first evaluation on, computed from the
  `init_value`s of inputs that have not delivered yet.  Today nothing is
  published until all inputs exist.  An eval feeding an output therefore
  needs sensible `init_value`s on its inputs.
- Two gaps are not checked at load time, because catching them would need
  a walk through the producer graph: a logged or metered value produced by
  an eval whose own inputs have no `stale_timeout` can be published
  frozen, and an output can be written with a frozen value forever.  The
  README says that every input should have a `stale_timeout`.

## 6. Testing

- Input without `stale_timeout`: the value keeps the last data forever and
  stays valid; before the first data it is invalid, the logger writes
  `null`, SunSpec answers exception 4, expressions read `init_value` and
  nothing is sent to outputs.
- Input with `stale_timeout`: reset to `init_value` after the timeout,
  dispatched once, logged once, logger `null`, SunSpec exception 4, output
  written with `init_value`; real data takes over and is logged once; a
  never-received input is reset at `stale_timeout` after startup.
- Inheritance: section value used, per-value override wins, `0` in a value
  overriding a section timeout.
- Race: real data and a timeout at the same moment (short timeout, data at
  the interval boundary); the next data corrects the value.
- Loopback: a name that is an MQTT input on one connection and an output
  on another; the reset is not sent back to the input's topic.
- Power counter with a timeout on the source: integration stops after the
  timeout, the total keeps being published and stays valid.
- Eval: a value reading two inputs, one of them invalid, is published as a
  number and logged as `null`; it becomes valid again with the data.
- Config errors: name without producer, logged value produced by an input
  without `stale_timeout`, device counter source with `stale_timeout`,
  integrating counter power source without one, negative `stale_timeout`,
  removed options.

## 7. Plan

One commit each, every step building:

1. Dispatcher: `valid` flag, `init_value`/`stale_timeout`/producer `val`
   in `uvrgw_conf_set_producer()`, validity parameter on
   `uvrgw_conf_disp_val()`, `uvrgw_conf_get_val()` always returning the
   number.  All modules adapted mechanically; their own staleness checks
   stay for now.
2. Inputs: `init_value` and `stale_timeout` for MQTT, JSON, Modbus and
   CAN, with device section inheritance; inputs drop invalid readings
   themselves.
3. Watchdog thread and timeout logging.
4. Consumers: logger (`null` when invalid, `stale_timeout` removed),
   SunSpec (exception 4 when invalid, `stale_timeout` removed), counter
   (`max_gap` removed, own validity).
5. Eval: validity as the AND over the input closure; `max_age`, skipping
   and per-input logging removed; `init` renamed to `init_value`.
6. Dispatcher cleanup: `last_update`/`updated` remnants removed.
7. Config checks: name without producer, logged/metered value produced by
   an input without `stale_timeout`, counter source rules.
8. Docs: README (options, inheritance, removed options, consequences) and
   `doc/eval.md`.  Production config (`uvrgw.conf`, untracked):
   `stale_timeout` per device section, replacing the removed
   `stale_timeout` of the SunSpec servers and the loggers and the
   `max_gap` of the integrating counters.

## 8. Decisions

- Validity is a flag, not a value.  Value and validity are separate
  channels: outputs and expressions use the number, the logger and
  SunSpec use the flag.  Nothing has to be configured twice, and no
  default can make a consumer behave wrongly.
- `stale_timeout` and `init_value` exist on inputs only.  Counters and
  eval values have no timeout: a counter total ages like a hardware
  register, and an eval inherits the validity of what it reads.
- `init_value` defaults to 0, `stale_timeout` to 0 (never).  Both may be
  set per device section and overridden per value.
- A timeout resets the value *and* clears the flag, so one option serves
  all consumers: the controller sees 0 W, PostgreSQL sees `null`.
- No compatibility fallback for removed options; breaking existing
  configs is accepted.
- Invalid source data (unparsable MQTT payload, `valid_if` failing,
  non-finite readings) is dropped at the input and counts as no data.

Rejected alternatives (so they are not discussed again):

- **NaN as the invalid state** (no flag, `init_value = nan`).  It makes
  `init_value` carry two jobs at once — which number consumers see and
  whether the value counts as known — so no default is right: NaN by
  default poisons outputs and expressions that need a number, 0 by
  default resets device counters and logs dead sensors as 0.  It also
  needs NaN rules in every consumer, a patched tinyexpr, `isnan()` and a
  documented NaN truth table.  The flag costs nothing instead:
  `uvrgw_conf_get_val()` already returns it.
- **A "seen" flag that is never cleared** (invalid only before the first
  data).  Then a dead power source is logged as 0 and a dead sensor as its
  last temperature, never as `null` — the logging problem the redesign is
  supposed to remove.
- **Timeouts derived from other values** (an eval taking the longest
  timeout of the values it reads).  The chain runs input → counter → eval,
  so the derivation needs a walk through the producer graph plus trigger
  analysis — the dependency closure that this redesign removes.
- **Timeouts on counters and eval values.** Nobody expects a timeout
  anywhere but at an input, and validity already propagates: a reset is a
  dispatch, so it triggers the evals that read the value.
