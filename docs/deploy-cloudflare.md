# Deploying the browser build to Cloudflare Pages

## The one thing that will bite you

This build uses **pthreads** (Web Workers over a `SharedArrayBuffer` heap). A
page can only allocate a `SharedArrayBuffer` if it is **cross-origin isolated**,
which means the server must send:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Without them the game does not start at all — it fails while loading, not with
a clear message. There is no client-side workaround; it has to come from the
server. On Cloudflare Pages that is the `_headers` file below.

(Removing this requirement is the point of the un-threading work on
`modernize/wasm`; see `docs/unthread.md` §7.6. Until that lands, these headers
are mandatory.)

## Build

```sh
source /path/to/emsdk/emsdk_env.sh
cmake --preset wasm
cmake --build --preset wasm
```

Produces in `build/wasm/`:

| file | ~size | what |
|---|---|---|
| `uqm.html` | 3 KB | page shell |
| `uqm.js` | 1.1 MB | Emscripten loader |
| `uqm.wasm` | 2.1 MB | the game |
| `uqm.data` | 13.7 MB | content pack |

## Assemble the site

Pages serves `index.html` at `/` and does **not** fall back to any other page,
so the build output cannot be published as-is — `uqm.html` is not `index.html`
and `/` would 404. Rename it, and ship only the one page:

```sh
rm -rf dist && mkdir dist
cp build/wasm/uqm.html dist/index.html
cp build/wasm/uqm.{js,wasm,data} dist/
```

The HTML loads `uqm.js` by name, so only the page itself is renamed.

Do not try to solve this with a `_redirects` rule pointing at `uqm.html`.
**Pages consults `_redirects` only for requests that do not match a static
asset**, so as soon as `uqm.html` exists in the output it is served directly
and the rule never runs. Shipping one page avoids the question.

Then `dist/_headers`:

```
/*
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp

/*.wasm
  Content-Type: application/wasm
  Cache-Control: public, max-age=31536000, immutable

/*.data
  Cache-Control: public, max-age=31536000, immutable

/*.js
  Cache-Control: public, max-age=31536000, immutable

/index.html
  Cache-Control: no-cache
```

The long cache lifetimes are safe for `.wasm`/`.data`/`.js` only because the
HTML is `no-cache` — a redeploy changes the payload but reuses the filenames,
so if the HTML were cached too, returning players would get a stale mix of old
and new files. If you add content hashing to the filenames later, drop the
`no-cache`.

## Deploy

```sh
npx wrangler pages deploy dist --project-name uqm
```

**Do not publish `build/wasm` directly** — not from the CLI and not by pointing
the dashboard's output directory at it. It has no `index.html`, so `/` returns
404, and no `_headers`, so even reaching `/uqm.html` gives a page that cannot
start. If you wire up a dashboard build, make the assembly part of the build
command and publish `dist`:

```
cmake --preset wasm && cmake --build --preset wasm && sh tools/wasm-dist.sh
```

## Check it worked

Open the site and run in the console:

```js
crossOriginIsolated   // must be true
```

If it is `false`, the headers are not arriving and the game will not start.
Check them with `curl -I https://your-site.pages.dev/`.

## Notes

- **Size.** ~17 MB, dominated by `uqm.data`. Cloudflare's 25 MB per-file limit
  is not a problem, but first load is a real download; the shell shows a
  progress bar.
- **Saves** live in IndexedDB via IDBFS, flushed every 5 s and on `pagehide`.
  They are per-origin, so a preview deployment has separate saves from
  production.
- **The debug log** is hidden. Append `?log` to the URL to show it, or call
  `uqmLog()` in the console. Worth knowing when a report says "it just hangs".
- **Fullscreen** is F11 (browser fullscreen), hinted under the canvas.
