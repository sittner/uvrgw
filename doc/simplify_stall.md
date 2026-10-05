# Implementation guide: simplify stalled data handling

Status: steps 1–5 of section 7 implemented; deviations from this guide
are listed in section 9.

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
- Device counter readings: no `stale_timeout`.  A dead meter then leaves
  the reading and the total valid and frozen, exactly as the hardware
  register does.  A timeout would not disturb the counter (it ignores
  invalid dispatches, section 4.8), but the raw name would read
  `init_value` for everything else that reads it, which is wrong for an
  energy register sent to an output.  Log and serve the counter total,
  not the raw reading.  A `stale_timeout` set on the device section also
  applies to the raw counter readings, so override it with
  `stale_timeout = 0` on each of them.

## 2. Current state (what is removed)

| Module | Today | After |
|---|---|---|
| dispatcher (`uvrgw_conf.c`) | stores value + update time + `updated` flag, drops NaN | stores value + valid flag, starts at the producer's `init_value`, owns the timeout watchdog |
| mqtt_logger | `stale_timeout` (s, default 600), "never received" warning | invalid → `null` |
| sunspec | `stale_timeout` (ms, default 30000), stale source → exception 4 | invalid source → exception 4 |
| counter | `max_gap` (ms, default 60000), publish only while power is fresh | publishes every tick; an invalid dispatch of the source is not data |
| eval | `max_age`, per-value skipping, per-input stale/never received logging | always computes; validity is the AND of the values read |
| outputs (Modbus, CAN, MQTT) | no staleness handling | unchanged behaviour; the callback gains a validity parameter they ignore |

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
      value grid_energy_imp { reg = 2  type = f32  stale_timeout = 60000 }
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
- The power source of a counter with `integrate_power` has no
  `stale_timeout`, if its producer is an input: the last power would be
  integrated forever.  A power produced by an eval is not checked
  (section 5).

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
- `uvrgw_conf_disp_val()` takes the dispatch mutex (section 4.3), stores
  value and validity under `last_lock`, sets `last_data` when `valid` is
  true, then fires the callbacks as today.  Inputs pass `true`, the
  watchdog `false`, eval the validity it computed, the counter its own
  validity.
- The dispatch callback becomes
  `int (*)(void *v, double f, bool valid)`.  Outputs ignore the flag; the
  counter uses it to tell data from a reset (section 4.8).
- `uvrgw_conf_get_val()` always returns the number; the return value says
  whether it is valid.  The `ts` parameter is gone.
- The guard in `uvrgw_conf_disp_val()` is extended from `isnan()` to
  `isfinite()`, so the invariant "a stored value is always a finite
  number" holds even if a module misses a check.
- Start values are not dispatched; outputs only receive real data, a
  computed value or a reset by the watchdog.

### 4.2 Watchdog

One thread for all names with `stale_timeout > 0`.  It wakes once per
second (the timeouts are seconds to minutes; no deadline scheduling).

Each such name has an armed deadline: armed at startup and on every
dispatch of real data (`last_data + stale_timeout`), and cleared once the
reset has been dispatched.  A name that never delivers is therefore reset
once at `stale_timeout` after startup, exactly like one that stops later,
and a reset is never repeated without new data.  Testing `valid` instead
would leave a never-received input unreset, unlogged, and its outputs
unwritten, because start values are not dispatched either.

For an expired name the watchdog sets `last_value = init_value` and
`valid = false` and dispatches the reset once with the producer's `val`
as source (locking: section 4.3).

The reset is logged once — "value 'x' timed out, reset to <init_value>",
or "value 'x' never received, set to <init_value>" if no data arrived at
all — and the next real data once ("value 'x' received again").  This
replaces the per-consumer stale logging of logger, SunSpec and eval.

### 4.3 Locking

The watchdog is the first dispatcher of a name that is not the name's
producer, so a reset and real data can meet.  Storing the value and
firing the callbacks must therefore be atomic per name.  Otherwise the
watchdog's callbacks can run after those of newer real data, leaving the
outputs and the counter at `init_value` while the stored value says the
real number is valid — a mismatch that lasts until the next data, which
is minutes for exactly the slow sources where the race is likely.

Each dispatcher therefore gets a dispatch mutex, held from storing the
value until all callbacks have returned.  Both `uvrgw_conf_disp_val()`
and the watchdog take it, and the watchdog re-checks the deadline under
it.  `last_lock` stays for the readers (`uvrgw_conf_get_val()`), so a
slow callback (an MQTT publish) does not block the eval thread, the
logger or SunSpec.

