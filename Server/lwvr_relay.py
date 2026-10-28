#!/usr/bin/env python3
"""
Relay server for two-headset play over the internet (The Lost World VR).

Both headsets connect OUT to this server, so nobody has to open a port on their router. The
first one says "HOST <code> <game>", the second says "JOIN <code> <game>"; the server pairs them
by code and from then on just passes the bytes between the two. It never looks at the game data.

Run:   python3 lwvr_relay.py [--port 1992]
Needs: Python 3.8 or newer, nothing to install. Open TCP port 1992 on the machine's firewall.
"""
import argparse
import asyncio
import re
import time

WAIT_FOR_JOINER = 30 * 60      # seconds a host may wait in a room
IDLE_LIMIT = 45                # seconds without any data from a side = the session is dead
SESSION_LIMIT = 500 * 2**20    # bytes per session (a game is ~20 MB/hour plus a ~15 MB snapshot)
MAX_ROOMS = 200

rooms = {}                     # (game, code) -> (reader, writer, done_event)
CODE_RE = re.compile(r"^[A-Za-z0-9]{4,12}$")
GAME_RE = re.compile(r"^[A-Za-z0-9_]{1,24}$")


def log(msg):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), msg, flush=True)


async def pipe(src, dst, counter):
    try:
        while True:
            data = await asyncio.wait_for(src.read(65536), IDLE_LIMIT)
            if not data:
                break
            counter[0] += len(data)
            if counter[0] > SESSION_LIMIT:
                break
            dst.write(data)
            await dst.drain()
    except (asyncio.TimeoutError, ConnectionError, OSError):
        pass


async def handle(reader, writer):
    peer = writer.get_extra_info("peername")
    try:
        line = await asyncio.wait_for(reader.readline(), 10)
        parts = line.decode("ascii", "replace").split()
        if len(parts) != 3 or parts[0] not in ("HOST", "JOIN") or not CODE_RE.match(parts[1]) or not GAME_RE.match(parts[2]):
            writer.write(b"BAD\n")
            await writer.drain()
            writer.close()
            return
        verb, code, game = parts
        key = (game, code)

        if verb == "HOST":
            if key in rooms or len(rooms) >= MAX_ROOMS:
                writer.write(b"BUSY\n")
                await writer.drain()
                writer.close()
                return
            done = asyncio.Event()
            rooms[key] = (reader, writer, done)
            log(f"room {code} ({game}) opened by {peer[0]}")
            try:
                await asyncio.wait_for(done.wait(), WAIT_FOR_JOINER)
            except asyncio.TimeoutError:
                pass
            if rooms.get(key, (None, None, None))[2] is done:
                del rooms[key]             # nobody joined
            writer.close()
            return

        # JOIN
        room = rooms.pop(key, None)
        if room is None:
            writer.write(b"NO\n")
            await writer.drain()
            writer.close()
            return
        hreader, hwriter, done = room
        log(f"room {code} ({game}): {peer[0]} joined")
        hwriter.write(b"OK\n")
        writer.write(b"OK\n")
        await asyncio.gather(hwriter.drain(), writer.drain())
        counter = [0]
        t1 = asyncio.ensure_future(pipe(hreader, writer, counter))
        t2 = asyncio.ensure_future(pipe(reader, hwriter, counter))
        await asyncio.wait([t1, t2], return_when=asyncio.FIRST_COMPLETED)
        for t in (t1, t2):
            t.cancel()
        log(f"room {code} closed ({counter[0] // 1024} KB passed)")
        hwriter.close()
        writer.close()
        done.set()
    except (asyncio.TimeoutError, ConnectionError, OSError):
        try:
            writer.close()
        except Exception:
            pass


async def main(port):
    server = await asyncio.start_server(handle, "0.0.0.0", port)
    log(f"relay listening on port {port}")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=1992)
    asyncio.run(main(ap.parse_args().port))
