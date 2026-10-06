# uvrgw

C daemon (gateway) running on the heating system hardware: CAN (TA UVR),
Modbus RTU/TCP, MQTT, REST/JSON, SunSpec meter emulation, persistent
counters, calculated values (eval).  `README.md` documents configuration
and architecture; read it first.

## Working rules

- Change files only when asked.  A question or a review request is not a
  request to change anything; answer it and ask before editing.
- Commit only when asked.  One branch per topic; the user merges
  (usually squash) to `master`.  Commit subject style: short,
  imperative, e.g. "Add MQTT logger for periodic value snapshots".
- Prefer the simplest, most transparent design.  The configuration
  decides how values behave; do not add safeguards, config checks,
  diagnostics or fallbacks that were not asked for.  If a review finds a
  problem, question the model before adding another rule.
- Implementation guides in `doc/` are the brief for implementation
  sessions.  Follow the current guide; when it conflicts with the code,
  say so instead of silently deviating.
- `uvrgw.conf` is the untracked production config with credentials.
  Never commit it.  Back it up (scratchpad) before editing it.

## Build

```
make CFLAGS="-I. -Wall -Wextra -Wno-unused-parameter -O2"
```

Must be warning free.  Do not run `make clean` or builds with other
flags in the user's tree; build test copies in the scratchpad
(`git archive HEAD | tar -x -C <dir>`, or copy the working files).

## Code conventions

- C, 2-space indent, Doxygen comments in the existing style, `syslog()`
  for all messages.
- Module pattern: `*_init()`, `*_configure(cfg)`,
  `*_register_disp_cbs()`, `*_unconfigure()`, `*_startup()`,
  `*_shutdown()`; config options in `uvrgw_conf.c`.  Values are linked
  by name through the dispatcher (`uvrgw_conf.h`).
- Validate the config at load and refuse invalid configs with a message
  naming the section/value.
- Runtime problems are logged once per state change, never per
  occurrence.
- Titled sections that must be unique need `CFGF_NO_TITLE_DUPES`
  (libconfuse silently merges duplicate titles otherwise).
- README: feature line, config section with example, architecture entry.

## Testing

Never write to production systems:
- No connections to the production MQTT broker (10.0.0.2).
- Devices of the user (Modbus TCP 10.0.3.74:1502/2502, Shelly HTTP) only
  with read-only requests (`mbpoll` reads, HTTP GET).  Never write
  registers.

Methods that work:
- `LD_PRELOAD` shim redirecting `syslog`/`__syslog_chk` to stderr; to
  capture MQTT output, the shim also replaces `mosquitto_publish()`
  (print topic and payload, return 0).
- Local broker: `/usr/sbin/mosquitto -p <port>`.  `mosquitto_sub` and
  python paho are not installed.
- Fake JSON source: `python3 -m http.server <port> --bind 127.0.0.1 -d
  <dir>`, the test rewrites files atomically (`os.replace`); deleting a
  file makes the source fail (HTTP 404).
- Drive everything from one Python script with `subprocess.Popen`; stop
  uvrgw with `send_signal(SIGINT)` and `wait(timeout=...)` to detect
  shutdown hangs.  Run once under `valgrind --leak-check=full`.
- Production config check: copy `uvrgw.conf`, prepend
  `state_dir = "<scratch dir>"` (no `/var/lib/uvrgw` here), run it with
  the shim.  CAN is not available on the dev machine, so a successful
  load ends with "Could not set CAN interface name 'can0'" before any
  MQTT connection.

Pitfalls:
- Do not background `cd x && prog &` chains in the shell, and do not use
  `pkill -f` (it matches the own shell); use the Python driver or
  `pgrep -a` first.
- Duplicate section titles in test configs are merged silently.
- The Shelly in `uvrgw.conf` has the placeholder `pwd = "secret"`, so
  REST tests against it fail.
- libmosquitto: `mosquitto_loop_start()` before
  `mosquitto_connect_async()` (otherwise no reconnect after a failed
  first connect); always `mosquitto_disconnect()` before
  `mosquitto_loop_stop()` (otherwise shutdown hangs); with QoS >= 1,
  publishing while disconnected returns `MOSQ_ERR_NO_CONN` but the
  message is queued and delivered after reconnect (no own retry queue).
