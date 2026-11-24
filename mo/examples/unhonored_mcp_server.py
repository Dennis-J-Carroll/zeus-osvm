#!/usr/bin/env python3
"""Real-failure MCP server: declares `report_export` and never honors it.

Glassport speaks JSON-RPC to this process over stdio. The initialize and
tools/list requests are answered; a `tools/call` for `report_export` deliberately
never gets a response. The process stays alive so the live tap remains open and
MO's synthetic tick, not end-of-stream, has to mint the BOLT.
"""
from __future__ import annotations

import json
import sys
import time


def _send(message: dict) -> None:
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


def main() -> int:
    for line in sys.stdin:
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(message, dict):
            continue

        method = message.get("method")
        msg_id = message.get("id")
        params = message.get("params") or {}

        if method == "initialize" and msg_id is not None:
            _send({
                "jsonrpc": "2.0",
                "id": msg_id,
                "result": {
                    "protocolVersion": params.get("protocolVersion", "2024-11-05"),
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "unhonored-demo", "version": "0.1"},
                },
            })
        elif method == "tools/list" and msg_id is not None:
            _send({
                "jsonrpc": "2.0",
                "id": msg_id,
                "result": {
                    "tools": [{
                        "name": "report_export",
                        "description": "declared, but this server never answers it",
                        "inputSchema": {"type": "object", "properties": {}},
                    }],
                },
            })
        elif method == "tools/call" and msg_id is not None:
            if params.get("name") == "report_export":
                # The crime scene: hold the request open forever. Because the
                # process stays alive, only the tick can expire the window.
                while True:
                    time.sleep(1)
            _send({
                "jsonrpc": "2.0",
                "id": msg_id,
                "result": {"content": [], "isError": False},
            })
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
