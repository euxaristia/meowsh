#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MEOWSH="$ROOT/meowsh"

if [[ ! -x "$MEOWSH" ]]; then
  echo "meowsh binary not found: $MEOWSH" >&2
  exit 1
fi

if ! command -v script >/dev/null 2>&1; then
  echo "Skipping interactive tests: 'script' command not found."
  exit 0
fi

if ! script -q -e -c "true" /dev/null >/dev/null 2>&1; then
  echo "Skipping interactive tests: pseudo-terminal allocation unavailable."
  exit 0
fi

tmpdir="$(mktemp -d)"
echo "Using tmpdir: $tmpdir"
# trap 'rm -rf "$tmpdir"' EXIT

strip_ansi() {
  sed -E $'s/\x1B\\[[0-9;?]*[ -/]*[@-~]//g'
}

run_tty_case() {
  local input="$1"
  local log="$2"
  shift 2
  (
    cd "$ROOT"
    printf "%b" "$input" | "$@" script -q -e -c "strace -f -o /tmp/meowsh_strace.log ./meowsh" "$log" >/dev/null
  )
}

echo "1) leading-space commands execute and shell exits"
log1="$tmpdir/case1.log"
run_tty_case "  echo HI_FROM_INTERACTIVE\nexit\n" "$log1" env -i MEOWSH_STARSHIP=0 PATH="$PATH" HOME="${HOME:-/tmp}" TERM="${TERM:-xterm-256color}"
strip_ansi <"$log1" >"$tmpdir/case1.stripped"
grep -F "HI_FROM_INTERACTIVE" "$tmpdir/case1.stripped" >/dev/null

echo "2) ENV is sourced by default in interactive mode"
rc="$tmpdir/rc.sh"
cat >"$rc" <<'EOF'
echo RC_LOADED_DEFAULT
EOF
log2="$tmpdir/case2.log"
run_tty_case "exit\n" "$log2" env -i MEOWSH_STARSHIP=0 PATH="$PATH" HOME="${HOME:-/tmp}" TERM="${TERM:-xterm-256color}" ENV="$rc"
strip_ansi <"$log2" >"$tmpdir/case2.stripped"
grep -F "RC_LOADED_DEFAULT" "$tmpdir/case2.stripped" >/dev/null

echo "3) empty PS1 from ENV falls back to a visible default prompt"
rc_empty_ps1="$tmpdir/rc-empty-ps1.sh"
cat >"$rc_empty_ps1" <<'EOF'
PS1=''
EOF
log3="$tmpdir/case3.log"
run_tty_case "exit\n" "$log3" env -i MEOWSH_STARSHIP=0 PATH="$PATH" HOME="${HOME:-/tmp}" TERM="${TERM:-xterm-256color}" ENV="$rc_empty_ps1"
strip_ansi <"$log3" >"$tmpdir/case3.stripped"
grep -F "🐱 " "$tmpdir/case3.stripped" >/dev/null

echo "Interactive regression tests passed."
