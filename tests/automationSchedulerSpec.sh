#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HEADER="$ROOT_DIR/src/Atanstack.h"
CPP="$ROOT_DIR/src/Atanstack.cpp"
PUMP_SKETCH_PATH="$ROOT_DIR/examples/ESP32C3_PumpControl/ESP32C3_PumpControl.ino"

# The library subscribes to the retained automation config topic.
rg -Fq 'buildAutomationTopic' "$HEADER"
rg -Fq 'control/automation' "$CPP"
rg -Fq '_mqtt.subscribe(automationTopic.c_str())' "$CPP"

# Automation rules are persisted to NVS (Preferences) so they survive reboots.
rg -Fq 'Preferences _automationPrefs' "$HEADER"
rg -Fq '_automationPrefs.begin("atan-auto"' "$CPP"
rg -Fq 'putString("cfg"' "$CPP"
rg -Fq 'getString("cfg"' "$CPP"

# Rules fire only against a synced clock, local timezone-aware.
rg -Fq 'configTzTime(_automationTz.c_str()' "$CPP"
rg -Fq 'automationClockSynced()' "$CPP"
rg -Fq 'runAutomationSchedule();' "$CPP"

# Fired executions are reported so the server can record history.
rg -Fq '"automation-fired"' "$CPP"

# Scheduler dedupe is stored per rule in NVS.
rg -Fq 'setAutomationLastFired' "$CPP"
rg -Fq 'putULong(key, minuteKey)' "$CPP"

# Timed runs persist a pending end epoch and fire the end action locally.
rg -Fq 'setAutomationEndEpoch' "$CPP"
rg -Fq 'automationEndDue' "$CPP"
rg -Fq 'publishAutomationFired(rule, rule.endOn, "end")' "$CPP"
rg -Fq 'publishAutomationFired(rule, rule.on, "start")' "$CPP"

# The pump example must keep driving the library loop so the local scheduler runs.
rg -Fq 'atanstack.loop();' "$PUMP_SKETCH_PATH"

# Host unit check of the pure minute-matching logic.
HOST_TEST="$ROOT_DIR/tests/automationScheduleHostTest.cpp"
BIN="$(mktemp)"
if command -v g++ >/dev/null 2>&1; then
  g++ -std=c++11 -Wall -Wextra -Werror -o "$BIN" "$HOST_TEST"
  "$BIN"
elif command -v clang++ >/dev/null 2>&1; then
  clang++ -std=c++11 -Wall -Wextra -Werror -o "$BIN" "$HOST_TEST"
  "$BIN"
else
  echo "no C++ compiler found; skipping host unit check" >&2
fi
rm -f "$BIN"

echo "automation scheduler spec passed"
