# Implementation guide: calculated values and control logic (`eval`)

Status: design only, nothing implemented.  Written as a self-contained
brief for the implementation session.

## 1. Goal

uvrgw gets `eval` sections: groups of values defined by expressions over
other values, evaluated periodically or when input values arrive.  They
cover derived values (e.g. the sum of the three Shelly 3EM phase counters)
and simple local control logic in the style of the function blocks of the
TA controllers (comparison, hysteresis, timers, logic, scaling, ...).

```
eval solar {
  period = 1000
  value diff       { expr = "collector_temp - tank_temp"  local = true }
  value solar_pump { expr = "hyst(solar_pump, diff, 8, 4) && tank_temp < 90" }
  value pump_on_s  { expr = "if(solar_pump, pump_on_s + dt, 0)" }
}

eval hp_energy {
  triggers = {"hp_energy_imp3"}
  value hp_energy_imp { expr = "hp_energy_imp1 + hp_energy_imp2 + hp_energy_imp3" }
}
```

Expressions are evaluated with [tinyexpr](https://github.com/codeplea/tinyexpr)
(C, zlib licence).  An earlier attempt with embedded Lua (branch
`lua-calc`) was dropped: a Lua expression does not tell which values it
reads, so dependencies had to be recorded at runtime (dispatcher hook,
locked dependency lists, runtime cycle handling), and the interpreter
needed a sandbox.  tinyexpr binds all names at compile time: the values an
expression reads are known at config load, unknown names are config
errors, and there is nothing to sandbox (no loops, no allocation during
evaluation).

## 2. Current architecture (what the new module must fit into)

Read `README.md` and these files before starting: `uvrgw_conf.c/h` (config
parser, dispatcher), `counter.c/h` (closest existing pattern: derived
values, own thread, dispatcher in and out), `mqtt_logger.c/h` (reads
values via `uvrgw_conf_get_val()`), `main.c` (startup/shutdown order).

### 2.1 Value dispatcher (`uvrgw_conf.c/h`)

All values are linked by name.  Relevant API:

- `uvrgw_conf_get_dispatcher(name, alloc_cb)` — get/create the dispatcher
  of a name during configuration.  `alloc_cb = true` reserves a callback
  slot (the module wants to be notified about updates of this name).
- `uvrgw_conf_register_disp_cb(dp, val, cb)` — in `*_register_disp_cbs()`,
  after `init_dispatcher()`; `cb(val, f)` is called on every update.
- `uvrgw_conf_disp_val(dp, val, f)` — publish a value: stores last value
  and monotonic timestamp, then calls all callbacks except the one
  registered with the same `val` pointer (loopback protection).
- `uvrgw_conf_get_val(dp, &f, &ts)` — thread-safe read of last value and
  update time (`utl_get_ticks()`, monotonic ms); false if never updated.

Callbacks run in the thread of the source (Modbus, REST, CAN main loop,
libmosquitto, counter thread).  They must be quick and thread-safe.

### 2.2 Module pattern

Each module provides `*_init()`, `*_configure(cfg)`,
`*_register_disp_cbs()`, `*_unconfigure()`, `*_startup()`,
`*_shutdown()`.  Config options are declared in `uvrgw_conf.c`
(`cfg_opt_t` arrays, `CFG_SEC(...)` in `opts[]`); children are parsed with
`uvrgw_conf_config_childs()`.  The calls are wired in `uvrgw_conf_load()` /
`uvrgw_conf_cleanup()` and `main.c`.

### 2.3 Conventions (keep them)

- C, 2-space indent, Doxygen comments in the existing style, `syslog()`
  for all messages.
- Validate everything at config load and refuse invalid configs with a
  clear message naming the section/value.
- Runtime problems are logged once per state change ("... invalid",
  "... valid again"), never per occurrence.
- Invalid/missing data → value is not published (becomes stale), never a
  substitute value.
- Titled sections that must be unique use `CFGF_NO_TITLE_DUPES`.
- README: feature line, config section with example and notes,
  architecture overview entry.
- Build must be warning free with
  `make CFLAGS="-I. -Wall -Wextra -Wno-unused-parameter -O2"`.

## 3. Configuration

```
eval <name> {
  period   = <ms>                 # evaluate every <ms>
  triggers = {"<value>", ...}     # or: evaluate when one of these values is updated
  max_age  = <ms>                 # optional, 0 (default) = no limit

  value <value name> {
    expr  = "<expression>"        # required
    local = true                  # optional, default false
    init  = <number>              # optional, default 0
  }
  ...
}
```

- `<name>` identifies the eval in log messages; unique
  (`CFGF_NO_TITLE_DUPES`).
- **Evaluation mode**, exactly one of:
  - `period`: evaluate every `period` ms.
  - `triggers`: evaluate when one of the listed values is updated.
  - neither: all outside values (see 4.1) read by the expressions are
    triggers.
  - `period` and `triggers` together are a config error (may be allowed
    later as "on trigger, and at least every period").
- `value` sections are evaluated in config order.  This is why there is
  only one section keyword: libconfuse keeps the order within one keyword,
  not across different ones.
- `local = true`: the value is private to the eval (no dispatcher, not
  published); the name can be reused in other evals.
- `init`: value before the first evaluation, for values that read
  themselves or are read by an earlier `value` of the same eval.
- `max_age`: if an outside value read by the eval is older, the eval is
  skipped (4.4).

Config errors: expression syntax error or unknown name (with the position
reported by tinyexpr), invalid value name, a value name defined twice
(in the same or another eval, or equal to a counter name), no `value`
section, `period` and `triggers` together, `period <= 0`, unknown trigger
name, trigger naming a value of the same eval, trigger cycle (4.3).

## 4. Design

### 4.1 Names in expressions

A name in an expression is resolved in this order:

1. `dt`: seconds since the previous evaluation of this eval (4.5).
2. A value of the same eval (local or not): its current state.  A value
   defined further up has the result of this evaluation, the value itself
   and values further down have the result of the previous one (or
   `init`).
3. An **outside value**: any other dispatcher name known in the
   configuration, including values of other evals and counters.
4. An expression function (4.6).

Names must be identifiers as tinyexpr accepts them: a letter, then
letters, digits and `_`.  Value names of an eval must follow this rule
and must not be `dt` or the name of an expression function.  Outside
values with other names cannot be used in expressions.

Unknown names are compile errors, so typos are found at config load.
"Known" means a dispatcher exists, i.e. any module references the name.
A name that exists but is never received is reported at runtime (4.4).

### 4.2 Compilation

`eval_configure()` runs after all other `*_configure()` calls, so all
dispatcher names exist, in two passes over all evals:

1. Parse the sections, create the dispatchers of all non-local values
   (`uvrgw_conf_get_dispatcher(name, false)`), so evals can read values of
   evals defined later in the file.
2. Per eval, build the `te_variable` list (`dt`, own values, outside
   values, expression functions) and compile each expression with
   `te_compile()`.

tinyexpr binds a variable to the address of a `double`.  Each eval has an
array of doubles ("slots"): `dt`, one per own value, one per outside
value.  The dispatcher list has to be made accessible for this (new
accessor in `uvrgw_conf.c`, e.g. returning the list head).

After compiling, the outside values actually read are determined by
walking the compiled expression trees (`te_expr` is public: nodes of type
`TE_VARIABLE` carry the bound address; function and closure nodes have
`type & 7` parameters).  Only these are refreshed before an evaluation,
checked for `max_age`, and used as default triggers.  Verify the tree
walk against the pinned tinyexpr version (constants are folded at compile
time).

Triggers: `uvrgw_conf_get_dispatcher(name, true)` for each trigger, and a
callback registered in `eval_register_disp_cbs()` with the eval as `val`
pointer.

### 4.3 Thread and triggers

One thread for all evals; expressions are never evaluated in the thread of
a source.

- Trigger callback (source thread): sets the `pending` flag of the eval
  under the module mutex and signals the condition variable.  Several
  triggers arriving together cause one evaluation (coalescing; only the
  latest values matter).
- Eval thread: waits (`pthread_cond_timedwait()`, `CLOCK_MONOTONIC`) for
  a pending flag or the next due time of a periodic eval, evaluates what
  is due in config order, publishes without holding the module mutex.
- Periodic evals keep a fixed schedule (`next += period`); if evaluations
  were missed, they are skipped, not caught up.
- At startup all evals are evaluated once.

Trigger cycles: eval A triggers eval B if a trigger of B is a value
published by A.  A cycle in this graph would evaluate forever, so it is a
config error (depth first search at config load).  Periodic evals have no
triggers and are not part of the graph: reading each other's values is
fine, they see the result of the other's last evaluation.  A cycle
through another module (e.g. eval → counter → eval) is not detected.

Results are published with `uvrgw_conf_disp_val(dp, eval, f)`.  Every
evaluation publishes all non-local values, also unchanged ones (counters
and the logger judge freshness by updates).

### 4.4 Evaluation

1. Copy the outside values read by the eval into their slots
   (`uvrgw_conf_get_val()`).  If one was never received, or is older than
   `max_age` (if set): skip the evaluation.
2. Set `dt`, save the slots of the own values.
3. Evaluate the `value` sections in order (`te_eval()`), each result goes
   into the slot of the value.  A result that is not finite (NaN/inf, e.g.
   division by zero) aborts the evaluation: the saved slots are restored
   and nothing is published.
4. Publish the non-local values.

A skipped or aborted evaluation leaves the state untouched and publishes
nothing, so the values of the eval become stale for their consumers.

Logging, once per state change per eval: "value '<x>' is stale / result of
'<v>' is not finite, not evaluated" and "valid again".  A value never
received is reported once after `max_age`, or after 10 minutes without
`max_age` (as the logger does), not at the first skipped evaluation:
values are normally missing for a moment after startup.

### 4.5 `dt`

Seconds (double) since the previous completed evaluation of the eval; 0
for the first evaluation and for the first one after a skipped or aborted
evaluation.  Time dependent expressions use it instead of assuming a
period, so they work in all modes and do not jump after a gap:

```
value pump_on_s { expr = "if(solar_pump, pump_on_s + dt, 0)" }
```

### 4.6 Expression functions

All values are doubles; 0 is false, everything else true.  Comparisons
and logic operators return 1 or 0.

From tinyexpr: `+ - * / % ^`, `< <= > >= == !=`, `&& || !`, `abs`,
`floor`, `ceil`, `sqrt`, `pow`, `exp`, `ln`, `log10`, trigonometric
functions, `pi`, `e`.  `&&` and `||` evaluate both sides (no short
circuit; harmless, expressions have no side effects).

Added by uvrgw (registered as tinyexpr functions):

| Function | Result |
|---|---|
| `if(c, a, b)` | `a` if `c` is true, else `b` (both are evaluated) |
| `min(a, b)`, `max(a, b)` | smaller / larger value |
| `clamp(x, lo, hi)` | `x` limited to `[lo, hi]` |
| `hyst(prev, x, on, off)` | 1 if `x >= on`, 0 if `x <= off`, else `prev` as 1/0; with `on < off` inverted (1 if `x <= on`, 0 if `x >= off`) |

State is always explicit: a function that needs memory gets it as an
argument, normally the value itself (`hyst(solar_pump, ...)`).  Further
functions (characteristic curve, time of day / weekday for time switches)
are added when the first use case needs them.

### 4.7 Startup / shutdown order

- `eval_configure()` last in `uvrgw_conf_load()`, before
  `init_dispatcher()`; `eval_register_disp_cbs()` after it.
- `eval_startup()` after the outputs (`mqtt_startup()`), so the first
  results are not lost.
- The eval thread is a value producer: `eval_shutdown()` together with the
  other producers, before `counter_stop()` / `mqtt_shutdown()` (see the
  file comment of `main.c`).  Trigger callbacks arriving later only set
  flags.

### 4.8 Build

- tinyexpr as git submodule `tinyexpr/` (https://github.com/codeplea/tinyexpr),
  pinned to a commit of `master` that has the comparison and logic
  operators (older versions do not).
- `Makefile`: add `tinyexpr/tinyexpr.c` to the sources and
  `-Itinyexpr` to the include path (in a variable that is not overridden
  by `CFLAGS` given on the command line).  Only this file is compiled;
  the repository also contains examples and tests with their own `main()`.
- README build instructions: `git clone --recurse-submodules`, or
  `git submodule update --init` in an existing clone.
- Module `eval.c/h`, prefix `eval_`.

## 5. Testing

In the session scratchpad, never against production brokers/devices with
writes:

- Build a copy with
  `make CFLAGS="-I. -Wall -Wextra -Wno-unused-parameter -O2"`.
- `LD_PRELOAD` shim redirecting `syslog`/`__syslog_chk` to stderr; for
  MQTT output checks a shim replacing `mosquitto_publish` (prints
  topic/payload), `mosquitto_connect_async` and `mosquitto_loop_start`.
- Values from a fake JSON source: `python3 -m http.server` serving a file
  that the test driver rewrites (atomic `os.replace`).
- Drive uvrgw, servers and timing from one Python script with
  `subprocess.Popen`; stop with `send_signal(SIGINT)` +
  `communicate(timeout=...)` (detects shutdown hangs).
- `uvrgw.conf` (untracked, production config) must load with each build;
  CAN/RTU/SunSpec are not available on the dev machine, so startup stops
  at "Could not set CAN interface name 'can0'" — the expected end.

Test cases:

1. Sum of three values with default triggers (three evaluations per
   poll) and with `triggers` on the last one (one evaluation).
2. Periodic eval: hysteresis and run time counter as in section 1;
   `dt` matches the period; `init` is used in the first evaluation.
3. Value order: a value reads one defined above (new result) and one
   defined below (previous result).
4. Outside value never received / older than `max_age` → nothing
   published; recovery; `dt` is 0 after the gap.
5. Division by zero → evaluation aborted, state restored, logged once,
   recovery logged.
6. Config errors of section 3, each with a clear message (syntax error
   and unknown name with position).
7. Trigger cycle between two evals → config error; the same two evals
   with `period` load and run.
8. Eval value as counter source and counter as eval input; eval reading
   a non-local value of another eval; local names reused in two evals.
9. Shutdown while values arrive (no hang, no crash; run under valgrind
   once).

## 6. Plan

1. Add the tinyexpr submodule and build integration.
2. `eval.c/h`: config, compilation, thread, triggers, evaluation,
   expression functions; dispatcher list accessor; wiring in
   `uvrgw_conf.c` and `main.c`.
3. README; tests of section 5; production config: `hp_energy_imp` /
   `hp_energy_exp` as evals.

## 7. Decisions and limits

- `max_age` defaults to 0 (no limit).  A limit by default would stop the
  outputs of an eval when one input stops, although the others are still
  valid; that is the bigger problem.  Evals that must not compute from old
  inputs set `max_age`.
- State is lost on restart (values start at `init`).  Accepted for now;
  persistence via `state_dir` only if a use case needs it.
- If uvrgw stops, outputs keep their last state.  Safety limits (e.g.
  tank over-temperature) stay in the controller.
