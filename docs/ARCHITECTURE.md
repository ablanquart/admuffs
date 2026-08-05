# Architecture

## Data flow

```
                        ┌─────────────┐
   TV audio ── (mic) ─► │  AudioBus   │  one ALSA capture thread, 15 s ring
                        └──────┬──────┘
                 ┌─────────────┴────── detection sources ────────────┐
                 │ LoudnessSource      AcrSource        MetadataSource* │
                 └───────────────┬────────────────────────────────────┘
                                 │ SourceVerdict{signal, confidence}
                                 ▼
                          FusionEngine  (weighted sum + hysteresis)
                                 │ FusionEvent{EnterCommercial | ExitCommercial}
                                 ▼
                          TvController  (Composite: API primary → IR fallback)
                                 │
             ┌───────────────────┼───────────────────────────┐
             ▼                                                 ▼
   network API (Roku/Samsung/LG/Sony/Vizio)          IR (irsend / ir-ctl)
             │                                                 │
             └────────────────────► the TV ◄───────────────────┘
     (* Metadata is a stub in this build)
```

## ACR pipeline (`src/detect/acr_client.*`, `AcrSource`)

Every `acr_interval_s` seconds, `AcrSource` pulls the last `acr_sample_seconds`
of mono S16 audio from the bus, decimates to 8 kHz when the rate divides
evenly, wraps it in a WAV container, signs the request per ACRCloud signature
v1 — `base64(HMAC-SHA1("POST\n/v1/identify\n<key>\naudio\n1\n<ts>", secret))` —
and multipart-POSTs it to `<acr_host>/v1/identify`. Response mapping:

| ACRCloud response | Verdict |
|---|---|
| `status.code 0` with `metadata.custom_files` (ads bucket hit) | **Commercial**, confidence 0.5–1.0 from the match score |
| `status.code 1001` (no result) | weak **Program** (0.25) — the ads bucket didn't recognize it |
| `status.code 0` with only `metadata.music` | **Unknown** — licensed music appears in both ads and shows |
| transport/auth error | **Unknown**, logged |

Verdicts expire after ~2.5 missed cycles so a stale "commercial" can never pin
the TV muted. The pipeline is pure free functions, unit-tested against a mock
server that re-verifies the HMAC signature byte-for-byte.

The run loop (`App::run`, `src/app.cpp`) polls each source every 200 ms, folds
the source's trust `weight()` into the confidence, and feeds the set to the
`FusionEngine`. `EnterCommercial` begins *suppression* and `ExitCommercial`
ends it, where suppression is the configured `mute_mode`: `mute` sends
Mute/Unmute; `volume_drop` steps the volume down/up by `drop_steps`.

## The mute paradox & suppression (`App::begin/end_suppression`)

A room microphone goes deaf when we mute the TV -- it would report the
silence *we* created as "program resumed" and the loop would oscillate. Three
mechanisms handle this, keyed by `audio_tap` and `mute_mode`:

- **Verdict gating** (`room` + `mute`): while suppressing, verdicts from
  sources with `hears_tv_audio()==true` (loudness, ACR) that say *Program*
  are downgraded to *Unknown* before fusion. Fusion holds the commercial
  state; unmute comes from the failsafe timer.
- **Failsafe timer** (`max_mute_s`, all modes): suppression never outlives
  this bound without a positive program signal. On expiry the audio is
  restored and `FusionEngine::reset()` clears state so a still-running break
  can re-trigger cleanly. Audio is also always restored on shutdown.
- **Drop compensation** (`room` + `volume_drop`): after stepping the volume down,
  the app measures the actual room attenuation (RMS before vs. after) and
  installs it via `LoudnessSource::set_comp_db()`, so volume-dropped audio is judged
  on the original scale and the *signal itself* drives the restore.

- **Normalize mode** (`mute_mode=normalize`, `App::normalize_tick`): fusion
  events become informational; instead, every `norm_interval_ms` the run loop
  compares a ~2 s EMA of room level against `norm_target_db` (set via `POST
  /sample/level` -> `App::set_norm_target`) and sends a single VolumeUp/Down
  when outside the `norm_tolerance_db` deadband. Guards: `norm_max_range`
  bounds net drift, a target-15 dB silence floor prevents volume-up during
  quiet pauses, and `restore_norm()` unwinds the net steps on disable or
  shutdown.

With `audio_tap=upstream` none of this is needed -- the tap hears the
broadcast regardless of TV state -- so verdicts pass through unmodified and
unmute timing is exact. That is the recommended wiring (HARDWARE.md §4).

## Fusion decision

`FusionEngine::update` (`src/detect/fusion.cpp`) computes a signed weighted
average of verdicts: commercial contributes `+confidence`, program `−confidence`,
unknown nothing. The result is a score in `[-1, +1]`. A transition only fires
after the score stays past `fire_threshold` for `mute_debounce_ms`
(commercial) / `unmute_debounce_ms` (program) — this hysteresis is what stops
the TV flapping on a single loud explosion in the show or a quiet beat in an ad.

Because sources that return `Unknown` contribute nothing, disabled/stub layers
are harmless: with only loudness active, the decision is loudness-driven; adding
ACR/metadata simply adds weighted votes.

## TV control

`TvController` (`src/tv/tv_controller.h`) is the interface: `available()`,
`connect()`, `send(TvCommand)`. The factory (`src/tv/factory.cpp`) builds:

- `ControlMethod::Ir` → `IrController` only.
- `ControlMethod::Api` → the brand's API controller only.
- `ControlMethod::Auto` → a `CompositeController` that uses the network API when
  it's reachable and connects, and transparently falls back to IR otherwise (and
  per-command, if an API send fails).

