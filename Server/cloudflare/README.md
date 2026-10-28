# Room directory on Cloudflare (free, nothing to host)

This small Worker remembers "room code -> where the host can be reached" and keeps the public game
list. It never sees game data: the two headsets connect straight to each other.

## Deploy (about 5 minutes, once)

1. Make a free account at https://dash.cloudflare.com/sign-up
2. Install Node.js (https://nodejs.org, the LTS version) if you do not have it.
3. In Git Bash:

       cd Server/cloudflare
       npx wrangler login          # opens a browser; allow it
       npx wrangler deploy

4. It prints an address like `https://lwvr-directory.YOURNAME.workers.dev`. That is the directory.
5. Tell the headsets about it, in `Supermodel.ini` (or have it built in as the default):

       LockstepDirectory = https://lwvr-directory.YOURNAME.workers.dev

Test it from a terminal:

    curl -X POST https://lwvr-directory.YOURNAME.workers.dev -d "LIST lostwsga"     # -> END

The free plan allows 100,000 requests a day; a hosted public game uses about 60 a hour.