Nested dispatch is allowed and exists today: the counter dispatches its
total inside the callback of its source (`counter.c`, `source_update()`),
holding its own lock.  Locks are therefore taken along the data flow
(source name → counter lock → counter name → ...), always in that order,
so they cannot deadlock as long as the synchronous data flow has no
cycle; evals break every cycle, because their callbacks only set a flag
and evaluation runs in the eval thread.  A single global dispatch lock is
the one variant that does deadlock: the counter thread holds the counter
lock and dispatches, while a source thread holding the global lock calls
the counter callback and waits for the counter lock.  Document the rule
in the header.

### 4.4 Inputs

`mqtt.c`, `rest.c`, `can.c`, `mb.c`: parse `init_value` and
`stale_timeout` (value, else device section), pass them to
`uvrgw_conf_set_producer()`, dispatch with `valid = true` and keep
dropping invalid readings (unparsable payloads, `valid_if`, `sunspec_na`,
non-finite floats) so they count as no data.

### 4.5 Outputs

Behaviour unchanged.  Modbus, CAN and MQTT outputs always write the
number they are given and ignore the validity parameter: a timeout reset
is how a dead source reaches the controller as `init_value`.  A fallback
value at an output is configured as `init_value` on the input.

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
- An invalid dispatch of the source is not data, in both modes.  A device
  counter takes no reading, so no reset is detected and the total stays
  valid and frozen, like the register in the device; the next real
  reading continues without a step.  A `stale_timeout` on a counter
  source is therefore harmless and no config error, although section 1
  advises against one.
- Power integration: `max_gap` and `power_fresh()` are removed.  Each
  power value is integrated until the next one arrives and the total is
  published every tick.  An invalid power stops integration, so the reset
  value of the source does not matter.  A power source that is an input
  needs a `stale_timeout` (section 3); a power produced by an eval has
  none, and the last computed power is integrated until the eval runs
  again.

### 4.9 Eval

- Every value is evaluated and published on every evaluation; there is no
  skipping.  The number is dispatched either way, so an invalid input
  never stops a value from being published.
- A value is valid if all outside values it reads are valid *and* its
  result is finite.  The validity of the values read comes from the
  existing input closure (`v->ins`, which already covers values read
  through other values of the same eval).  A non-finite result leaves the
  number unchanged and is dispatched as invalid, so a broken expression is
  logged as `null` instead of as a frozen number, exactly like a value
  whose inputs died; it is logged once, as today.
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
- An eval's validity only changes when the eval runs.  With explicit
  `triggers` that do not include a value the expressions read, it lags
  behind that value's timeout until the next evaluation.
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
  the interval boundary); the stored value, the outputs and the counter
  all end at the same value.
- Loopback: a name that is an MQTT input on one connection and an output
  on another; the reset is not sent back to the input's topic.
- Power counter with a timeout on the source: integration stops after the
  timeout, the total keeps being published and stays valid.
- Device counter with a timeout on the source: the total stays valid and
  frozen, no reset is detected, and the next real reading continues
  without a step.
- Eval: a value reading two inputs, one of them invalid, is published as a
  number and logged as `null`; it becomes valid again with the data.  A
  non-finite result (division by zero) keeps the previous number, is
  logged as `null` and is reported once.
- Config errors: name without producer, logged value produced by an input
  without `stale_timeout`, integrating counter power source without one,
  negative `stale_timeout`, removed options.

## 7. Plan

One commit each, every step building (steps 1–5 done, see section 9):

1. Dispatcher: `valid` flag, `init_value`/`stale_timeout`/producer `val`
   in `uvrgw_conf_set_producer()`, validity parameter on
   `uvrgw_conf_disp_val()` and on the dispatch callback,
   `uvrgw_conf_get_val()` always returning the number, dispatch mutex.
   All modules adapted mechanically; their own staleness checks and the
   `ts` parameter stay until step 6.
2. Inputs: `init_value` and `stale_timeout` for MQTT, JSON, Modbus and
   CAN, with device section inheritance; inputs drop invalid readings
   themselves.
3. Watchdog thread and timeout logging.
4. Consumers: logger (`null` when invalid, `stale_timeout` removed),
   SunSpec (exception 4 when invalid, `stale_timeout` removed), counter
   (`max_gap` removed, own validity, invalid dispatch is not data).
5. Eval: validity as the AND over the input closure; `max_age`, skipping
   and per-input logging removed; `init` renamed to `init_value`.
