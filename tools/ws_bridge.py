#!/usr/bin/env python3
"""
Dev bridge: WebSocket pass-through + static server per la console.

  HTTP  :8080 /                -> console.html
  WS    :8080 /gateway         -> l'ESP32 gateway si connette qui
  WS    :8080 /client          -> la console.html (o altri tool) qui

Messaggi inoltrati transparenti in entrambe le direzioni.

Installa:  pip install websockets
Avvia:     python ws_bridge.py [--port 8080] [--console ../webui/console.html]

Apri poi http://localhost:8080/ nel browser.
"""
import argparse
import asyncio
import json
import logging
import os
from pathlib import Path

import websockets
from websockets.exceptions import ConnectionClosed

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
log = logging.getLogger("bridge")

gateway_ws = None
client_ws  = None
console_html_path = None


async def handler(ws, path):
    global gateway_ws, client_ws

    if path == "/gateway":
        if gateway_ws is not None:
            log.warning("gateway duplicato, rifiuto")
            await ws.close(code=1008, reason="already connected")
            return
        gateway_ws = ws
        log.info("gateway connesso")
        role = "gateway"
    elif path == "/client":
        if client_ws is not None:
            log.info("client sostituito")
            try: await client_ws.close()
            except: pass
        client_ws = ws
        log.info("client connesso")
        role = "client"
    else:
        await ws.close(code=1008, reason="unknown path")
        return

    try:
        async for msg in ws:
            peer = client_ws if role == "gateway" else gateway_ws
            if peer is None:
                log.debug("nessun peer, messaggio droppato")
                continue
            try:
                parsed = json.loads(msg)
                t = parsed.get("type", "?")
                log.info(f"{role:>7} → {'client' if role == 'gateway' else 'gateway':<7}  {t}")
            except Exception:
                pass
            try:
                await peer.send(msg)
            except ConnectionClosed:
                pass
    except ConnectionClosed:
        pass
    finally:
        if role == "gateway":
            gateway_ws = None
            log.info("gateway disconnesso")
        else:
            if client_ws is ws: client_ws = None
            log.info("client disconnesso")


async def process_request(path, headers):
    """Se la richiesta è HTTP (non upgrade WS), serve file statici."""
    if "upgrade" in (headers.get("Upgrade", "") or "").lower():
        return None  # lascia gestire come WS

    static_map = {
        "/":            ("index.html", "text/html; charset=utf-8"),
        "/index.html":  ("index.html", "text/html; charset=utf-8"),
        "/app.css":     ("app.css",    "text/css; charset=utf-8"),
        "/app.js":      ("app.js",     "application/javascript; charset=utf-8"),
    }

    if path in static_map:
        fname, ctype = static_map[path]
        try:
            fpath = console_html_path.parent / fname
            body = fpath.read_bytes()
            return (200, [
                ("Content-Type", ctype),
                ("Content-Length", str(len(body))),
                ("Cache-Control", "no-cache"),
            ], body)
        except Exception as e:
            return (500, [("Content-Type", "text/plain")], str(e).encode())

    if path == "/health":
        body = json.dumps({
            "gateway_connected": gateway_ws is not None,
            "client_connected":  client_ws is not None,
        }).encode()
        return (200, [("Content-Type", "application/json")], body)

    return (404, [("Content-Type", "text/plain")], b"Not Found")


async def main():
    global console_html_path
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--console",
                    default=str(Path(__file__).parent.parent / "webui" / "index.html"),
                    help="path to webui/index.html (app.css e app.js nella stessa dir)")
    args = ap.parse_args()

    console_html_path = Path(args.console).resolve()
    if not console_html_path.exists():
        log.warning(f"console.html not found at {console_html_path}, only WS will work")
    else:
        log.info(f"serving console from {console_html_path}")

    async with websockets.serve(
        handler, "0.0.0.0", args.port,
        process_request=process_request,
    ):
        log.info(f"bridge listening on :{args.port}")
        log.info(f"  open in browser: http://localhost:{args.port}/")
        log.info(f"  gateway connects: ws://<host>:{args.port}/gateway")
        log.info(f"  client  connects: ws://<host>:{args.port}/client")
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
