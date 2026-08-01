#!/usr/bin/env bash
# Bring a bare Ubuntu 24.04 box up to a built, running kei-node.
#
# Runs as root, either from cloud-init on first boot (provision-build-box.py
# hands it over as user_data) or by hand on a box that already exists. It is
# the executable form of the claim in docs/decisions-m2.md §3 that the build
# box is disposable: everything here comes from two public repos, so destroying
# the box when a milestone is done costs one cold build to undo, and leaving it
# running costs money every hour.
#
# Deliberately not a general provisioner. It installs exactly what
# .github/workflows/build.yml installs, and configures the same
# banano_dev_network the workflow builds, so a claim checked here and a claim
# checked in CI are claims about the same node.
set -uo pipefail

log=/var/log/kei-setup.log
exec > >(tee -a "${log}") 2>&1

# cloud-init runs this with no HOME in the environment, and third-party install
# scripts dereference it unconditionally — bun's does, which under `set -u` took
# the whole run down *after* the node had finished building. Setting it is a
# smaller change than relaxing `set -u`, which is doing real work everywhere else.
export HOME="${HOME:-/root}"

node_repo="${KEI_NODE_REPO:-https://github.com/keicoin-org/kei-node.git}"
sdk_repo="${KEI_SDK_REPO:-https://github.com/keicoin-org/kei-transaction.git}"
branch="${KEI_BRANCH:-master}"
data="${KEI_DATA_PATH:-/root/keidata}"
rpc_port="${KEI_RPC_PORT:-45000}"

# A sentinel rather than a log grep, so "is it ready" is one cheap ssh and not
# a judgement call about how far down the log to read.
status=/root/kei-setup-status
echo "running" > "${status}"

say () {
	echo "kei-setup: $(date -u '+%H:%M:%S') $*"
}

fail () {
	say "FAILED: $*"
	echo "failed" > "${status}"
	exit 1
}

say "branch ${branch}, data path ${data}"

export DEBIAN_FRONTEND=noninteractive

# cloud-init and unattended-upgrades both hold the dpkg lock on a box this
# young, and losing that race is the single most common way this script dies
# thirty seconds in. Wait for it rather than failing.
for _ in $(seq 1 60); do
	fuser /var/lib/dpkg/lock-frontend > /dev/null 2>&1 || break
	sleep 5
done

apt-get update || fail "apt-get update"

# gcc-12 for the same reason build.yml pins it: vendored RocksDB 7.8.3 predates
# GCC 13 dropping the transitive <cstdint> include and will not compile on the
# distro default. jq and gdb are not in the CI list; they are here because this
# box is where things get debugged by hand.
apt-get install -y --no-install-recommends \
	build-essential cmake ninja-build ccache gcc-12 g++-12 gdb \
	git curl ca-certificates unzip jq python3 \
	|| fail "apt-get install"

# Boost and RocksDB dominate the build and never change, so the whole point of
# keeping a box alive between sessions is this cache. The 5G default is not
# enough to hold both and it silently evicts the expensive half. 8G is sized
# for the cx33's 80 GB disk, which also has to hold the recursive checkout and
# a RelWithDebInfo build tree; raise it on a box with more room.
ccache -M "${KEI_CCACHE_SIZE:-8G}" || fail "ccache -M"

# 8 GB of RAM is not much to link a RelWithDebInfo nano/banano tree in, and the
# box has no swap at all out of the box, so the first OOM kill lands in the
# middle of a twenty minute build with a message that reads like a compiler
# crash. Swap turns that into slowness, which is recoverable.
if [ "$(swapon --show --noheadings | wc -l)" -eq 0 ]; then
	say "no swap; adding 8G"
	fallocate -l 8G /swapfile || fail "fallocate swapfile"
	chmod 600 /swapfile
	mkswap /swapfile > /dev/null || fail "mkswap"
	swapon /swapfile || fail "swapon"
	grep -q '^/swapfile' /etc/fstab || echo '/swapfile none swap sw 0 0' >> /etc/fstab
fi

# ninja defaults to one job per core, which on a 4 core / 8 GB box means four
# concurrent gcc processes chewing through Boost and RocksDB translation units
# at roughly 2.5 GB each. That OOMs. Derive the job count from memory instead
# of cores and take whichever is smaller, so this stays right if the box is
# ever resized rather than being a magic number tied to one server type.
mem_gb=$(awk '/MemTotal/ { printf "%d", $2 / 1024 / 1024 }' /proc/meminfo)
cores=$(nproc)
jobs="${KEI_BUILD_JOBS:-$(( mem_gb * 2 / 5 ))}"
[ "${jobs}" -lt 1 ] && jobs=1
[ "${jobs}" -gt "${cores}" ] && jobs="${cores}"
say "${cores} cores, ${mem_gb} GB RAM -> building with -j ${jobs}"

if [ -d /root/kei-node/.git ]; then
	say "kei-node already cloned, fetching ${branch}"
	git -C /root/kei-node fetch --all --prune || fail "git fetch"
	git -C /root/kei-node checkout "${branch}" || fail "git checkout ${branch}"
	git -C /root/kei-node pull --ff-only || fail "git pull"
	git -C /root/kei-node submodule update --init --recursive || fail "submodule update"
