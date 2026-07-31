#!/usr/bin/env bash
# Turn a failed build log into GitHub check annotations.
#
# This exists because of decisions-m2.md §3: there is no working toolchain on
# the machine this node is written on, so CI is the only compiler, and the run
# log needs a token to download. Annotations do not — they are readable from the
# public API, which makes them the only way to find out why a build failed.
#
# Compiler diagnostics carry file/line/column and are anchored to the source.
# Everything else — link errors, ninja's own failures, an internal compiler
# error — has no position, so it is emitted unanchored rather than dropped. A
# silent failure report is worse than a noisy one: the previous version of this
# matched only `file:line:col: error:` and reported "0 source errors" on a build
# that had plainly failed.
set -uo pipefail

log="${1:?usage: annotate-build-failure.sh <build log>}"
emitted=0

emit () {
	# Annotations are single-line, and "::" would be read as a command separator.
	printf '%s\n' "$1" | tr -d '\r' | sed 's/::/ - /g'
}

while IFS= read -r line; do
	file=${line%%:*}
	rest=${line#*:}
	lineno=${rest%%:*}
	rest=${rest#*:}
	col=${rest%%:*}
	msg=${rest#*:}
	echo "::error file=$(emit "${file}"),line=${lineno},col=${col}::$(emit "${msg}")"
	emitted=$((emitted + 1))
done < <(grep -E '^[^ ]+:[0-9]+:[0-9]+: (error|fatal error):' "${log}" | sort -u | head -30)

while IFS= read -r line; do
	echo "::error::$(emit "${line}")"
	emitted=$((emitted + 1))
done < <(grep -E 'undefined reference|undefined symbol|ld returned|collect2:|internal compiler error|cannot find -l|No space left|virtual memory exhausted|Killed' "${log}" | sort -u | head -20)

# Nothing recognised: the tail of the log is still better than silence.
if [ "${emitted}" -eq 0 ]; then
	echo "::error::Build failed with no diagnostic this script recognises. Last lines of ${log}:"
	tail -n 25 "${log}" | while IFS= read -r line; do
		echo "::error::$(emit "${line}")"
	done
fi
