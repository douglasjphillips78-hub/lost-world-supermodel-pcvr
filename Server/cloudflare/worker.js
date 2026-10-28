// Room directory for two-player Lost World VR, as a Cloudflare Worker (free plan, nothing to host).
//
// It only remembers "room code -> where the host can be reached" for a little while, and keeps the
// public game list. It never sees any game data: the two headsets then connect straight to each other.
//
// The game sends one line of text in a POST and gets one reply:
//   PUT <code> <game> <port> [pub] [ip=<ipv4>] [tok=<secret>]   -> OK | BUSY | BAD
//   GET <code> <game>                                           -> ADDR <ip> <port> | NO
//   DEL <code> <game> [tok=<secret>]                            -> OK
//   LIST <game>                                                 -> ROOM <code> D <seconds> ... END

const LIFE = 2 * 3600;       // seconds a room stays after its last refresh
const LISTING_LIFE = 150;    // a public room is listed while its host keeps refreshing (about once a minute)
const CODE_RE = /^[A-Za-z0-9]{4,12}$/;
const GAME_RE = /^[A-Za-z0-9_]{1,24}$/;
const IPV4_RE = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/;

export class Directory {
  constructor(state) { this.state = state; }

  async fetch(request) {
    const line = (await request.text()).trim().slice(0, 200);
    const all = line.split(/\s+/);
    const opts = {};
    const parts = [];
    for (const t of all) {
      const m = /^(ip|tok)=(.+)$/.exec(t);
      if (m) opts[m[1]] = m[2]; else parts.push(t);
    }
    const now = Math.floor(Date.now() / 1000);
    const st = this.state.storage;
    const reply = (t) => new Response(t + "\n", { headers: { "content-type": "text/plain" } });

    if (parts[0] === "LIST" && parts.length === 2 && GAME_RE.test(parts[1])) {
      const rows = await st.list({ prefix: parts[1] + "/" });
      const out = [];
      for (const [key, v] of rows) {
        if (v.public && now - v.time < LISTING_LIFE) out.push(`ROOM ${key.split("/")[1]} D ${now - v.time}`);
      }
      return reply(out.slice(0, 40).concat(["END"]).join("\n"));
    }

    if (parts.length < 3 || !CODE_RE.test(parts[1]) || !GAME_RE.test(parts[2])) return reply("BAD");
    const key = parts[2] + "/" + parts[1];
    const cur = await st.get(key);
    const live = cur && now - cur.time < LIFE;

    if (parts[0] === "PUT" && /^\d+$/.test(parts[3] || "") && +parts[3] > 0 && +parts[3] < 65536) {
      let ip = opts.ip && IPV4_RE.test(opts.ip) ? opts.ip : request.headers.get("CF-Connecting-IP");
      if (!ip || !IPV4_RE.test(ip)) return reply("BAD");
      if (live && cur.tok !== (opts.tok || "") && now - cur.time < 600) return reply("BUSY");
      await st.put(key, { ip, port: +parts[3], time: now, public: parts.includes("pub"), tok: opts.tok || "" });
      // tidy: forget old rooms now and then
      if (Math.random() < 0.05) {
        for (const [k, v] of await st.list()) if (now - v.time > LIFE) await st.delete(k);
      }
      return reply("OK");
    }
    if (parts[0] === "GET" && parts.length === 3) {
      return reply(live ? `ADDR ${cur.ip} ${cur.port}` : "NO");
    }
    if (parts[0] === "DEL" && parts.length === 3) {
      if (live && cur.tok === (opts.tok || "")) await st.delete(key);
      return reply("OK");
    }
    return reply("BAD");
  }
}

export default {
  async fetch(request, env) {
    if (request.method !== "POST") return new Response("Lost World VR room directory\n");
    const stub = env.DIRECTORY.get(env.DIRECTORY.idFromName("main"));
    return stub.fetch(request);
  },
};
