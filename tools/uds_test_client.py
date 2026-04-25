#!/usr/bin/env python3
"""
Client CLI per testare il gateway ESP32 UDS via WebSocket.

Richiede: pip install websockets

Esempi d'uso:

    # Leggi VIN (DID 0xF190)
    python uds_test_client.py --uri ws://localhost:8080/gateway \\
        read-did --tx 0x7E0 --rx 0x7E8 --did 0xF190

    # Leggi DTC
    python uds_test_client.py --uri ws://... read-dtc --tx 0x7E0 --rx 0x7E8

    # Sessione + security access
    python uds_test_client.py --uri ws://... session --tx 0x7E0 --rx 0x7E8 --type 0x02
    python uds_test_client.py --uri ws://... security --tx 0x7E0 --rx 0x7E8 --level 0x01

    # Flash completo (upload + start)
    python uds_test_client.py --uri ws://... flash \\
        --tx 0x7E0 --rx 0x7E8 --address 0x08010000 \\
        --file firmware.bin --level 0x01

    # Modalità interattiva REPL
    python uds_test_client.py --uri ws://... repl --tx 0x7E0 --rx 0x7E8
"""
import argparse
import asyncio
import base64
import binascii
import json
import sys
import uuid
import zlib
from pathlib import Path

try:
    import websockets
except ImportError:
    print("Install websockets: pip install websockets", file=sys.stderr)
    sys.exit(1)


# ------------------------------------------------------------------
# Helpers
# ------------------------------------------------------------------
def hex_str(b: bytes) -> str:
    return b.hex().upper()


def parse_hex_int(s: str) -> int:
    return int(s, 0)


def make_id() -> str:
    return uuid.uuid4().hex[:12]


async def send_json(ws, obj):
    await ws.send(json.dumps(obj))


async def recv_matching(ws, pred, timeout=10.0):
    """Riceve finché pred(msg) -> True oppure timeout."""
    deadline = asyncio.get_event_loop().time() + timeout
    while True:
        remaining = deadline - asyncio.get_event_loop().time()
        if remaining <= 0:
            raise asyncio.TimeoutError()
        raw = await asyncio.wait_for(ws.recv(), timeout=remaining)
        msg = json.loads(raw)
        if pred(msg):
            return msg


async def uds_request(ws, tx, rx, sid, data_hex="", timeout_ms=2000):
    rid = make_id()
    await send_json(ws, {
        "id": rid,
        "type": "uds_request",
        "tx_id": f"0x{tx:03X}",
        "rx_id": f"0x{rx:03X}",
        "sid":   f"0x{sid:02X}",
        "data":  data_hex,
        "timeout_ms": timeout_ms,
    })
    msg = await recv_matching(
        ws,
        lambda m: m.get("type") == "uds_response" and m.get("id") == rid,
        timeout=timeout_ms / 1000 + 5,
    )
    return msg


def pretty_print(msg):
    status = msg.get("status")
    sid    = msg.get("sid", "")
    data   = msg.get("data", "")
    el     = msg.get("elapsed_ms", "?")
    if status == "positive":
        print(f"  ✓ positive  sid={sid}  data={data or '-'}  ({el} ms)")
    elif status == "negative":
        print(f"  ✗ negative  sid={sid}  nrc={msg.get('nrc')}  ({el} ms)")
    else:
        print(f"  ! {status}  sid={sid}  ({el} ms)")


# ------------------------------------------------------------------
# Comandi
# ------------------------------------------------------------------
async def cmd_read_did(ws, args):
    did_hex = f"{args.did:04X}"
    print(f"ReadDataByIdentifier DID=0x{did_hex}")
    msg = await uds_request(ws, args.tx, args.rx, 0x22, did_hex)
    pretty_print(msg)
    if msg.get("status") == "positive" and msg.get("data"):
        payload = bytes.fromhex(msg["data"])
        # skip i primi 2 byte (echo del DID)
        try:
            as_text = payload[2:].decode("ascii", errors="replace")
            print(f"  ascii: {as_text!r}")
        except Exception:
            pass


async def cmd_read_dtc(ws, args):
    print("ReadDTCInformation sub=0x02 mask=0xFF")
    msg = await uds_request(ws, args.tx, args.rx, 0x19, "02FF")
    pretty_print(msg)
    if msg.get("status") == "positive" and msg.get("data"):
        payload = bytes.fromhex(msg["data"])
        # skip sub + availability mask
        dtcs = payload[2:]
        print(f"  {len(dtcs) // 4} DTC(s):")
        for i in range(0, len(dtcs), 4):
            b = dtcs[i:i + 4]
            if len(b) < 4:
                break
            code_num = (b[0] << 16) | (b[1] << 8) | b[2]
            prefix = ["P", "C", "B", "U"][(b[0] >> 6) & 0x3]
            pretty = f"{prefix}{code_num & 0x3FFF:04X}"
            print(f"    {pretty}  status=0x{b[3]:02X}")


