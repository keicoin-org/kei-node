#!/usr/bin/env bash
# Turn a crash of the freshly built node into GitHub check annotations.
#
# The `bananode` ninja edge does not stop at the link. CMake attaches a
# POST_BUILD step that runs the binary to write the sample configs
# (nano/nano_node/CMakeLists.txt:24), so the target also fails when the node
# dies on startup — with a linker command line printed above it, which reads
# like a link failure and is not one. What ninja reports in that case is
# `FAILED: [code=139] bananode` and "Segmentation fault", and neither says
# where.
#
# The executable is sitting there, built and runnable, so the backtrace costs
# one gdb invocation. Same reasoning as annotate-build-failure.sh: annotations
# are readable without a token, the run log is not (decisions-m2.md §3).
set -uo pipefail

node="${1:-build/bananode}"
emitted=0
limit=40

emit () {
	printf '%s' "$1" | tr -d '\r' | sed 's/::/ - /g'
}

say () {
	if [ "${emitted}" -lt "${limit}" ]; then
		echo "::error::$(emit "$1")"
		emitted=$((emitted + 1))
	fi
}

# Nothing to say if the build never got as far as producing a binary — that is
# an ordinary compile or link failure and annotate-build-failure.sh covers it.
if [ ! -x "${node}" ]; then
	exit 0
fi

if ! command -v gdb > /dev/null 2>&1; then
	say "${node} exists, so the link succeeded and the failure is the node crashing on startup. gdb is not installed, so there is no backtrace."
	exit 0
fi

# The same command the POST_BUILD step runs. Its stdout is the generated config
# and would otherwise bury the backtrace, so it goes to /dev/null; gdb's own
# output stays on stderr and stdout of gdb itself.
crash=crash-backtrace.log
gdb -batch -nx \
	-ex 'set confirm off' \
	-ex 'set pagination off' \
	-ex 'run --generate_config node > /dev/null' \
	-ex 'echo \n--- backtrace ---\n' \
	-ex 'bt 40' \
	-ex 'echo \n--- all threads ---\n' \
	-ex 'thread apply all bt 12' \
	"${node}" > "${crash}" 2>&1

# A normal exit here means the crash is not reproducible under gdb, which is
# worth saying plainly rather than emitting an empty annotation.
if grep -qE 'exited (normally|with code 0)' "${crash}"; then
	say "${node} ran to completion under gdb, so the startup crash did not reproduce. The POST_BUILD failure is something else."
	exit 0
fi

say "--- ${node} crashed on startup; gdb backtrace follows ---"

while IFS= read -r line; do
	[ "${emitted}" -ge "${limit}" ] && break
	say "${line}"
done < <(grep -vE '^\[|^$|^Reading symbols|^\(gdb\)|warning: |^Using host|^For help|^Type ' "${crash}" | head -45)

exit 0
