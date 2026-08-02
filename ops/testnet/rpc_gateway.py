#!/usr/bin/env python3
"""Public, deliberately narrow gateway for the Kei testnet RPC.

The node's control RPC stays on loopback.  This process is the only public
listener and forwards only the actions used by the SDK.  It also puts hard
bounds around the dev-network faucet; the node's deterministic faucet key must
never be reachable through an unbounded public request.
"""

from __future__ import annotations

import argparse
import collections
import http.server
import json
import os
import ssl
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Callable


ALLOWED_ACTIONS = frozenset(
    {
        "version",
        "account_info",
        "account_history",
        "block_info",
        "accounts_receivable",
        "process",
        "work_thresholds",
        "asset_info",
        "asset_by_symbol",
        "account_holdings",
        "asset_balance",
        "asset_holders",
        "commit_info",
        "claim_status",
        "faucet",
    }
)

MAX_BODY_BYTES = 256 * 1024
DEFAULT_FAUCET_MAX_RAW = 10_000 * 10**18
CONTROL_PREFIXES = ("wallet_", "password_", "account_create", "account_move", "send", "receive")


@dataclass(frozen=True)
class Limit:
    requests: int
    seconds: int


class SlidingWindow:
    """Small in-memory limiter. A restart forgets history, never ledger state."""

    def __init__(self, clock: Callable[[], float] = time.monotonic):
        self._clock = clock
        self._events: dict[str, collections.deque[float]] = {}
        self._lock = threading.Lock()

    def allow(self, key: str, limit: Limit) -> bool:
        now = self._clock()
        cutoff = now - limit.seconds
        with self._lock:
            events = self._events.setdefault(key, collections.deque())
            while events and events[0] <= cutoff:
                events.popleft()
            if len(events) >= limit.requests:
                return False
            events.append(now)
            return True


class GatewayState:
    def __init__(self, upstream: str, faucet_max_raw: int = DEFAULT_FAUCET_MAX_RAW):
        self.upstream = upstream
        self.faucet_max_raw = faucet_max_raw
        self.limiter = SlidingWindow()
        self.slots = threading.BoundedSemaphore(32)


class GatewayServer(http.server.ThreadingHTTPServer):
    """Accept TLS connections without serialising their handshakes.

    Wrapping the listening socket makes `accept()` perform each TLS handshake
    on the one accept loop. Cloudflare opens several origin connections in
    parallel, so one slow handshake can otherwise produce intermittent 525s.
    """

    daemon_threads = True

    def __init__(self, address: tuple[str, int], handler: type[GatewayHandler], context: ssl.SSLContext | None = None):
        self.tls_context = context
        super().__init__(address, handler)

    def get_request(self):  # type: ignore[no-untyped-def]
        request, address = self.socket.accept()
        if self.tls_context:
            request = self.tls_context.wrap_socket(
                request,
                server_side=True,
                do_handshake_on_connect=False,
            )
        return request, address


