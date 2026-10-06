# Implementation guide: calculated values and control logic (`eval`)

Status: implemented (`eval.c/h`, README section "Calculated values and
control logic").  Decisions taken during implementation, beyond this
guide:

- A local value whose name is used outside of the eval (by any module)
  is a config error; it would hide that value in the eval.
- An eval without `period` and `triggers` that reads no outside value
  would only be evaluated at startup: config error.  A trigger given
  twice is a config error.
- `if`, `min`, `max`, `clamp` and `hyst` return NaN if an argument is
  NaN (e.g. `sqrt(-1)`), so the value is skipped instead of hiding it.
- Logging (4.4): stale and never received inputs are logged once per
  eval and input ("input 'x' is stale" / "never received" / "valid
  again"), per value only non-finite results.  Values skipped because of
  an input or a skipped dependency are not logged individually.
- tinyexpr is not copied unmodified: it gets `te_is_builtin()` (4.8)
  instead of a copied list of built-in names in `eval.c`.
- Producer owners are named by module and section: `modbus_tcp '<ip>'`,
  `modbus_rtu '<interface>'`, `can '<interface>'`, `mqtt '<host>'`,
  `json '<url>'`, `counter '<name>'`, `eval '<name>'`.
- Two periodic evals whose values depend on each other directly (a value
  of A reads a value of B that reads a value of A) never start, since
  each waits for the other's first result.  Only periodic evals where at
  least one side does not depend on the other run.

## 1. Goal

uvrgw gets `eval` sections: groups of values defined by expressions over
other values, evaluated periodically or when input values arrive.  They
cover derived values (e.g. the sum of the three Shelly 3EM phase counters)
and simple local control logic in the style of the function blocks of the
TA controllers (comparison, hysteresis, timers, logic, scaling, ...).

```
eval solar {
  period  = 1000
  max_age = 10000
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
(C, zlib licence), release v1.1.1, copied into the repository (4.8).  An
earlier attempt with embedded Lua (branch `lua-calc`) was dropped: a Lua
expression does not tell which values it reads, so dependencies had to be
recorded at runtime (dispatcher hook, locked dependency lists, runtime
cycle handling), and the interpreter needed a sandbox.  tinyexpr binds all
names at compile time: the values an expression reads are known at config
load, unknown names are config errors, and there is nothing to sandbox (no
loops, no allocation during evaluation).

## 2. Current architecture (what the new module must fit into)

Read `README.md` and these files before starting: `uvrgw_conf.c/h` (config
parser, dispatcher), `counter.c/h` (closest existing pattern: derived
values, own thread, dispatcher in and out), `mqtt_logger.c/h` (reads
values via `uvrgw_conf_get_val()`, stale handling), `main.c`
(startup/shutdown order).

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

The dispatcher does not know whether a module produces or consumes a
name.  This guide adds that information (4.2).

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
  substitute value.  A derived value must not outlive its inputs: if an
  input is stale, the derived value is not published either.
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
  max_age  = <ms>                 # max. age of outside values, default 600000, 0 = no limit

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
    triggers ("default triggers").
  - `period` and `triggers` together are a config error (may be allowed
    later as "on trigger, and at least every period").
- `value` sections are evaluated in config order.  This is why there is
  only one section keyword: libconfuse keeps the order within one keyword,
  not across different ones.
- `local = true`: the value is private to the eval (no dispatcher, not
  published); the name can be reused in other evals.
- `init`: value before the first evaluation, for values that read
  themselves or are read by an earlier `value` of the same eval.
- `max_age`: a value is not evaluated if an outside value it depends on
  is older (4.4).  The default (600000 ms = 10 min) matches the default
  `stale_timeout` of the MQTT logger and is long enough for MQTT sensors
  that publish rarely; control logic sets a tighter limit.

Config errors: expression syntax error or unknown name (with the position
reported by tinyexpr), invalid value name, a value name defined twice
(in the same or another eval), a value name produced by another module
(4.2), no `value` section, `period` and `triggers` together,
`period <= 0`, `max_age < 0`, unknown trigger name, trigger naming a value
of the same eval, trigger cycle (4.3).

## 4. Design

### 4.1 Names in expressions

A name in an expression is resolved in this order:

1. `dt`: seconds since the previous evaluation of this value (4.5).
2. A value of the same eval (local or not): its current state.  A value
   defined further up has the result of this evaluation, the value itself
   and values further down have the result of the previous one (or
   `init`).
3. An **outside value**: any other dispatcher name known in the
   configuration, including values of other evals and counters.
4. An expression function (4.6): the tinyexpr built-ins and the functions
   added by uvrgw.

Names must be identifiers as tinyexpr accepts them: a letter, then
letters, digits and `_`.

tinyexpr looks up the variable list before its built-in functions, so a
variable named like a built-in silently replaces it in every expression
(tested: a variable `e` makes `e` return its value instead of 2.718...).
Therefore:

- Value names of an eval must not be `dt` or the name of an expression
  function (built-in or uvrgw): config error.
- Outside values whose name is not an identifier, is `dt`, or equals the
  name of an expression function are not put into the variable list.
  They cannot be used in expressions; using one gives an unknown name
  error or calls the function.  The built-in names are only defined in
  the `functions[]` table of `tinyexpr.c`; `eval.c` asks tinyexpr with
  `te_is_builtin()`, added to tinyexpr by uvrgw (4.8).

Unknown names are compile errors, so typos are found at config load.
"Known" means a dispatcher exists, i.e. any module references the name.
A name that exists but is never received is reported at runtime (4.4).

### 4.2 Compilation and dependencies

`eval_configure()` runs after all other `*_configure()` calls, so all
dispatcher names exist, in two passes over all evals:

1. Parse the sections, create the dispatchers of all non-local values
   (`uvrgw_conf_get_dispatcher(name, false)`), so evals can read values of
   evals defined later in the file.
2. Per eval, build the `te_variable` list (`dt`, own values, outside
   values except the excluded names of 4.1, uvrgw functions) and compile
   each expression with `te_compile()`.

tinyexpr binds a variable to the address of a `double`.  Each eval has an
array of doubles ("slots"): `dt`, one per own value, one per outside
value.  The dispatcher list has to be made accessible for this (new
accessor in `uvrgw_conf.c`, e.g. returning the list head).

**Dependencies.**  After compiling, walk each compiled expression tree
(`te_expr` is public: nodes of type `TE_VARIABLE` carry the bound
address; function and closure nodes have `type & 7` parameters) and
record per value:

- the outside values it reads directly,
- the own values defined **above** it that it reads (they carry the
  result of this evaluation, so their validity propagates, 4.4).

Own values read from the same or a later position are previous state and
are not dependencies in this sense.

**Input closure.**  The outside values a value depends on are the ones it
reads directly plus those of all other own values it reads (above or
below), transitively; computed once after the walk (fixed point over the
`refs` lists).  The validity check (4.4) uses this closure.  Without it,
a value reading the previous state of a value further down would keep
publishing results from that value's last state after its inputs went
stale (found in review: `back = later * 2` with `later = y` kept
publishing after `y` stopped).  The tempting alternative rule "skip if
the value below was skipped last time" deadlocks with feedback between
values (a reads b below, b reads a above: once a is skipped, b is
skipped because a was not updated, and a stays skipped because b was).
The static closure cannot block recovery.  The outside values read by any value of the eval
are the ones refreshed before an evaluation and used as default triggers.
Constants are folded at compile time (`optimize()` replaces pure function
nodes with only constant parameters); variable nodes are never folded,
so the walk finds every variable read.

**Producers.**  Add a "produced" flag to the dispatcher, set by a new
function (e.g. `uvrgw_conf_set_producer(dp, owner)`) that returns an
error if the name already has a producer.  Every module calls it for the
names it publishes: Modbus/CAN/REST/MQTT inputs, counters, evals (pass 1).
A name produced twice is a config error naming both owners ("value 'x'
of eval 'a' is already produced by modbus_tcp 'y'").  This replaces the
counter-only check and also catches two inputs publishing the same name,
which today interleave silently.  Check where each module creates the
dispatchers of its inputs; outputs and sources read by counters, the
logger and SunSpec are consumers and do not call it.

Triggers: `uvrgw_conf_get_dispatcher(name, true)` for each trigger
(explicit or default), and a callback registered in
`eval_register_disp_cbs()` with the eval as `val` pointer.

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
published by A.  "Trigger" means the effective triggers: the `triggers`
list, or the default triggers (all outside values read) of an eval
without `period` and `triggers`.  A cycle in this graph would evaluate
forever, so it is a config error (depth first search at config load,
message naming the evals of the cycle).  Example: two evals without
`period`/`triggers` that read each other's values form a cycle.  Periodic
evals have no triggers and are not part of the graph: reading each
other's values is fine, they see the result of the other's last
evaluation.  A cycle through another module (e.g. eval → counter → eval)
is not detected.

Results are published with `uvrgw_conf_disp_val(dp, eval, f)`.  Every
evaluation publishes all non-local values that were evaluated, also
unchanged ones (counters and the logger judge freshness by updates).

### 4.4 Evaluation

Validity is decided per value, not per eval, so a dead input only stops
the values that depend on it, and stale inputs never produce fresh
looking outputs.

1. Copy the outside values read by the eval into their slots
   (`uvrgw_conf_get_val()`) and mark each as valid or invalid: invalid if
   never received, or older than `max_age` (if not 0).
2. Evaluate the `value` sections in order.  For each value:
   - If one of the outside values in its input closure (4.2) is invalid,
     or one of its own-value dependencies (above it) was not updated in
     this evaluation: the value is **skipped**.
   - Otherwise set `dt` (4.5) and evaluate (`te_eval()`).  If the result
     is not finite (NaN/inf), the value is **skipped**.
   - Otherwise store the result in the slot of the value; it is
     **updated**.
   - A skipped value keeps its previous state in its slot and is not
     published.  Values further up that read it as previous state get
     that state only if their own input closure is valid; this happens
     after a non-finite result (they see the last finite one) and in the
     first evaluation after a gap (one evaluation late, by definition of
     reading a value further down).
3. Publish the updated non-local values.

Only the final result of a value is checked, not intermediate results:
`if(x != 0, a / x, 0)` is a working guard although `if()` evaluates both
arguments and `a / x` is inf for `x = 0`.

A skipped value is not published, so it becomes stale for its consumers.
Its dependents in the same eval are skipped in the same evaluation; in
other evals once it is older than their `max_age`.

Logging, once per state change per value: "eval '<e>': value '<v>' not
evaluated: input '<x>' is stale" / "... result is not finite" / "... '<u>'
not evaluated" (dependency) and "eval '<e>': value '<v>' valid again".
Log the cause only for the first value of a chain where it makes sense;
at least avoid logging the same stale input once per dependent value
(e.g. log stale inputs once per eval and input, and per value only
non-finite results).  A value never received is reported once after
`max_age`, or after 10 minutes with `max_age = 0` (as the logger does),
not at the first skipped evaluation: values are normally missing for a
moment after startup.

### 4.5 `dt`

Seconds (double) since the previous update of the value being evaluated;
0 for its first evaluation and for the first one after it was skipped.
`dt` is a single slot set before each `te_eval()` (evaluation is
sequential).  Time dependent expressions use it instead of assuming a
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

Precedence, highest first: unary `- + !`, `^`, `* / %`, `+ -`,
`< <= > >=`, `== !=`, `&&`, `||`.

tinyexpr behaviour to know (tested with v1.1.1):

- `tinyexpr.c` is compiled with `TE_POW_FROM_RIGHT` (4.8), so `^` is
  right associative and binds tighter than unary minus:
  `-2^2 = -4`, `2^3^2 = 512` (without the define: 4 and 64).
- `log` is `log10` (`TE_NAT_LOG` not defined).  Document `ln` and
  `log10` only; `log` stays available but ambiguous.
- `!` binds tighter than `+`: `!0+1 = 2`.
- One-argument functions also work without parentheses: `abs -3 = 3`.
- `(a, b)` is a comma operator and returns `b`.
- Division by zero gives inf, not an error (handled by 4.4).

Added by uvrgw (registered as tinyexpr functions, `TE_FLAG_PURE`):

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

- tinyexpr v1.1.1 is copied into the repository (no submodule):
  `tinyexpr/tinyexpr.c`, `tinyexpr/tinyexpr.h`, `tinyexpr/LICENSE`, plus
  `tinyexpr/README.uvrgw` naming the upstream URL, the release tag and
  commit (`6d2233ce14dca063994708a91b3b98e24e8aa0c3`) and the uvrgw
  modification.  Do not copy the examples and tests (they have their
  own `main()`).  v1.1.1 contains the comparison and logic operators;
  `master` differs from it only in copyright headers.
- The only modification is `te_is_builtin()` in `tinyexpr.c/h`
  (`tinyexpr/uvrgw.patch`, marked "uvrgw:" in the source, as the zlib
  licence requires for altered versions).  tinyexpr has no public way to
  list its built-ins (`functions[]` is static), and a copied list in
  `eval.c` would silently go stale on a tinyexpr update; with the patch
  forgotten, linking fails instead.
- The compile options are set in the `Makefile`, not by editing
  `tinyexpr.c`: `-DTE_POW_FROM_RIGHT` for `tinyexpr/tinyexpr.o` only.
- `Makefile`: add `tinyexpr/tinyexpr.c` to the sources and
  `-Itinyexpr` to the include path, both in variables that are not
  overridden by `CFLAGS` given on the command line (the test build passes
  its own `CFLAGS`).  v1.1.1 compiles warning free with the test flags
  and `-DTE_POW_FROM_RIGHT` (checked).
- README: the licence section ("No license is currently specified")
  gets a note on the bundled tinyexpr (zlib, `tinyexpr/LICENSE`).  No
  change to the build instructions (no submodule, no new package).
- Module `eval.c/h`, prefix `eval_`.

### 4.9 README

The README section for `eval` (written with the implementation) covers,
besides config example and options:

- evaluation modes and default triggers,
- names in expressions (4.1), including the rule that outside values
  named like a function or `dt` cannot be used,
- per-value validity and `max_age` (4.4): a value whose inputs are stale
  is not published, dependents are skipped too,
- operators, precedence and functions (4.6), including the guard example
  `if(x != 0, a / x, 0)` and that only the final result of a value is
  checked for NaN/inf,
- `dt` and explicit state (`hyst(solar_pump, ...)`),
- state is lost on restart; safety limits belong in the controller (7).

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

1. Sum of three values with default triggers (up to three evaluations per
   poll) and with `triggers` on the last one (one evaluation).
2. Periodic eval: hysteresis and run time counter as in section 1;
   `dt` matches the period; `init` is used in the first evaluation.
3. Value order: a value reads one defined above (new result) and one
   defined below (previous result).
4. Per-value validity: eval with two independent values and one
   dependent value; one input stops → only the values depending on it
   stop being published (also when it is read via a value above), the
   other one continues; recovery; `dt` is 0 after the gap; log messages
   once per state change and not repeated per dependent value.
5. Periodic eval with the default `max_age` and an input that stops:
   output stops after 600 s (shortened `max_age` in the test).
   `max_age = 0`: output continues.
6. Division by zero → value skipped, previous state kept, dependents
   skipped, logged once, recovery logged; the guard
   `if(x != 0, a / x, 0)` publishes 0.
7. Config errors of section 3, each with a clear message (syntax error
   and unknown name with position); value name equal to a Modbus/REST
   input or a counter; two inputs of different modules with the same
   name (now a config error, check the production config still loads).
8. Name shadowing: an outside value named `e` or `min` → `e` is still
   2.718..., `min()` still works; an eval value named `e` → config error.
9. tinyexpr options: `-2^2 = -4`, `2^3^2 = 512`.
10. Trigger cycle between two evals with explicit triggers and between
    two evals with default triggers → config error; the same evals with
    `period` load and run.
11. Eval value as counter source and counter as eval input; eval reading
    a non-local value of another eval; local names reused in two evals.
12. Shutdown while values arrive (no hang, no crash; run under valgrind
    once).

## 6. Plan

1. Copy tinyexpr v1.1.1, build integration, README licence note.
2. Producer flag in the dispatcher and `uvrgw_conf_set_producer()` calls
   in all producing modules (own commit; verify the production config
   still loads).
3. `eval.c/h`: config, compilation, dependencies, thread, triggers,
   evaluation, expression functions; dispatcher list accessor; wiring in
   `uvrgw_conf.c` and `main.c`.
4. README (4.9); tests of section 5; production config: `hp_energy_imp` /
   `hp_energy_exp` as evals.

## 7. Decisions and limits

- `max_age` defaults to 600000 ms (10 min).  A derived value must not be
  published from stale inputs, otherwise consumers (logger, counters)
  treat it as fresh.  Because validity is per value (4.4), a stale input
  only stops the values depending on it, so a default limit does not
  stop unrelated outputs of the eval.  `max_age = 0` disables the check
  for evals that intentionally use the last known value.
- State is lost on restart (values start at `init`).  Accepted for now;
  persistence via `state_dir` only if a use case needs it.
- If uvrgw stops, outputs keep their last state.  Safety limits (e.g.
  tank over-temperature) stay in the controller.