async def cmd_session(ws, args):
    print(f"DiagnosticSessionControl type=0x{args.type:02X}")
    msg = await uds_request(ws, args.tx, args.rx, 0x10, f"{args.type:02X}")
    pretty_print(msg)


async def cmd_security(ws, args):
    print(f"SecurityAccess level=0x{args.level:02X}")
    # request seed
    msg = await uds_request(ws, args.tx, args.rx, 0x27, f"{args.level:02X}")
    pretty_print(msg)
    if msg.get("status") != "positive":
        return
    data = bytes.fromhex(msg["data"])
    # data[0] = echo level, data[1:] = seed
    seed = data[1:]
    print(f"  seed: {seed.hex().upper()}")

    # compute key (algoritmo compatibile col simulatore)
    if len(seed) != 4:
        print("  (seed non 32-bit, non so calcolare la key di default)")
        return
    s = int.from_bytes(seed, "big")
    k = ((s ^ 0xA5A5A5A5) << 7 | (s ^ 0xA5A5A5A5) >> 25) & 0xFFFFFFFF
    print(f"  key:  {k:08X}")

    # send key
    payload = f"{args.level + 1:02X}{k:08X}"
    msg = await uds_request(ws, args.tx, args.rx, 0x27, payload)
    pretty_print(msg)


async def cmd_tester_present(ws, args):
    print("TesterPresent (suppress)")
    msg = await uds_request(ws, args.tx, args.rx, 0x3E, "80")
    pretty_print(msg)


async def cmd_raw(ws, args):
    print(f"RAW sid=0x{args.sid:02X} data={args.data}")
    msg = await uds_request(ws, args.tx, args.rx, args.sid, args.data,
                            timeout_ms=args.timeout)
    pretty_print(msg)


async def cmd_flash(ws, args):
    fw = Path(args.file).read_bytes()
    crc = zlib.crc32(fw) & 0xFFFFFFFF
    print(f"Firmware: {len(fw)} bytes, CRC32=0x{crc:08X}")

    # -------- upload begin --------
    rid = make_id()
    await send_json(ws, {
        "id":    rid,
        "type":  "flash_upload_begin",
        "size":  len(fw),
        "crc32": crc,
    })
    msg = await recv_matching(
        ws, lambda m: m.get("type") == "flash_upload_begin_ack" and m.get("id") == rid,
    )
    if msg.get("status") != "ok":
        print(f"  upload begin failed: {msg.get('message')}")
        return
    print("  upload begin OK")

    # -------- chunks --------
    CHUNK = args.chunk_size
    offset = 0
    while offset < len(fw):
        end = min(offset + CHUNK, len(fw))
        b64 = base64.b64encode(fw[offset:end]).decode()
        cid = make_id()
        await send_json(ws, {
            "id":       cid,
            "type":     "flash_upload_chunk",
            "offset":   offset,
            "data_b64": b64,
        })
        msg = await recv_matching(
            ws, lambda m: m.get("type") == "flash_upload_chunk_ack" and m.get("id") == cid,
        )
        if msg.get("status") != "ok":
            print(f"  chunk at {offset} failed: {msg.get('message')}")
            return
        offset = end
        pct = offset * 100 // len(fw)
        print(f"\r  upload {offset}/{len(fw)} ({pct}%)", end="", flush=True)
    print()

    # -------- upload end --------
    rid = make_id()
    await send_json(ws, {"id": rid, "type": "flash_upload_end"})
    msg = await recv_matching(
        ws, lambda m: m.get("type") == "flash_upload_end_ack" and m.get("id") == rid,
    )
    if msg.get("status") != "ok":
        print(f"  upload end failed: {msg.get('message')}")
        return
    print("  upload end OK, CRC verified")

    # -------- flash start --------
    rid = make_id()
    await send_json(ws, {
        "id": rid,
        "type": "flash_start",
        "tx_id":            f"0x{args.tx:03X}",
        "rx_id":            f"0x{args.rx:03X}",
        "address":          f"0x{args.address:08X}",
        "security_level":   f"0x{args.level:02X}",
        "erase_before":     True,
        "erase_routine_id": "0xFF00",
        "check_routine_id": "0xFF01",
    })
    msg = await recv_matching(
        ws, lambda m: m.get("type") == "flash_start_ack" and m.get("id") == rid,
    )
    if msg.get("status") != "ok":
        print(f"  flash_start refused: {msg.get('message')}")
        return
    print("  flash started, streaming progress...")

    # -------- progress + result --------
    while True:
        raw = await asyncio.wait_for(ws.recv(), timeout=120)
        msg = json.loads(raw)
        t = msg.get("type")
        if t == "flash_progress":
            phase = msg.get("phase")
            done  = msg.get("done", 0)
            total = msg.get("total", 0)
            if total:
                pct = done * 100 // total if total else 0
                print(f"\r  [{phase:<16}] {done}/{total} ({pct}%)", end="", flush=True)
            else:
                print(f"\r  [{phase:<16}]", end="", flush=True)
        elif t == "flash_result":
            print()
            status = msg.get("status")
            code   = msg.get("code")
            if status == "ok":
                print(f"  ✓ FLASH OK")
            else:
                print(f"  ✗ FLASH ERROR code={code}")
            return


