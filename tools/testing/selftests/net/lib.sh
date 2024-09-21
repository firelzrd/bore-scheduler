#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

##############################################################################
# Defines

WAIT_TIMEOUT=${WAIT_TIMEOUT:=20}
BUSYWAIT_TIMEOUT=$((WAIT_TIMEOUT * 1000)) # ms

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4
# namespace list created by setup_ns
NS_LIST=()

##############################################################################
# Helpers
busywait()
{
	local timeout=$1; shift

	local start_time="$(date -u +%s%3N)"
	while true
	do
		local out
		if out=$("$@"); then
			echo -n "$out"
			return 0
		fi

		local current_time="$(date -u +%s%3N)"
		if ((current_time - start_time > timeout)); then
			echo -n "$out"
			return 1
		fi
	done
}

cleanup_ns()
{
	local ns=""
	local ret=0

	for ns in "$@"; do
		[ -z "${ns}" ] && continue
		ip netns pids "${ns}" 2> /dev/null | xargs -r kill || true
		ip netns delete "${ns}" &> /dev/null || true
		if ! busywait $BUSYWAIT_TIMEOUT ip netns list \| grep -vq "^$ns$" &> /dev/null; then
			echo "Warn: Failed to remove namespace $ns"
			ret=1
		fi
	done

	return $ret
}

cleanup_all_ns()
{
	cleanup_ns "${NS_LIST[@]}"
}

# setup netns with given names as prefix. e.g
# setup_ns local remote
setup_ns()
{
	local ns=""
	local ns_name=""
	local ns_list=()
	local ns_exist=
	for ns_name in "$@"; do
		# Some test may setup/remove same netns multi times
		if unset ${ns_name} 2> /dev/null; then
			ns="${ns_name,,}-$(mktemp -u XXXXXX)"
			eval readonly ${ns_name}="$ns"
			ns_exist=false
		else
			eval ns='$'${ns_name}
			cleanup_ns "$ns"
			ns_exist=true
		fi

		if ! ip netns add "$ns"; then
			echo "Failed to create namespace $ns_name"
			cleanup_ns "${ns_list[@]}"
			return $ksft_skip
		fi
		ip -n "$ns" link set lo up
		! $ns_exist && ns_list+=("$ns")
	done
	NS_LIST+=("${ns_list[@]}")
}
