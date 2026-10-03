# Implementation guide: Lua calculations and scripts

Status: design, not implemented.  Written as a self-contained brief for the
implementation sessions.  Step 1 (`calc`) is the immediate goal; step 2
(`script`) is designed here so step 1 does not need to be reworked later.

## 1. Goal

uvrgw gets an embedded Lua 5.4 interpreter for derived values and, later,
local control logic:

- **Step 1 – `calc`:** a derived value defined by a one-line Lua expression
  over other values, published under its own name.  First use case: log the
  sum of the three Shelly 3EM phase counters instead of the single phases:

  ```
  calc hp_energy_imp {
    expr    = "hp_energy_imp1 + hp_energy_imp2 + hp_energy_imp3"
    max_age = 10000
  }
  ```

- **Step 2 – `script`:** Lua script files with a small API (read values,
  publish values, react to value updates, timers) for stateful logic such as
  hysteresis, delays and minimum on/off times.

Why Lua instead of an own expression parser: stateless formulas would be easy
to parse, but every stateful function needed for control tasks (hysteresis,
timers, …) would be hand-made language design.  Lua is small (~250 KB),
proven for embedding (nginx, HAProxy, Wireshark) and available as Debian
package `liblua5.4-dev` (5.4.7 on the dev machine; runtime `liblua5.4-0`).

## 2. Current architecture (what the new module must fit into)

Read `README.md` (configuration, architecture overview) and these files
before starting: `uvrgw_conf.c/h` (config parser, dispatcher), `counter.c/h`
(closest existing pattern: derived values, own thread, dispatcher in and
out), `mqtt_logger.c/h` (reads values via `uvrgw_conf_get_val()`), `main.c`.

### 2.1 Value dispatcher (`uvrgw_conf.c/h`)

All values are linked by name.  Relevant API:

- `uvrgw_conf_get_dispatcher(name, alloc_cb)` — get/create the dispatcher of
  a name during configuration.  `alloc_cb = true` reserves a callback slot
  (the module wants to be notified about updates of this name).
- `uvrgw_conf_register_disp_cb(dp, val, cb)` — in `*_register_disp_cbs()`,
  after `init_dispatcher()`; `cb(val, f)` is called on every update.
- `uvrgw_conf_disp_val(dp, val, f)` — publish a value: stores last value and
  monotonic timestamp, then calls all callbacks except the one registered
  with the same `val` pointer (loopback protection).  NaN is dropped.
- `uvrgw_conf_get_val(dp, &f, &ts)` — thread-safe read of last value and
  update time (`utl_get_ticks()`, monotonic ms); false if never updated.
- `uvrgw_conf_state_dir()` — state directory (not needed for step 1).

Callbacks run in the thread of the source (Modbus, REST, CAN main loop,
libmosquitto, counter thread, …).  They must be quick and thread-safe.

### 2.2 Module pattern

Each module (`can`, `mb`, `mqtt`, `rest`, `sunspec`, `counter`) provides:
`*_init()`, `*_configure(cfg)`, `*_register_disp_cbs()`,
`*_unconfigure()`, `*_startup()`, `*_shutdown()`.  Config options are
declared in `uvrgw_conf.c` (`cfg_opt_t` arrays, `CFG_SEC(...)` in `opts[]`);
children are parsed with `uvrgw_conf_config_childs()`.  The calls are wired
in `uvrgw_conf_load()` / `uvrgw_conf_cleanup()` and `main.c`.

### 2.3 Conventions (keep them)

- C, 2-space indent, Doxygen comments for files/structs/functions in the
  existing style, `syslog()` for all messages.
- Validate everything at config load and refuse invalid configs with a clear
  message naming the section/value.
- Runtime problems are logged **once per state change** (e.g. "… invalid",
  "… valid again"), never per occurrence.
- Invalid/missing data → value is not published (becomes stale), never a
  substitute value like 0.
- Titled sections that must be unique use `CFGF_NO_TITLE_DUPES` (libconfuse
  otherwise silently merges sections with the same title).
- README: feature line, config section with example and notes,
  architecture overview entry.
- Build must be warning free with
  `make CFLAGS="-I. -Wall -Wextra -Wno-unused-parameter -O2"`.
- Commit messages: summary line, blank line, explanation, trailer.

## 3. Prerequisite: fix shutdown use-after-free (separate commit)

`main.c` shuts down in the order sunspec, rest, mqtt, mb, can, counter.
`mqtt_shutdown()` destroys the libmosquitto instances (`conn->mosq` stays
dangling), but the Modbus thread and the counter thread (publishes every
second while a power integration source is fresh) still run and can call
MQTT output callbacks → `mosquitto_publish()` on freed memory.  Small window,
but real; a Lua thread would add another producer.