async def cmd_repl(ws, args):
    print("REPL attivo. Comandi:")
    print("  rd <DID_hex>               ReadDataByIdentifier")
    print("  wr <DID_hex> <data_hex>    WriteDataByIdentifier")
    print("  dtc                        Read DTCs")
    print("  cdtc                       Clear DTCs (mask FFFFFF)")
    print("  ses <type_hex>             Session control")
    print("  sec <level_hex>            Security access")
    print("  tp                         Tester present")
    print("  raw <sid_hex> <data_hex>   Richiesta arbitraria")
    print("  quit")

    loop = asyncio.get_event_loop()
    while True:
        try:
            line = await loop.run_in_executor(None, input, "uds> ")
        except EOFError:
            break
        parts = line.strip().split()
        if not parts:
            continue
        cmd = parts[0]
        try:
            if cmd == "quit":
                break
            elif cmd == "rd":
                did = int(parts[1], 16)
                m = await uds_request(ws, args.tx, args.rx, 0x22, f"{did:04X}")
                pretty_print(m)
            elif cmd == "wr":
                did = int(parts[1], 16)
                data = parts[2]
                m = await uds_request(ws, args.tx, args.rx, 0x2E, f"{did:04X}{data}")
                pretty_print(m)
            elif cmd == "dtc":
                m = await uds_request(ws, args.tx, args.rx, 0x19, "02FF")
                pretty_print(m)
            elif cmd == "cdtc":
                m = await uds_request(ws, args.tx, args.rx, 0x14, "FFFFFF")
                pretty_print(m)
            elif cmd == "ses":
                m = await uds_request(ws, args.tx, args.rx, 0x10, f"{int(parts[1], 16):02X}")
                pretty_print(m)
            elif cmd == "sec":
                args.level = int(parts[1], 16)
                await cmd_security(ws, args)
            elif cmd == "tp":
                m = await uds_request(ws, args.tx, args.rx, 0x3E, "80")
                pretty_print(m)
            elif cmd == "raw":
                sid = int(parts[1], 16)
                data = parts[2] if len(parts) > 2 else ""
                m = await uds_request(ws, args.tx, args.rx, sid, data)
                pretty_print(m)
            else:
                print("comando non riconosciuto")
        except Exception as e:
            print(f"errore: {e}")


# ------------------------------------------------------------------
# Main
# ------------------------------------------------------------------
async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uri", required=True, help="WebSocket URI del gateway")
    ap.add_argument("--token", default=None, help="Bearer token (opzionale)")

    sub = ap.add_subparsers(dest="cmd", required=True)

    def add_common(p):
        p.add_argument("--tx", type=parse_hex_int, required=True)
        p.add_argument("--rx", type=parse_hex_int, required=True)

    p = sub.add_parser("read-did"); add_common(p); p.add_argument("--did", type=parse_hex_int, required=True)
    p = sub.add_parser("read-dtc"); add_common(p)
    p = sub.add_parser("session");  add_common(p); p.add_argument("--type", type=parse_hex_int, required=True)
    p = sub.add_parser("security"); add_common(p); p.add_argument("--level", type=parse_hex_int, default=1)
    p = sub.add_parser("tp");       add_common(p)
    p = sub.add_parser("raw")
    add_common(p); p.add_argument("--sid", type=parse_hex_int, required=True)
    p.add_argument("--data", default=""); p.add_argument("--timeout", type=int, default=2000)
    p = sub.add_parser("flash"); add_common(p)
    p.add_argument("--address", type=parse_hex_int, required=True)
    p.add_argument("--file",    required=True)
    p.add_argument("--level",   type=parse_hex_int, default=1)
    p.add_argument("--chunk-size", type=int, default=2048)
    p = sub.add_parser("repl"); add_common(p); p.add_argument("--level", type=parse_hex_int, default=1)

    args = ap.parse_args()

    headers = {}
    if args.token:
        headers["Authorization"] = f"Bearer {args.token}"

    async with websockets.connect(args.uri, extra_headers=headers) as ws:
        fn = {
            "read-did": cmd_read_did,
            "read-dtc": cmd_read_dtc,
            "session":  cmd_session,
            "security": cmd_security,
            "tp":       cmd_tester_present,
            "raw":      cmd_raw,
            "flash":    cmd_flash,
            "repl":     cmd_repl,
        }[args.cmd]
        await fn(ws, args)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
