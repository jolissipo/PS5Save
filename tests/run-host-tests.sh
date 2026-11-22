#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
trap 'exit 1' HUP INT TERM
ulimit -c 0

set --
if ${CC:-cc} -Werror -Wno-format-truncation -x c -fsyntax-only /dev/null 2>/dev/null; then
  set -- -Wno-format-truncation
fi

# The host struct stat uses POSIX field names for these timestamps.
${CC:-cc} ${CFLAGS:-} -std=c11 -D_DEFAULT_SOURCE -UNDEBUG -Wall -Wextra -Werror -pthread "$@" \
  -Dst_mtimespec=st_mtim -Dst_ctimespec=st_ctim -DPROGRESS_INTERVAL_SECONDS=1 \
  -DTEST_ROOT="\"$test_dir/fixture\"" -Itests/include \
  tests/notifications.c -o "$test_dir/notifications"

for scenario in legacy settings errors off gameplay protection empty read-error snapshot progress language recovery concurrent; do
  rm -rf "$test_dir/fixture"
  if "$test_dir/notifications" "$scenario" >"$test_dir/log" 2>&1; then
    printf 'PASS %s\n' "$scenario"
  else
    cat "$test_dir/log" >&2
    printf 'FAIL %s\n' "$scenario" >&2
    exit 1
  fi
done

# Exercise the embedded HTML and HTTP router without PS5 hardware.
${CC:-cc} ${CFLAGS:-} -std=c11 -D_DEFAULT_SOURCE -UNDEBUG -Wall -Wextra -Werror -pthread \
  -DTEST_ROOT="\"$test_dir/dashboard-fixture\"" \
  tests/dashboard.c -o "$test_dir/dashboard"
"$test_dir/dashboard"
