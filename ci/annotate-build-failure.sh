#!/usr/bin/env bash
# Turn a failed build log into GitHub check annotations.
#
# This exists because of decisions-m2.md §3: there is no working toolchain on
# the machine this node is written on, so CI is the only compiler, and the run
# log needs a token to download. Annotations do not — they are readable from the
# public API, which makes them the only way to find out why a build failed.
#
# The build runs with `-k 0` so that one run reports every error in the tree.
# That means the failure is almost never at the end of the log: ninja carries on
# and the successful edges that follow scroll it away. So the anchor is ninja's
# own "FAILED:" marker and the lines under it, not the tail.
set -uo pipefail

log="${1:?usage: annotate-build-failure.sh <build log>}"
emitted=0
limit=45

emit () {
	# Annotations are single-line, and "::" would be read as a command separator.
	printf '%s' "$1" | tr -d '\r' | sed 's/::/ - /g'
}

say () {
	if [ "${emitted}" -lt "${limit}" ]; then
		echo "::error::$(emit "$1")"
		emitted=$((emitted + 1))
	fi
}

# Compiler diagnostics carry a file, line and column, so they can be anchored to
# the source and show up on the pull request beside the code that caused them.
while IFS= read -r line; do
	[ "${emitted}" -ge "${limit}" ] && break
	file=${line%%:*}
	rest=${line#*:}
	lineno=${rest%%:*}
	rest=${rest#*:}
	col=${rest%%:*}
	msg=${rest#*:}
	echo "::error file=$(emit "${file}"),line=${lineno},col=${col}::$(emit "${msg}")"
	emitted=$((emitted + 1))
done < <(grep -E '^[^ ]+:[0-9]+:[0-9]+: (error|fatal error):' "${log}" | sort -u | head -25)

# Everything else has no position: link errors, ninja's own failures, an
# internal compiler error, a runner out of disk. Emit the "FAILED:" marker with
# the lines under it, which is where the actual diagnostic lives.
if grep -qE '^FAILED:' "${log}"; then
	say "--- ninja reported these edges as FAILED ---"
	while IFS= read -r line; do
		[ "${emitted}" -ge "${limit}" ] && break
		say "${line}"
	done < <(grep -A 12 -E '^FAILED:' "${log}" | grep -vE '^\[[0-9]+/[0-9]+\]' | head -30)
fi

while IFS= read -r line; do
	[ "${emitted}" -ge "${limit}" ] && break
	say "${line}"
done < <(grep -E 'undefined reference|undefined symbol|ld returned|collect2:|internal compiler error|cannot find -l|No space left|virtual memory exhausted|Killed|ninja: build stopped' "${log}" | sort -u | head -15)

# Nothing recognised: the tail is still better than silence, even though under
# -k 0 it is usually just the edges that succeeded after the failure.
if [ "${emitted}" -eq 0 ]; then
	say "Build failed with no diagnostic this script recognises. Last lines of ${log}:"
	while IFS= read -r line; do
		say "${line}"
	done < <(tail -n 25 "${log}")
fi
