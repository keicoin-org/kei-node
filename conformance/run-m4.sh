#!/usr/bin/env bash
set -euo pipefail

sdk_dir="${KEI_SDK_DIR:?Set KEI_SDK_DIR to a kei-transaction checkout.}"
node_url="${KEI_NODE_URL:-http://127.0.0.1:45000}"

cd "${sdk_dir}"
export KEI_NODE_URL="${node_url}"
exec bun test \
	packages/core/test/m4-node.test.ts \
	packages/kei/test/m4-over-http.test.ts
