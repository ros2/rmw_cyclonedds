#!/usr/bin/env bash
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"

BUG_REF="${BUG_REF:-rolling}"
FIX_REF="${FIX_REF:-cdr-header-truncation-fix}"
WS_ROOT="${WS_ROOT:-${TMPDIR:-/tmp}/rmw585}"

if ! command -v ros2 >/dev/null 2>&1 && [ -f "/opt/ros/${ROS_DISTRO:-}/setup.bash" ]; then
  set +u; source "/opt/ros/${ROS_DISTRO}/setup.bash"; set -u
fi
command -v ros2      >/dev/null || { echo "ERROR: source /opt/ros/<distro>/setup.bash first."; exit 2; }
command -v colcon    >/dev/null || { echo "ERROR: colcon not found (apt install python3-colcon-common-extensions)."; exit 2; }
command -v iox-roudi >/dev/null || { echo "ERROR: iox-roudi not found (apt install ros-<distro>-rmw-cyclonedds-cpp)."; exit 2; }

WORKTREES=()
cleanup() {
  for wt in "${WORKTREES[@]:-}"; do
    [ -n "$wt" ] && git -C "$REPO" worktree remove --force "$wt" 2>/dev/null
  done
}
trap cleanup EXIT INT TERM

build_and_run() {
  local label="$1" ref="$2"
  local ws="$WS_ROOT/$label" wt="$WS_ROOT/$label/_src"
  echo "=================================================================="
  echo "### [$label] build rmw_cyclonedds_cpp @ $ref, then reproduce ###"
  echo "=================================================================="
  rm -rf "$ws"; mkdir -p "$ws"
  git -C "$REPO" worktree remove --force "$wt" 2>/dev/null || true
  git -C "$REPO" worktree add --detach "$wt" "$ref" >/dev/null || return 3
  WORKTREES+=("$wt")
  echo "[$label] commit: $(git -C "$wt" rev-parse --short HEAD)"
  echo "[$label] building (log: $ws/build.log)..."
  ( cd "$ws" && colcon build --base-paths "$wt" \
      --packages-select rmw_cyclonedds_cpp \
      --cmake-args -DCMAKE_BUILD_TYPE=Release ) >"$ws/build.log" 2>&1 \
    || { echo "[$label] BUILD FAILED:"; tail -20 "$ws/build.log"; return 3; }
  echo "[$label] built OK; running reproducer..."
  ( set +u; source "$ws/install/setup.bash"; set -u
    "$SCRIPT_DIR/reproduce.sh" )
}

build_and_run "without-fix" "$BUG_REF"; bug_rc=$?
pkill -f iox-roudi 2>/dev/null; sleep 2
build_and_run "with-fix" "$FIX_REF"; fix_rc=$?

echo
echo "########################## SUMMARY ##########################"
echo "  without fix ($BUG_REF): reproduce.sh exit $bug_rc   (1 = bug reproduced)"
echo "  with fix    ($FIX_REF): reproduce.sh exit $fix_rc   (0 = delivered OK)"
if [ "$bug_rc" -eq 1 ] && [ "$fix_rc" -eq 0 ]; then
  echo "  VERDICT: bug reproduced without the fix, resolved with it."
  exit 0
fi
echo "  VERDICT: unexpected — inspect the per-run output above."
exit 1
