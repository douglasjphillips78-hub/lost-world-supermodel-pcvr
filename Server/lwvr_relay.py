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

directory = {}                 # (game, code) -> (ip, port, time, public): rooms hosted directly (see PUT/GET)
LISTING_LIFE = 150             # a public direct room is listed while its host keeps refreshing it (every ~60 s)
DIRECTORY_LIFE = 2 * 3600
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
        if len(parts) == 2 and parts[0] == "LIST" and GAME_RE.match(parts[1]):
            # The public game list: "ROOM <code> D|R <seconds>" lines (D = joinable directly, R = through
            # this relay), then END. Addresses are only handed out to someone who asks to join.
            now = time.time()
            out = []
            for (g, c), v in list(directory.items()):
                if g == parts[1] and v[3] and now - v[2] < LISTING_LIFE:
                    out.append(f"ROOM {c} D {int(now - v[2])}")
            for (g, c), v in list(rooms.items()):
                if g == parts[1] and v[3]:
                    out.append(f"ROOM {c} R {int(now - v[4])}")
            writer.write(("\n".join(out[:40]) + ("\n" if out else "") + "END\n").encode())
            await writer.drain()
            writer.close()
            return
        if parts and parts[0] in ("PUT", "GET", "DEL") and len(parts) >= 3 and CODE_RE.match(parts[1]) and GAME_RE.match(parts[2]):
            # The directory: a host that can be reached directly leaves its address under its room
            # code, a joiner asks for it. (Lets people pick their own code without the relay
            # carrying any game data.)
            now = time.time()
            for k in [k for k, v in directory.items() if now - v[2] > DIRECTORY_LIFE]:
                del directory[k]
            key = (parts[2], parts[1])
            if parts[0] == "PUT" and len(parts) in (4, 5) and parts[3].isdigit() and 0 < int(parts[3]) < 65536:
                if key in directory and directory[key][0] != peer[0] and now - directory[key][2] < 600:
                    writer.write(b"BUSY\n")
                else:
                    public = len(parts) == 5 and parts[4] == "pub"
                    if key not in directory:
                        log(f"directory: {parts[1]} ({parts[2]}) -> {peer[0]}:{parts[3]}{' (public)' if public else ''}")
                    directory[key] = (peer[0], int(parts[3]), now, public)
                    writer.write(b"OK\n")
            elif parts[0] == "GET" and len(parts) == 3:
                e = directory.get(key)
                writer.write(f"ADDR {e[0]} {e[1]}\n".encode() if e else b"NO\n")
            elif parts[0] == "DEL" and len(parts) == 3:
                if key in directory and directory[key][0] == peer[0]:
                    del directory[key]
                writer.write(b"OK\n")
            else:
                writer.write(b"BAD\n")
            await writer.drain()
            writer.close()
            return
        public = len(parts) == 4 and parts[0] == "HOST" and parts[3] == "pub"
        if public:
            parts = parts[:3]
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
            rooms[key] = (reader, writer, done, public, time.time())
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
        hreader, hwriter, done = room[:3]
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
