# Security

Admuffs runs on your home LAN and controls a TV. It is not designed to be
exposed to the public internet, and there is no TLS — treat it like any other
smart-home appliance on a trusted network. Within that scope, the web remote
is hardened along OWASP lines.

## Threat model

- **In scope:** other devices/users on the same LAN reaching the web remote;
  malformed or hostile HTTP requests; injection via any value the UI can set.
- **Out of scope:** a hostile actor already on the Pi as the admuffs user
  (they can read the config directly); physical IR replay; internet exposure
  (don't port-forward this — see below).

## Authentication

- The web remote requires a **PIN**. Default is **`0000`** — change it under
  **ADMUFFS SETTINGS → CHANGE PIN** before putting the Pi on a shared network.
- The PIN is never stored in plaintext. The config holds only
  `pin_salt` (16 random bytes) and `pin_hash` = `SHA-256(salt ":" pin)`.
- PIN verification is **constant-time**; a correct PIN issues a **256-bit
  session token** from the OS CSPRNG (`/dev/urandom` via OpenSSL `RAND_bytes`),
  returned as an **HttpOnly, SameSite=Strict** cookie (12-hour sliding expiry).
- **Brute-force lockout:** 5 wrong PINs → 60-second lockout (a 4-digit PIN has
  only 10 000 combinations). Changing the PIN invalidates all sessions.
- Every endpoint except the static page shell and `POST /auth` returns **401**
  without a valid session.

## Input handling

- **No shell for IR.** IR transmission uses `fork`/`execvp` with an argument
  vector (`run_argv`) — never a shell string — so no value (including the
  web-settable `ir_remote`) can inject a command.
- **Config editor allowlists.** `POST /config/set` only accepts a fixed set of
  keys; values are length-capped, stripped of control characters, and
  charset-validated per field (hostnames, device paths, and names each have a
  conservative allowlist). Enums and numbers are range/format checked.
- **IR key names** recorded via the web are validated against `^KEY_[A-Z0-9_]+$`
  before use.
- **Request limits:** headers capped at 16 KiB, bodies at 4 KiB.
- Output that echoes user/host strings into JSON is escaped.

## Transport & headers

Every response carries: a strict `Content-Security-Policy`
(`default-src 'none'`, same-origin connect, no framing, no external loads),
`X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`,
`Referrer-Policy: no-referrer`, and `Cache-Control: no-store`.

There is **no TLS**: traffic is plaintext HTTP on the LAN. Do not port-forward
port 8995 or expose it through a reverse proxy without adding TLS and
authentication in front of it.

## Secrets at rest

- The config file (`~/.config/admuffs/admuffs.conf`) is written **`0600`** and
  holds the PIN hash and any ACR credentials. It is created owner-only from the
  first byte (a tightened `umask` around the write), so there is no window in
  which it is world-readable.
- The log file is written **`0600`**. Secrets are never logged: both ACR
  credentials (key and secret) are masked, and the PIN is never written
  anywhere in any form.
- Both ACR credentials are masked in the `/config` response as well; submitting
  the mask value unchanged leaves the stored secret intact.
- `.gitignore` excludes `*.conf` (except the example), `*.log`, and
  `recorded_keys.json` so runtime state and secrets can't be committed.

## Request handling / availability

- The web server enforces an overall read deadline per connection, so a client
  that opens a socket and dribbles bytes (slow-request / slowloris) is dropped
  rather than tying up the single server thread.
- `Content-Length` bodies are hard-capped (4 KiB), header reads are bounded
  (~16 KiB), and all URL/cookie parsing is length-checked.
- All values reflected into JSON (errors, log lines, controller name) are
  escaped for control characters, so a smuggled newline/NUL can't forge or
  split a line in the log viewer.
- Numeric settings are range-checked (`web_port` 1–65535, and sane bounds on
  the volume-drop/normalize/timeout fields) so a bad value can't overflow or self-DoS.
- WebSocket frames from a TV are size-capped before allocation, and idle web
  sessions are reaped so tokens can't accumulate unbounded.
- `SIGPIPE` is ignored process-wide: a client that disconnects mid-response
  produces an `EPIPE` error return, not process death.

## Running as a service

`sudo admuffs --install-service` runs admuffs as the **invoking user**
(`SUDO_USER`), not root, so it holds only the `audio`/`i2c`/`video`/`gpio`
group access it needs. `--install-service` itself requires root only to write
the unit file.

## Reporting

Found something? Open a GitHub issue (omit exploit details for anything
sensitive) or contact the maintainer directly.