else
	# Boost is vendored as the superproject at submodules/boost and CMakeLists
	# add_subdirectory()s each library, so the recursive clone is required and
	# is most of the wall clock here.
	say "cloning kei-node (recursive; this is a large checkout)"
	git clone --recursive --branch "${branch}" "${node_repo}" /root/kei-node \
		|| fail "git clone ${node_repo}"
fi

say "configuring"
cmake -B /root/kei-node/build -S /root/kei-node -G Ninja \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DACTIVE_NETWORK=banano_dev_network \
	-DNANO_GUI=OFF \
	-DNANO_TEST=ON \
	-DPORTABLE=1 \
	-DCMAKE_C_COMPILER=gcc-12 \
	-DCMAKE_CXX_COMPILER=g++-12 \
	-DCMAKE_C_COMPILER_LAUNCHER=ccache \
	-DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
	|| fail "cmake configure"

# The binary is `bananode`, not `keinode` — renaming it is a packaging change
# and is deliberately not bundled in here (build.yml says the same).
#
# A failure of this target is not necessarily a compile or link failure: CMake
# attaches a POST_BUILD step that runs the binary to write the sample configs,
# so a node that crashes on startup fails here under a printed link command.
# ci/annotate-startup-crash.sh tells the two apart and is worth running by hand
# on this box if that happens.
say "building bananode (cold build is ~40 minutes on a cx33; seconds once ccache is warm)"
cmake --build /root/kei-node/build --target bananode -- -k 0 -j "${jobs}" \
	|| fail "build bananode — try: bash /root/kei-node/ci/annotate-startup-crash.sh /root/kei-node/build/bananode"

/root/kei-node/build/bananode --version || fail "bananode --version"

# The SDK checkout is the other half of the point of this box: the conformance
# suite runs from here against the node above, under bun.
if [ ! -d /root/kei-transaction/.git ]; then
	say "cloning kei-transaction"
	git clone "${sdk_repo}" /root/kei-transaction || fail "git clone ${sdk_repo}"
fi

if [ ! -x /root/.bun/bin/bun ]; then
	say "installing bun"
	curl -fsSL https://bun.sh/install | bash || fail "bun install"
fi
export BUN_INSTALL=/root/.bun
export PATH="${BUN_INSTALL}/bin:${PATH}"
grep -q 'BUN_INSTALL' /root/.bashrc || echo 'export PATH="/root/.bun/bin:${PATH}"' >> /root/.bashrc
(cd /root/kei-transaction && bun install) || fail "bun install in kei-transaction"

mkdir -p "${data}" || fail "mkdir ${data}"

# Two files, because the node reads two. config-node.toml's [rpc] enable is
# what makes the daemon start an RPC at all (nano/nano_node/daemon.cpp:150);
# with child_process left at its default the handler runs in-process, which is
# what we want — one process to start, stop and attach gdb to.
cat > "${data}/config-node.toml" <<'TOML'
[rpc]
enable = true
TOML

# `address` is parsed as an IPv6 address (nano/lib/rpcconfig.cpp:77), so a
# plain "127.0.0.1" is rejected outright and the IPv4-mapped form is required.
#
# Loopback is not a default worth changing here: rpc.cpp:37 refuses to start a
# non-loopback listener with enable_control set, and it is right to — control
# RPCs include wallet access, so binding this to a public address hands the
# box's funds to the internet.
cat > "${data}/config-rpc.toml" <<TOML
address = "::ffff:127.0.0.1"
port = ${rpc_port}
enable_control = true
TOML

# systemd rather than a backgrounded binary, for one specific reason: stopping
# the node by hand invites `pkill -f bananode`, whose pattern matches the ssh
# command running it and kills the session instead. `systemctl restart
# kei-node` has no such edge, and journalctl beats a stray nohup.out.
cat > /etc/systemd/system/kei-node.service <<UNIT
[Unit]
Description=kei-node (bananode, banano_dev_network)
After=network-online.target

[Service]
ExecStart=/root/kei-node/build/bananode --daemon --data_path ${data}
Restart=on-failure
RestartSec=5
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload || fail "systemctl daemon-reload"
systemctl enable --now kei-node || fail "systemctl enable kei-node"

# "It built" is a weaker claim than "it answers", and the difference is the
# whole reason this box exists rather than just CI. Ask the running node.
say "waiting for RPC on ${rpc_port}"
for _ in $(seq 1 60); do
	version=$(curl -fsS -m 5 -d '{"action":"version"}' "http://[::ffff:127.0.0.1]:${rpc_port}" 2>/dev/null)
	if [ -n "${version}" ]; then
		say "RPC up: ${version}"
		echo "ready" > "${status}"
		say "done"
		exit 0
	fi
	sleep 5
done

# Reaching here means a built node that will not answer, which is a different
# and more interesting failure than a broken build — so say where to look
# rather than just exiting non-zero.
say "node built but RPC never answered on ${rpc_port}"
say "check: systemctl status kei-node; journalctl -u kei-node -n 100"
say "config the node actually parsed is ${data}/config-rpc.toml; a sample with"
say "every key and its documentation is written to /root/kei-node/build/"
fail "RPC did not come up"
