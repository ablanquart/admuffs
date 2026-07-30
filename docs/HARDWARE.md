# Hardware setup — ANAVI Infrared pHAT

## Overview

The ANAVI Infrared pHAT gives the Raspberry Pi an **IR transmitter** (an IR LED,
used to send remote codes to your TV) and an **IR receiver** (a TSOP demodulator,
used to *record* codes from your existing remote). It also has three I²C slots
for optional sensors (BH1750 light, HTU21D temp/humidity, BMP180 pressure) that
admuffs does not require.

## 1. Enable the IR overlays

On Raspberry Pi OS, IR is handled by the kernel `gpio-ir` (receive) and
`gpio-ir-tx` (transmit) device-tree overlays. The ANAVI pHAT wires the LED to
**GPIO17** and the receiver to **GPIO18**. Add to `/boot/firmware/config.txt`
(older images: `/boot/config.txt`):

```
dtoverlay=gpio-ir,gpio_pin=18
dtoverlay=gpio-ir-tx,gpio_pin=17
```

> **Two classic ways these lines silently do nothing:**
>
> 1. **Wrong file.** On Raspberry Pi OS Bookworm the live config is
>    `/boot/firmware/config.txt`; the old `/boot/config.txt` still exists but
>    is just a "this file has moved" stub, and edits there are ignored.
> 2. **Wrong section.** config.txt has conditional sections — `[pi4]`,
>    `[pi5]`, `[cm4]`, `[cm5]`, `[all]` — and every line below a section
>    header applies *only* to that model. The stock file **ends** with such
>    sections, so lines appended at the bottom often land inside one (e.g.
>    `[cm5]`) and are skipped on your board. Put the `dtoverlay` lines under
>    an `[all]` header (or above the first bracketed section):
>
>    ```
>    [all]
>    dtoverlay=gpio-ir,gpio_pin=18
>    dtoverlay=gpio-ir-tx,gpio_pin=17
>    ```
>
> Either way, changes only take effect after a **reboot**. Verify with
> `dmesg | grep -iE "gpio-ir|lirc"` and `ls /dev/lirc*`, or just run
> `admuffs --ir-check`.

> Note: some revisions of ANAVI's own docs also show GPIO11/12 in a pinout
> table; the overlay values above (17 TX / 18 RX) are what the driver uses and
> what the board's setup instructions specify. If transmit doesn't work, swap
> the two pin numbers and re-test — board revisions vary. `scripts/setup-pi.sh`
> writes these lines for you.

Reboot, then confirm the LIRC character devices exist:

```bash
ls -l /dev/lirc*      # expect /dev/lirc0 (rx) and /dev/lirc1 (tx), or similar
```

> **Note on I²C:** enabling I²C in `raspi-config` is only needed for the
> pHAT's optional plug-in *sensors* — and it is completely independent of the
> IR transceiver (I²C = GPIO2/3, IR = GPIO17/18; different pins, different
> kernel subsystems). Enable both: simultaneous use is the designed setup,
> and the web remote's SYSTEM INFO panel then shows live readings from the
> HTU21D, BH1750, and BMP180 modules.

The fastest way to verify the whole chain is the built-in diagnostic:

```bash
./build/admuffs --ir-check
```

It asks the kernel directly which `/dev/lirc*` devices exist and whether each
can TRANSMIT or RECEIVE, checks that `irsend`/`ir-ctl` are installed, flags a
`dryrun` backend, and verifies your selected TV profile has usable codes for
the chosen backend (including whether LIRC actually knows the remote name).

## 2. Choose an IR backend

admuffs can transmit two ways — pick one in the wizard or config:

