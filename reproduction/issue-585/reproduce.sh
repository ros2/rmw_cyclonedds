#!/usr/bin/env bash
set -u

TOPIC="chatter_repro_585"
RUN_SECS="${RUN_SECS:-8}"
WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/rmw585.XXXXXX")"
CFG="$WORKDIR/cyclonedds.xml"

cat > "$CFG" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<CycloneDDS xmlns="https://cdds.io/config">
    <Domain id="any">
        <General>
            <Interfaces>
                <PubSubMessageExchange type="iox" library="psmx_iox" priority="1000000"
                  config="LOG_LEVEL=INFO;SERVICE_NAME=repro585;KEYED_TOPICS=true"/>
            </Interfaces>
        </General>
    </Domain>
</CycloneDDS>
XML

export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
export CYCLONEDDS_URI="file://$CFG"

PIDS=()
cleanup() {
  trap - EXIT INT TERM
  for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
  sleep 1
  for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill -9 "$p" 2>/dev/null; done
  rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

command -v ros2      >/dev/null || { echo "ERROR: 'ros2' not found. Source your ROS 2 setup.bash first."; exit 2; }
command -v iox-roudi >/dev/null || { echo "ERROR: 'iox-roudi' not found."; exit 2; }

ROUDI_LOG="$WORKDIR/roudi.log"
iox-roudi >"$ROUDI_LOG" 2>&1 &
PIDS+=("$!")
for _ in $(seq 1 50); do
  grep -q "RouDi is ready for clients" "$ROUDI_LOG" && break
  sleep 0.2
done
grep -q "RouDi is ready for clients" "$ROUDI_LOG" \
  || { echo "RouDi did not start:"; sed 's/^/  /' "$ROUDI_LOG"; exit 2; }

LIS_LOG="$WORKDIR/listener.log"
ros2 run demo_nodes_cpp listener --ros-args -r chatter:="$TOPIC" >"$LIS_LOG" 2>&1 &
PIDS+=("$!")

TALK_LOG="$WORKDIR/talker.log"
ros2 run demo_nodes_cpp talker --ros-args -r chatter:="$TOPIC" >"$TALK_LOG" 2>&1 &
PIDS+=("$!")

sleep "$RUN_SECS"

published=$(grep -c "Publishing:" "$TALK_LOG" 2>/dev/null); published=${published:-0}
received=$(grep -c "I heard"      "$LIS_LOG"  2>/dev/null); received=${received:-0}

echo "messages published by talker : $published"
echo "messages received by listener: $received"

if [ "$published" -gt 0 ] && [ "$received" -eq 0 ]; then
  echo "RESULT: BUG REPRODUCED — variable-size samples are NOT delivered over PSMX."
  exit 1
elif [ "$received" -gt 0 ]; then
  echo "RESULT: OK — listener received $received samples over PSMX."
  exit 0
else
  echo "RESULT: INCONCLUSIVE — talker published nothing."
  exit 2
fi
