from __future__ import annotations

import http.client
import importlib.util
import json
import pathlib
import sys
import threading
import unittest
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


MODULE_PATH = pathlib.Path(__file__).with_name("rpc_gateway.py")
SPEC = importlib.util.spec_from_file_location("rpc_gateway", MODULE_PATH)
assert SPEC and SPEC.loader
gateway = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = gateway
SPEC.loader.exec_module(gateway)


class Upstream(BaseHTTPRequestHandler):
    calls: list[dict[str, object]] = []

    def log_message(self, _format: str, *_args: object) -> None:
        pass

    def do_POST(self) -> None:  # noqa: N802
        body = self.rfile.read(int(self.headers["Content-Length"]))
        request = json.loads(body)
        self.calls.append(request)
        response = json.dumps({"action": request["action"]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)


class GatewayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.upstream = ThreadingHTTPServer(("127.0.0.1", 0), Upstream)
        upstream_url = f"http://127.0.0.1:{cls.upstream.server_port}"
        cls.gateway = gateway.GatewayServer(
            ("127.0.0.1", 0), gateway.handler_for(gateway.GatewayState(upstream_url))
        )
        cls.base = f"http://127.0.0.1:{cls.gateway.server_port}"
        for server in (cls.upstream, cls.gateway):
            threading.Thread(target=server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.gateway.shutdown()
        cls.upstream.shutdown()

    def post(self, value: dict[str, object]) -> tuple[int, dict[str, object], dict[str, str]]:
        request = urllib.request.Request(
            self.base + "/rpc",
            data=json.dumps(value).encode(),
            method="POST",
            headers={"Content-Type": "application/json"},
        )
        try:
            response = urllib.request.urlopen(request)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, json.loads(response.read()), dict(response.headers.items())

    def test_allows_sdk_rpc_and_sets_browser_cors(self) -> None:
        status, body, headers = self.post({"action": "account_info", "account": "kei_1"})
        self.assertEqual(status, 200)
        self.assertEqual(body, {"action": "account_info"})
        self.assertEqual(headers["Access-Control-Allow-Origin"], "*")

    def test_refuses_inherited_control_actions(self) -> None:
        before = len(Upstream.calls)
        status, body, _ = self.post({"action": "wallet_create"})
        self.assertEqual(status, 403)
        self.assertIn("not public", str(body["error"]))
        self.assertEqual(len(Upstream.calls), before)

    def test_unknown_action_keeps_the_node_contract_without_forwarding(self) -> None:
        before = len(Upstream.calls)
        status, body, _ = self.post({"action": "definitely_not_an_action"})
        self.assertEqual(status, 200)
        self.assertIn("not public", str(body["error"]))
        self.assertEqual(len(Upstream.calls), before)

    def test_caps_the_public_faucet_before_forwarding(self) -> None:
        status, body, _ = self.post(
            {"action": "faucet", "account": "kei_1", "amount": str(10_001 * 10**18)}
        )
        self.assertEqual(status, 400)
        self.assertIn("10,000 Kei", str(body["error"]))

    def test_keeps_one_connection_open_across_requests(self) -> None:
        # Cloudflare pools origin connections. An HTTP/1.0 answer makes it race
        # the server's close, which truncates about one request in a hundred.
        connection = http.client.HTTPConnection("127.0.0.1", self.gateway.server_port, timeout=5)
        try:
            for _ in range(3):
                connection.request(
                    "POST",
                    "/rpc",
                    body=json.dumps({"action": "account_info", "account": "kei_1"}),
                    headers={"Content-Type": "application/json"},
                )
                response = connection.getresponse()
                self.assertEqual(response.version, 11)
                self.assertEqual(json.loads(response.read()), {"action": "account_info"})
                self.assertFalse(response.will_close)
        finally:
            connection.close()

    def test_an_oversized_body_closes_rather_than_desyncing(self) -> None:
        # The body is never read on that path, so a kept-alive connection would
        # parse its remainder as the next request.
        connection = http.client.HTTPConnection("127.0.0.1", self.gateway.server_port, timeout=5)
        try:
            connection.request(
                "POST",
                "/rpc",
                body=b"x" * (gateway.MAX_BODY_BYTES + 1),
                headers={"Content-Type": "application/json"},
            )
            response = connection.getresponse()
            self.assertEqual(response.status, 413)
            self.assertTrue(response.will_close)
        finally:
            connection.close()

    def test_limits_each_faucet_account(self) -> None:
        account = "kei_test_limit_account"
        self.assertEqual(self.post({"action": "faucet", "account": account})[0], 200)
        self.assertEqual(self.post({"action": "faucet", "account": account})[0], 200)
        status, body, _ = self.post({"action": "faucet", "account": account})
        self.assertEqual(status, 429)
        self.assertIn("limit reached", str(body["error"]))


if __name__ == "__main__":
    unittest.main()