- **LIRC (`irsend`)** — the traditional stack. You provide a *remote config*
  (a `lircd.conf` describing your TV's codes) and admuffs calls
  `irsend SEND_ONCE <remote> KEY_MUTE`. Install with `sudo apt install lirc`.
- **kernel rc-core (`ir-ctl`)** — no LIRC daemon; admuffs sends raw carrier+pulse
  data straight to `/dev/lircN`. Install with `sudo apt install v4l-utils`.

### Required lircd configuration (irsend backend only)

A fresh `lirc` install **cannot transmit** until you fix two defaults in
`/etc/lirc/lirc_options.conf`:

```ini
[lircd]
driver = default          # Debian ships 'devinput', which is RECEIVE-ONLY
device = /dev/lirc0       # your TRANSMIT device -- check with: admuffs --ir-check
```

Then:

```bash
sudo systemctl restart lircd
```

Symptoms of getting this wrong: `irsend` (and admuffs's test mute) fail with
`hardware does not support sending` / `Error running command: Input/output
error`, even though the hardware is fine. The `devinput` driver can never
send, and pointing lircd at the RECEIVE device fails the same way — the two
`dtoverlay` lines create *separate* TX and RX devices, and which is `lirc0`
vs `lirc1` can vary, so always confirm with `admuffs --ir-check` ([1] shows
which is TRANSMIT). Note: with lircd bound to the TX device, lircd-based
receiving (`irrecord`, `irw`) is unavailable; record codes via the RX device
directly with `ir-ctl -r` / `scripts/record-ir-key.sh` instead. None of this
section applies to the `ir-ctl` backend, which bypasses lircd entirely.

Test transmit quickly with the built-in dry-run first:

```bash
./build/admuffs --dry-run --test-mute    # logs the code it *would* send
```

## 3. Get IR codes for your TV

Bundled coverage: raw ready-to-send codes for LG (NEC 0x04), Toshiba (NEC
0x40), Panasonic (Kaseikyo), and Magnavox/Philips (RC5) core keys, plus LIRC
name mappings for every brand. Brands whose IR protocols vary too much by
model line (JVC, Sharp, Sanyo, RCA, Emerson, Zenith, Hisense) ship as
name-only profiles: point your real remote at the pHAT and record (below) —
two minutes per key.

The bundled database (`data/ir_codes/`) has per-brand **LIRC button-name**
mappings (`KEY_MUTE`, `KEY_VOLUMEUP`, …) plus a **raw NEC example**. LIRC button
names only work if you have a matching remote config installed. If your TV isn't
covered, record your own — point your real remote at the pHAT's receiver:

### With LIRC

```bash
sudo systemctl stop lircd
sudo irrecord -d /dev/lirc0 ~/mytv.lircd.conf     # follow prompts, name buttons KEY_MUTE etc.
sudo cp ~/mytv.lircd.conf /etc/lirc/lircd.conf.d/
sudo systemctl start lircd
irsend LIST "" ""            # find your remote's name
irsend SEND_ONCE <remote> KEY_MUTE   # verify
```

Then set `ir_remote=<remote>` in the config (or pick it in the wizard).

### With ir-ctl (raw)

The easy way — the bundled helper records one keypress and prints a
ready-to-paste JSON entry:

```bash
./scripts/record-ir-key.sh KEY_MUTE          # auto-picks the RECEIVE device
./scripts/record-ir-key.sh KEY_POWER /dev/lirc1
```

Paste its output into the model's `"commands"` in
`data/ir_codes/<brand>.json`. (Manual route: `ir-ctl -r -d <rx device>`,
copy the pulse/space durations yourself — microseconds, alternating
pulse/space, list ending on a pulse.) Data files are read at startup, so no
rebuild is needed after editing them.

## 4. Audio input — and why WHERE you tap it matters

Detection listens to audio, and admuffs's response is to silence the TV —
which creates a trap called the **mute paradox**: a microphone in the room
goes deaf the moment the TV is muted, so nothing can hear when the commercial
ends. admuffs handles all configurations, but they behave differently:

| `audio_tap` | Hardware | Unmute is driven by |
|---|---|---|
| `upstream` **(recommended)** | Tap the audio *before* the TV: the cable box / streamer's analog line-out into a USB audio input, an HDMI audio extractor inline before the TV, or the TV's optical-out (many sets keep it live while speakers are muted — verify yours). | **The signal itself.** Detection hears the broadcast throughout the mute, so the TV unmutes right when the program resumes. Loudness *and* ACR keep working while muted. |
| `room` + `mute_mode=mute` | USB mic near the TV speakers. | **The `max_mute_s` timer** (default 240 s). While muted, the mic hears only the silence admuffs created, so audio "program" verdicts are discarded and the failsafe timer restores audio. Fixed-length muting: may clip a short break's tail or unmute into a long one. |
| `room` + `mute_mode=duck` | USB mic near the TV speakers. | **The signal.** Volume drops `duck_steps` steps instead of muting; the mic still hears the (quiet) ad. admuffs measures the attenuation and compensates the detector, so volume is restored when the program actually resumes. Trade-off: ads are quiet, not silent. |
| `room` + `mute_mode=normalize` | USB mic near the TV speakers. | **Continuous leveling, no mute at all.** admuffs nudges the volume so the room tracks the target you sampled with the web remote's SET VOLUME TARGET button. Loud ads are pulled down, quiet program comes back up. The most natural-feeling mic-only mode. |

Set `audio_tap` and `mute_mode` in the wizard or config. If you can run one
cable, run the upstream tap — it is the configuration this system is designed
around.

## 4b. Setting up the capture device

Loudness and ACR detection need to hear the TV. Any USB mic or USB audio input
works; place it near the TV speakers (or feed line-out/headphone-out into a
USB audio input).

The default `audio_device=auto` scans the sound cards and picks the first one
that can capture — on a Pi that's your USB mic, since the onboard audio is
playback-only. (That's also why `audio_device=default` fails on a Pi with
`capture slave is not defined`: ALSA's `default` points at the playback-only
onboard card.)

To pin a specific device instead:

```bash
arecord -l                   # note the card,device -> e.g. "plughw:1,0"
arecord -D plughw:1,0 -f S16_LE -r 16000 -d 3 /tmp/t.wav && aplay /tmp/t.wav
```

Set that (e.g. `plughw:1,0`) as `audio_device` in the wizard/config. Note the
card *number* can change across reboots when multiple USB audio devices are
attached — `auto` avoids that. Leave it blank to disable audio-based detection.

## 5. Run at boot (optional)

A `systemd` unit keeps admuffs running:

```ini
# /etc/systemd/system/admuffs.service
[Unit]
Description=admuffs - auto-mute TV commercials
After=network-online.target sound.target

[Service]
ExecStart=/usr/local/bin/admuffs --run
Restart=on-failure
User=pi

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable --now admuffs
journalctl -u admuffs -f
```