6. Dispatcher cleanup: the `ts` parameter of `uvrgw_conf_get_val()`
   removed (`last_update` and `updated` are already gone, section 9).
7. Config checks: name without producer, logged/metered value produced by
   an input without `stale_timeout`, integrating counter power source.
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
- The dispatch callback gains a validity parameter.  Outputs ignore it;
  the counter uses it to tell data from a reset, so a `stale_timeout` on
  a counter source is harmless instead of a config error.  The advice
  stays to leave it off and to log the total instead of the raw reading
  (section 1).
- A value is invalid if its own result is not finite, not only if its
  inputs are: the flag answers "we have no number", whatever the
  reason.
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
- **Accepting a lost update instead of a dispatch mutex per name.**  The
  outputs and the counter would be left at `init_value` while the stored
  value is valid real data, for as long as the source's interval.  Only a
  single global dispatch lock deadlocks against the counter; a mutex per
  name is taken along the data flow and cannot (section 4.3).

## 9. Implementation notes

Deviations of the implemented steps from this guide, and the interim
state the following steps build on.

Step 1 (dispatcher):

- `updated` and `last_update` were replaced right away instead of in
  step 6: the dispatcher has `valid` and `last_data` (monotonic ms of the
  last valid data, 0 if none).  `uvrgw_conf_get_val()` keeps its `ts`
  parameter until step 6 and returns `last_data` there.
- Producer registrations: inputs pass their value pointer, eval passes
  `e` (its dispatch source) and `v->init`, counter passes `c`, 0 and 0.
  Counter and eval dispatch with `valid = true` until steps 4 and 5.
- `counter.c` `source_update()` receives `valid` but ignores it until
  step 4 (its local result variable was renamed to `ok`).

Step 2 (inputs):

- `stale_timeout < 0` is not checked after all sections are configured
  (section 3) but in `uvrgw_conf_set_producer()`, when the input value is
  registered.  It also refuses a non-finite `init_value` (not in the
  guide; it keeps NaN out of the stored value, like eval's `init`
  check).  Section settings without input values are not checked.
- Modbus (`dispatch_value()`) and JSON drop non-finite readings
  themselves; MQTT already did.

Step 3 (watchdog):

- The watchdog lives in `uvrgw_conf.c` as `uvrgw_conf_startup()` /
  `uvrgw_conf_shutdown()`; the thread only runs if a name has a
  `stale_timeout`.  It dispatches values, so `main.c` starts it after
  MQTT and stops it after eval and before `counter_stop()` /
  `mqtt_shutdown()`.
- Deadlines are armed in `uvrgw_conf_startup()` for names that have not
  delivered data yet (MQTT inputs may already have); `disp_lock` also
  protects the deadline and a `timed_out` flag for the "received again"
  log.

Step 4 (consumers):

- Logger and SunSpec pass `NULL` as `ts` to `uvrgw_conf_get_val()` until
  step 6.  The SunSpec state log names the first invalid source ("value
  'x' is invalid").
- Counter: an invalid source dispatch integrates the held power up to
  that moment and then stops integration (`power_stop()`); a device
  counter ignores it.  `power_ts` is gone with `max_gap`.  A dead power
  source is thus integrated until the watchdog's reset, i.e. up to
  `stale_timeout` plus the watchdog's 1 s check period.
- Not in the guide: a counter loaded from its state file has a total but
  was only dispatched with the next source reading, so a device counter
  whose source was down at startup stayed invalid (the logger wrote
  `null`, SunSpec answered exception 4) — unlike a source dying later,
  which leaves the total valid and frozen.  `counter_publish()`
  dispatches every counter that has a total once at startup; `main.c`
  calls it after `mqtt_startup()`, so the MQTT outputs receive it.  Done
  as a separate commit.

Step 5 (eval):

- `dt` restarts at 0 after a non-finite result (`has_last` is the
  finiteness of the previous result), as it did after a skipped
  evaluation; the guide does not say.
- `deps` is removed (it only served the skipping); `refs` stays for the
  input closure.
- A non-finite result is logged once as "value 'x' invalid: result is
  not finite" and its recovery as "value 'x' finite again".
- The NaN checks in the expression functions (`if`, `min`, `max`,
  `clamp`, `hyst`) are left unchanged; they are no longer needed but
  harmless.

Interim state until step 8: the README still documents the removed
options (`stale_timeout` of logger and SunSpec, `max_gap`, `max_age`,
eval `init`), and the production config does not load, because it still
uses them.
