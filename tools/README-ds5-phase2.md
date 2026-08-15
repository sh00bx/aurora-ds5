# DS5 latency — Phase 2 measurement kit

Phase 2 of `workspace/ds5-latency-deep-dive-2026-08-15.md` is "measure without
deploying". It costs almost no code, but it did need one instrument that did not
exist: the TV has **no `btmon`, no `hcidump`, no `hcitool`**, so items 2.3 / 2.4 /
2.5 — all of which are phrased as "read it off the monitor" — had nothing to read
it with. `ds5_sniff` is that instrument.

| File | Runs on | Purpose |
|---|---|---|
| `ds5_sniff.c` | TV (ARM) | passive HCI-monitor probe — the missing instrument |
| `ds5_phase2_run.sh` | TV | one labelled capture slice (sniff + daemon-ledger delta + manifest) |
| `ds5_phase2_eth.sh` | TV | the 2.1 eth0 ladder, with a restore watchdog |
| `ds5_phase2_report.py` | workstation | captures → verdicts for 2.3/2.4/2.5/2.6 |
| `ds5_click_latency.py` | workstation | Phase 1's microphone rig (pad-depth ground truth) |

## Why a separate process and not more daemon code

`ds5_txd` must never be restarted mid-session, so every daemon change costs a
deploy window and can only be observed in the *next* session. A separate process
attaches and detaches at will — including in the middle of a running game, which
is exactly when the interesting episodes happen.

It also removes a conflict of interest: Phase 1 moved the gap histogram into the
daemon's own capture thread, and the acceptance for that asks for agreement with
"monitor ground truth to ±2 ms". The daemon cannot be its own witness. If the
capture thread is ever starved of CPU it will *under*-count gaps, and only an
independent reader can show that.

`ds5_sniff` binds `HCI_CHANNEL_MONITOR`, a receive-only broadcast channel. There
is no code path in it that can emit an HCI command, so it cannot spend link
budget or perturb what it measures. Multiple monitor readers coexist; the
daemon's own `ds5-cap` keeps running untouched. It runs at `nice 19`.

## Build & install

```sh
# ARM, static, no libc on the TV to match
~/x-tools/armv5-eabi--musl--stable-2025.08-1/bin/arm-linux-gcc -O2 -static \
    -Wall -Wextra tools/ds5_sniff.c -o /tmp/ds5_sniff

# rm-first, then verify by md5 — never trust the copy (ETXTBSY / stale inode)
ssh LG 'rm -f /tmp/ds5_sniff'
scp /tmp/ds5_sniff        LG:/tmp/ds5_sniff
scp tools/ds5_phase2_run.sh tools/ds5_phase2_eth.sh LG:/tmp/
ssh LG 'chmod +x /tmp/ds5_sniff /tmp/ds5_phase2_*.sh; md5sum /tmp/ds5_sniff'
```

`/tmp` is wiped on reboot — that is deliberate. Nothing here should survive into
a session nobody is watching.

## Taking a slice

```sh
# on the TV, as root. Start it BEFORE switching the pad on (see "role" below).
nohup /tmp/ds5_phase2_run.sh base 1800 >/dev/null 2>&1 &     # 30 min baseline
nohup /tmp/ds5_phase2_run.sh maxq8 1800 8 >/dev/null 2>&1 &  # 2.6 ladder rung
```

Each run lands in `/tmp/phase2/<label>/` as `sniff.log`, `txd.log` (the daemon
ledger delta over the *same* window) and `manifest.txt` (machine state at both
ends). Pull the directory and report on it:

```sh
scp -r LG:/tmp/phase2/base ./
python3 tools/ds5_phase2_report.py base
python3 tools/ds5_phase2_report.py maxq12 maxq8 maxq6 maxq4   # 2.6 ladder table
```

## What each record answers

* `GAP` — one line per NOCP gap ≥30 ms that closed, **with the traffic breakdown
  for that gap's own duration**. This is the 2.3/2.5 join: what was on the air
  while our credits starved, without post-hoc log alignment. `out=` is the
  outstanding count before the credit landed — a gap only counts as starvation if
  we were in fact waiting, the same gate the daemon applies.
* `EV` — conn/disconn, role, sniff mode, `max_slots` (re-prices T5),
  `flush_occurred` (the witness L1 will need in Phase 3).
* `SEC` — one aggregate line per second; the activity filter and all baselines
  are computed from these.

## Reading rules that are not optional

* **Rates, never counts.** Slices differ in length.
* **Activity-filtered.** Idle seconds have no audio to starve. Averaging them in
  is exactly the error that produced the bogus "−73 %" RT-boost result; the
  report only counts seconds with audio actually flowing.
* **Judge the ≥80 ms band.** The 30–49 band runs ~300/min and is benign; it will
  drown any tail signal in a pooled ratio. The report judges the tail whenever
  there are ≥8 tail events and says so explicitly when it had to fall back.
* **Alternate rungs across days.** Coex weather drifts. A rung measured on one
  evening is a measurement of that evening.
* **One substitute at a time** (maxq / auto-flush / Q_TARGET). The telemetry
  cannot attribute a delta to two changes.

## Role attribution needs an early start

2.4 is answered from the `Accept_Connection_Request` command and `Role_Change`
events — both are passive, but both only happen *at connect time*. A handle that
was already up when the sniffer attached has no connection event to learn from;
the sniffer marks it `[learned]` and reports `role=?` rather than guessing.

**So: start the capture before switching the pad on.** One session started in the
right order answers 2.4 permanently.

Mid-session attach is still fully valid for 2.3/2.5/2.6 — a handle is promoted to
BR/EDR by an ACL payload no LE link can carry (>100 B; LE tops out at 251 even
with DLE, DS5 audio frames are 398/547). Handles that stay unproven are kept
**out** of the gap ledger: an LE handle counted as a starving DS5 link would
manufacture exactly the gaps this tool exists to attribute.

## 2.1 (eth0 ladder) — the one item that needs hardware

`eth0` is present but has **no carrier**; the whole 65 GB/session stream runs over
`wlan0`, and WiFi shares the MT7921 combo chip with BT. The cable is the largest
environmental lever available and costs zero latency.

```sh
/tmp/ds5_phase2_eth.sh precheck      # changes nothing; measures real throughput
/tmp/ds5_phase2_eth.sh b             # eth0 carries the stream, wlan0 idle
/tmp/ds5_phase2_eth.sh c 30          # wlan0 DOWN for 30 min, auto-restored
/tmp/ds5_phase2_eth.sh restore
```

Rung c takes `wlan0` down, so it arms an unconditional background restore first:
a dropped SSH session or a killed script still leaves the TV reachable. It
refuses outright if `eth0` has no carrier.

Run the precheck **after** plugging in and **before** trusting rung b: webOS has
to actually move the stream to the wire, and a 100BASE-T link carrying >60 % of
its capacity adds queueing delay of its own — that would measure the cable, not
the coex.