class GatewayHandler(http.server.BaseHTTPRequestHandler):
    server_version = "KeiTestnetGateway/1"
    # Cloudflare pools its origin connections and reuses them. Answering
    # HTTP/1.0 tells it nothing about that, so it kept sending a request into a
    # socket this end was closing: the body arrived truncated, the JSON parse
    # failed, and writing the 400 back raised SSLEOFError into an empty
    # connection. About one request in a hundred died that way, which the SDK
    # reports as `node-unreachable` — a real network that fails a hundredth of
    # the time is worse than one that is down, because nothing retries it.
    protocol_version = "HTTP/1.1"
    # Keep-alive costs a thread per idle connection, so reap them.
    timeout = 30
    state: GatewayState

    def log_message(self, fmt: str, *args: object) -> None:
        # Never log bodies: future SDK requests may carry material that should
        # not become an accidental credential log.
        super().log_message("%s %s", self.client_address[0], fmt % args)

    def do_OPTIONS(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if self.path != "/rpc":
            self._json(404, {"error": "Not found"})
            return
        self.send_response(204)
        self._cors()
        self.send_header("Access-Control-Max-Age", "86400")
        self._flush(b"")

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if self.path == "/healthz":
            self._json(200, {"status": "ok"})
        else:
            self._json(404, {"error": "Not found"})

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        if self.path != "/rpc":
            self._json(404, {"error": "Not found"})
            return

        length_text = self.headers.get("Content-Length", "")
        if not length_text.isdigit() or int(length_text) > MAX_BODY_BYTES:
            # The body is never read on this path, so whatever follows it on the
            # socket would be parsed as the next request. Close instead.
            self.close_connection = True
            self._json(413, {"error": "RPC request body is missing or too large"})
            return
        declared = int(length_text)
        request_body = self.rfile.read(declared)
        if len(request_body) != declared:
            # A short read means the peer went away mid-request. There is
            # nobody left to answer.
            self.close_connection = True
            return
        try:
            request = json.loads(request_body)
        except (UnicodeDecodeError, json.JSONDecodeError):
            self._json(400, {"error": "RPC request must be JSON"})
            return
        if not isinstance(request, dict) or not isinstance(request.get("action"), str):
            self._json(400, {"error": "RPC request needs an action"})
            return

        action = request["action"]
        if action not in ALLOWED_ACTIONS:
            # Preserve the node contract for a genuinely unknown command (HTTP
            # 200 with a JSON error), while making inherited control actions a
            # visible policy refusal. Neither kind is ever forwarded.
            control = action.startswith(CONTROL_PREFIXES)
            self._json(403 if control else 200, {"error": "That RPC action is not public"})
            return

        client = self.client_address[0]
        if not self.state.limiter.allow("global", Limit(3_000, 60)):
            self._json(429, {"error": "The public testnet is busy; retry in a minute"})
            return
        # Cloudflare origin connections identify an edge, not one end user.
        # Keep a per-edge ceiling high enough for several wallets polling at
        # once; the global, concurrency, body, action and faucet limits remain
        # the hard public bounds.
        if not self.state.limiter.allow("client:" + client, Limit(1_200, 60)):
            self._json(429, {"error": "Too many RPC requests; retry in a minute"})
            return
        if action == "faucet" and not self._allow_faucet(request, client):
            return

        if not self.state.slots.acquire(blocking=False):
            self._json(503, {"error": "The public testnet is busy; retry shortly"})
            return
        try:
            upstream = urllib.request.Request(
                self.state.upstream,
                data=request_body,
                method="POST",
                headers={"Content-Type": "application/json"},
            )
            try:
                with urllib.request.urlopen(upstream, timeout=15) as response:
                    body = response.read(MAX_BODY_BYTES + 1)
                    if len(body) > MAX_BODY_BYTES:
                        self._json(502, {"error": "The node response was too large"})
                        return
                    self.send_response(response.status)
                    self._cors()
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Cache-Control", "no-store")
                    self.send_header("Content-Length", str(len(body)))
                    self._flush(body)
            except (urllib.error.URLError, TimeoutError):
                self._json(502, {"error": "The testnet node is temporarily unavailable"})
        finally:
            self.state.slots.release()

    def _allow_faucet(self, request: dict[str, object], client: str) -> bool:
        account = request.get("account")
        if not isinstance(account, str) or not account.startswith("kei_") or len(account) > 70:
            self._json(400, {"error": "Faucet account must be a Kei address"})
            return False
        amount = request.get("amount")
        if amount is not None:
            if not isinstance(amount, str) or not amount.isdigit():
                self._json(400, {"error": "Faucet amount must be raw units as a decimal string"})
                return False
            if int(amount) <= 0 or int(amount) > self.state.faucet_max_raw:
                self._json(400, {"error": "Faucet request exceeds the 10,000 Kei testnet cap"})
                return False

        checks = (
            ("faucet-global", Limit(200, 3600)),
            ("faucet-client:" + client, Limit(20, 3600)),
            ("faucet-account:" + account, Limit(2, 86400)),
        )
        if not all(self.state.limiter.allow(key, limit) for key, limit in checks):
            self._json(429, {"error": "Faucet limit reached; retry later"})
            return False
        return True

    def _cors(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("X-Content-Type-Options", "nosniff")

    def _json(self, status: int, value: dict[str, str]) -> None:
        body = json.dumps(value, separators=(",", ":")).encode()
        self.send_response(status)
        self._cors()
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self._flush(body)

    def _flush(self, body: bytes) -> None:
        """Send headers and body, tolerating a peer that has already left.

        A client that hangs up mid-exchange is ordinary on a public endpoint,
        and a stack trace per occurrence buries the log this is operated from.
        """
        try:
            if self.close_connection:
                # Say so, rather than dropping a socket the client still counts
                # as usable — that is the failure this handler exists to avoid.
                self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        except OSError:  # ssl.SSLError included; it derives from OSError
            self.close_connection = True


def handler_for(state: GatewayState) -> type[GatewayHandler]:
    return type("ConfiguredGatewayHandler", (GatewayHandler,), {"state": state})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--listen", default=os.environ.get("KEI_GATEWAY_LISTEN", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("KEI_GATEWAY_PORT", "8080")))
    parser.add_argument("--tls-cert", default=os.environ.get("KEI_GATEWAY_TLS_CERT"))
    parser.add_argument("--tls-key", default=os.environ.get("KEI_GATEWAY_TLS_KEY"))
    parser.add_argument(
        "--upstream",
        # The node config needs an IPv4-mapped IPv6 bind address, but clients
        # can and should use ordinary loopback. It also avoids proxy-variable
        # edge cases around bracketed IPv6 URLs in urllib.
        default=os.environ.get("KEI_RPC_UPSTREAM", "http://127.0.0.1:45000"),
    )
    parser.add_argument(
        "--faucet-max-raw",
        type=int,
        default=int(os.environ.get("KEI_FAUCET_MAX_RAW", str(DEFAULT_FAUCET_MAX_RAW))),
    )
    args = parser.parse_args()
    if bool(args.tls_cert) != bool(args.tls_key):
        parser.error("--tls-cert and --tls-key must be provided together")
    state = GatewayState(args.upstream, args.faucet_max_raw)
    context = None
    if args.tls_cert and args.tls_key:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.tls_cert, args.tls_key)
    server = GatewayServer((args.listen, args.port), handler_for(state), context)
    server.serve_forever()


if __name__ == "__main__":
    main()