### Per-brand API notes

- **Roku** — unauthenticated ECP. `VolumeMute` is a *toggle*, so mute, unmute,
  and toggle all issue the same keypress. Discovery via SSDP `roku:ecp`.
- **Samsung Tizen** — WebSocket remote (`...samsung.remote.control`).
  `KEY_MUTE` toggles. Connect strategy: with a stored token, go straight to
  `wss://ip:8002`; otherwise try plaintext `ws://ip:8001` (older sets), then
  fall back to `wss://ip:8002` with a 30 s greeting window so the user can
  accept the on-screen "allow" prompt. The `ms.channel.connect` greeting is
  parsed for a pairing token, which is pushed through `CredentialSaveFn` and
  persisted to the config file automatically (`App::credential_saver`).
- **LG webOS** — SSAP, preferring the secure socket `wss://ip:3001` (webOS
  4.x+) and falling back to `ws://ip:3000` for older sets. The plaintext port
  is often slow-pathed on newer TVs, which can delay the on-screen pairing
  prompt by minutes; the secure port avoids that. Supports *explicit* mute
  state (`ssap://audio/setMute`), so mute/unmute are precise. The one-time
  registration waits on a 5-minute deadline (not a fixed read count) so a slow
  prompt still completes, and logs timing so the delay's source is visible.
  The returned `client-key` is stored.
- **Sony Bravia** — IRCC-IP SOAP (`/sony/IRCC`) plus the audio REST service
  (`/sony/audio setAudioMute`) for explicit mute; authenticated with a
  pre-shared key you set on the TV (Settings → Network → IP control).
- **Vizio SmartCast** — HTTPS REST on `:7345` with a self-signed cert (the HTTP
  client sets `insecure` for this host). Needs an auth token from PIN pairing,
  which the wizard performs (`/pairing/start` → enter PIN → `/pairing/pair`).

### IR fallback

`IrController` maps `TvCommand` → logical key (`KEY_MUTE`, …), looks it up in the
`IrTvProfile` from the JSON database, and hands it to `IrTransmitter`, which
shells out to `irsend` (LIRC) or `ir-ctl` (raw pulses). Shelling out avoids a
link-time dependency on liblirc and matches the standard Pi OS tooling.

## Networking helpers (`src/net/`)

- `http.*` — libcurl wrapper (GET/POST/PUT, custom headers, per-host
  `insecure`, multipart/form-data for ACR uploads).
- `ws_client.*` — a compact RFC 6455 client with optional TLS (`connect(...,
  use_tls)`): OpenSSL, SNI, `SSL_VERIFY_NONE` because TVs present self-signed
  LAN certificates. Client-masked frames, ping/pong, close frame on shutdown.
- `discovery.*` — SSDP M-SEARCH for finding TVs (`roku:ecp`, DIAL, `ssdp:all`).

## Web remote (`src/web/`)

`WebServer` is a small single-threaded HTTP/1.1 server (own thread, one
request per connection) started by `App::run` when `web_port > 0`. It serves
an embedded, self-contained HTML remote and three endpoints: `POST
/key/<name>` presses a button, `GET /status` reports the active controller
and auto-mute state, `POST /automute` toggles automatic muting, `GET /config` + `POST
/config/set` read/write the whitelisted TUI-equivalent settings (validated,
persisted, live-vs-restart classified), `POST /ir/record/<KEY>` captures a
button from the IR receiver into `recorded_keys.json` (which
`make_ir_controller` merges over the database), `GET /info` reports host
facts + I2C sensor readings (`src/sensors/`, `src/sys/`), `GET /log` serves
the in-memory log ring (`log_since` in common.cpp), `POST /restart` exits
with code 75 for systemd's Restart=always, and `POST
/sample/normal` / `POST /sample/commercial` record loudness-calibration
samples (App::sample_audio: ~2 s RMS from the AudioBus, running average per
label persisted in the config; with both labels present the loudness
threshold becomes 60% of the measured dB gap, clamped to 1.5-12 dB, applied
live via LoudnessSource::set_delta_db and reloaded at startup). The
toggle gates only the fusion events' TV sends (`App::run`); detection sources
and the fusion state keep running, manual presses always work, and flipping
the switch mid-commercial sends the compensating mute/unmute
(`App::set_automute`). Presses call
the same `TvController` instance as the mute loop, serialized by a mutex in
`App`, so a web press and an auto-mute can never interleave on the wire.

The command vocabulary (`TvCommand`) covers Power, D-pad + OK, Back,
Home, Settings, digits 0-9, volume/channel rockers, Mute, and Input. Controllers return `false` for
commands their protocol cannot express (LG SSAP has no D-pad without the
pointer-input socket; Roku ECP has no input cycle; Vizio D-pad codes are
community-mapped) — the composite controller then retries that single press
over IR, so the web remote works uniformly as long as the IR profile carries
the key.

## Adding a brand

1. Add `data/ir_codes/<brand>.json` (and list it in `index.json`) with at least
   `KEY_MUTE`.
2. For a network API, add a `TvController` subclass in `src/tv/api_controllers.*`
   and a branch in `make_api_controller` (`src/tv/factory.cpp`).

## Adding a detection source

Implement `DetectionSource` (`src/detect/detector.h`): `start/stop/poll`,
`weight()`, `enabled()`. Return `SourceVerdict{signal, confidence}` from
`poll()`. Register it in `App::init` (`src/app.cpp`). The ACR and metadata stubs
in `src/detect/sources.cpp` show exactly where the recognition/feed logic goes.