Suggested fix: two-phase shutdown — first stop all threads that produce
values (rest, sunspec, mb, can RX is already stopped, counter tick, lua),
then shut down outputs/destroy libmosquitto, then save counter states.  E.g.
split `counter_shutdown()` into stopping the thread and saving, or give
modules a `*_stop()` (threads) and keep `*_shutdown()` (resources).  Also set
`conn->mosq = NULL` after destroy.  Startup has the mirror issue: sources may
publish before `mqtt_startup()`; `mosquitto_publish(NULL, …)` returns
`MOSQ_ERR_INVAL`, which is harmless, but document the intended order.

## 4. Design

### 4.1 One Lua thread, coalesced update events

A Lua state is not thread-safe, values arrive from many threads.  So:

- One module `lua.c/h` with **one Lua state and one thread**.
- Dispatcher callbacks of watched values do **not** call Lua.  They only mark
  the value as changed and wake the Lua thread (mutex + condition variable,
  or eventfd).  Coalescing: per watched value a "changed" flag; the Lua
  thread takes the current value via `uvrgw_conf_get_val()`.  No event queue
  that can overflow; only the latest value matters.
- The Lua thread processes changed values (re-evaluates dependent calcs, later
  calls script `on()` handlers) and timers, then waits again (timeout = next
  timer, max. e.g. 1 s).
- Everything published from Lua goes through `uvrgw_conf_disp_val()` with a
  per-calc / per-script `val` pointer (loopback protection).

### 4.2 `calc` (step 1)

```
calc <name> {
  expr    = "<Lua expression>"   # required
  max_age = 10000                # ms, default 10000; operand older -> no result
}
```

- `<name>`: the published value name; same rules as counter names but must be
  a valid Lua identifier as well (`[A-Za-z_][A-Za-z0-9_]*`, not a Lua
  keyword), so it can be used in other expressions.
- Compile at config load: `luaL_loadbufferx(L, "return " .. expr, …, "t")`
  with a dedicated environment table (see 4.4).  Syntax errors → config error.
- **Operands (dependencies)** are determined at load by a small lexer over the
  expression: identifiers `[A-Za-z_][A-Za-z0-9_]*` that are not preceded by
  `.` or `:`, are not Lua keywords, not inside string literals and not names
  of provided functions (see 4.4).  Each operand gets a dispatcher with
  `alloc_cb = true` and a callback that marks the calc dirty.
- **Evaluation**: when any operand changed.  The environment's `__index`
  resolves an operand name to its current value via `uvrgw_conf_get_val()`.
  If an operand was never received or is older than `max_age`, abort the
  evaluation (e.g. raise a specific Lua error) → nothing is published.
- **Result**: number → published (Lua integers converted to double); boolean
  → 1.0/0.0; nil/other/NaN/inf → not published.  Runtime errors are logged
  once per calc (state-change logging), recovery logged too.
