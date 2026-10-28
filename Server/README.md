# Relay server for internet two-player

Both headsets connect out to this server, which pairs them by room code and passes bytes on.
It never reads the game data. One small program, Python 3.8+, nothing to install.

    python3 lwvr_relay.py --port 1992

Open TCP port 1992 on the machine's firewall. Then on **both** Quests add to
`Android/data/<package>/files/Config/Supermodel.ini`:

    LockstepRelay = your.server.name.or.ip:1992
    LockstepOnlineDelay = 6        ; frames of input delay over the internet (try 6-8)

In the headset menu (left menu button) -> 2 PLAYER: one player picks HOST ONLINE and reads out
the 6-digit code, the other picks JOIN ONLINE, types it and presses GO.

Docker: `docker build -t lwvr-relay . && docker run -d -p 1992:1992 --restart always lwvr-relay`

The relay also keeps a small directory: a host that opened its own router port leaves its address
under its room code (`PUT`), and a joiner looks it up (`GET`). That is what lets people choose their
own short room code and still connect directly, with the relay carrying no game data. Entries expire
after two hours or when the host stops.