- **Never received operands** (typos): warn once after `max_age` since startup
  (same idea as the logger's "never received" warning).
- **Cycles**: a calc must not depend on itself, directly or via other calcs →
  detect at config load (graph over calc names), config error.  A calc may
  use counters and counters may use calcs as source (counter sources that are
  calcs are fine: no locking cycle, because calcs publish from the Lua thread
  and counters only lock their own mutex).
- Partial updates: the three Shelly phases arrive one after another within a
  REST poll, so the sum is evaluated up to three times per poll with a mix of
  old/new phase values.  Harmless for logging and still monotonic for
  counters.  A later option `trigger = "<operand>"` could restrict evaluation
  to one operand; not part of step 1.

### 4.3 `script` (step 2, design only)

```
lua {
  script = "/etc/uvrgw/control.lua"   # may be given multiple times
  max_instructions = 1000000          # per callback, see 4.5
}
```

Script API (table `uvrgw`):

| Function | Meaning |
|---|---|
| `uvrgw.get(name)` | `value, age_ms` or `nil` if never received |
| `uvrgw.set(name, value)` | publish value via dispatcher (name must be declared, see below) |
| `uvrgw.on(name, fn)` | call `fn(value)` when `name` is updated (coalesced) |
| `uvrgw.every(ms, fn)` | periodic timer |
| `uvrgw.after(ms, fn)` | one-shot timer |
| `uvrgw.log(level, msg)` | syslog |

- `on()` names must be known at config load to reserve dispatcher callback
  slots (`alloc_cb`) — the dispatcher allocates callback arrays once in
  `init_dispatcher()`.  Options: run the script once during configuration
  (top level registers handlers) before `init_dispatcher()`, or declare
  inputs/outputs in the config (`inputs = {...}`, `outputs = {...}`).
  Decide in step 2; the first variant is nicer but runs user code during
  config load.
- Same for `set()`: published names must have a dispatcher (create with
  `alloc_cb = false` at config load).
- State lives in normal Lua variables of the script; it is lost on restart
  (persistence could use `state_dir` later).
- Helper library in Lua (optional): `hysteresis(on, off)`, `delay_on(ms)`,
  `min_on_off(...)` built on the API above, shipped as a Lua file.

### 4.4 Sandbox / environment

- Open only safe libraries: base (without `dofile`, `loadfile`, `load` with
  binary chunks, `require`), `math`, `string`, `table`, `utf8`, `os.time`,
  `os.clock`, `os.date`.  No `io`, no `os.execute`/`os.remove`/…, no
  `package`/`debug`.
- `calc` expressions run in an environment providing `math`, `min`, `max`,
  `abs`, `floor`, `ceil`, `clamp(x, lo, hi)` and operand resolution via
  `__index`.  Assignments to globals are not possible in an expression
  (`return <expr>`).

### 4.5 Robustness

- Every call into Lua via `lua_pcall()`; errors logged once per calc/script
  function (state change), never crash uvrgw.
- Instruction limit with `lua_sethook(L, hook, LUA_MASKCOUNT, n)` per call,
  raising an error on endless loops.
- Memory: optional custom allocator with a limit.
- A failing calc does not affect others.

### 4.6 Startup / shutdown order

- `lua_configure()` (compile, collect operands, create dispatchers) before
  `init_dispatcher()`; `lua_register_disp_cbs()` after it.
- `lua_startup()` **after** all output modules are started (published values
  reach outputs), before or after sources does not matter (changes are only
  flags).  Shutdown: stop the Lua thread together with the other producers
  (see section 3), before outputs are destroyed.

### 4.7 Build

- `Makefile`: `CFLAGS += $(shell pkg-config --cflags lua5.4)`,
  `LIBS += $(shell pkg-config --libs lua5.4)` (Debian: `lua5.4.pc`).
- README build prerequisites: add `liblua5.4-dev`; device needs `liblua5.4-0`.

## 5. Testing (what worked in this project)

All in the session scratchpad, never against production brokers/devices
with writes:

- Build a copy: `cp *.c *.h Makefile <scratch>/b && make -C <scratch>/b
  CFLAGS="-I. -Wall -Wextra -Wno-unused-parameter -O2"`.
- `LD_PRELOAD` shim redirecting `syslog`/`__syslog_chk` to stderr; for MQTT
  output checks a shim replacing `mosquitto_publish` (prints topic/payload),
  `mosquitto_connect_async` and `mosquitto_loop_start`.  For real MQTT tests
  run `/usr/sbin/mosquitto` (installed, not in PATH) on a high port with a
  small config, plus a tiny libmosquitto subscriber.
- Values from a fake JSON source: `python3 -m http.server` serving a file
  that the test driver rewrites (atomic `os.replace`).
- Drive uvrgw, servers and timing from one Python script with
  `subprocess.Popen` and stop with `send_signal(SIGINT)` +
  `communicate(timeout=…)` (detects shutdown hangs).  Do not background
  shell chains (`cd x && prog &`) — `$!` is then the subshell, and
  `pkill -f <pattern>` can match the own shell.
- `uvrgw.conf` (untracked, production config) must load with each build;
  CAN/RTU/SunSpec IPs are not available on the dev machine, so startup stops
  at "Could not set CAN interface name 'can0'" — that is the expected end.

Test cases for step 1:

1. Sum of three operands, result published on each operand update.
2. Operand never received / older than `max_age` → no result; recovery.
3. Syntax error → config error with message; runtime error (e.g. `nil +
   1` via unknown function) → logged once, recovery logged.
4. Boolean result → 1/0; integer arithmetic (`7 // 2`) → double.
5. Cycle `a = b + 1`, `b = a + 1` → config error.
6. Endless loop in expression (e.g. via a function in step 2) → instruction
   limit error, uvrgw keeps running.
7. Calc as counter source and counter as calc operand.
8. Sandbox: `io`, `os.execute`, `require` not available.
9. Shutdown while values arrive (no hang, no crash; run under valgrind once).

## 6. Plan

1. Fix the shutdown use-after-free (section 3) — own branch/commit.
2. `lua.c/h` with Lua thread, change flags, sandbox, `calc`; config options;
   README; tests; production config: replace the three `hp_energy_imp*` /
   `hp_energy_exp*` logger values by `calc` sums.
3. Later: `script` API (section 4.3) once the first control task is defined;
   decide where control logic lives (simple, safety-relevant local rules in
   uvrgw; optimisation with forecasts/prices in the planned EMS).

## 7. Open decisions

- Module/section names: `calc` and `lua` (or `script`).
- Default `max_age` (10 s proposed; must exceed the slowest operand's update
  interval — UVR CAN values may only be sent every few minutes).
- Step 2: register `on()` handlers by running the script at config load, or
  declare inputs/outputs in the config.
