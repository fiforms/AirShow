# Integrating AirShow with kiosk and signage setups

AirShow can tell your own scripts when someone starts and stops casting, so a
kiosk or signage setup can hand the screen to AirShow only while it's in use.
For example, it can hide a fullscreen browser during a session. AirShow
doesn't manage anything else on the display. It only runs your commands and
reports when it's ready.

It provides three things for this:

- [Session hooks](#session-hooks): `--on-session-start` and `--on-session-end`
- [Readiness notification](#readiness-systemd-typenotify) for systemd
  `Type=notify` units
- [A fixed window identity](#window-identity) for compositor window rules

## Session hooks

    airshow --on-session-start 'COMMAND' --on-session-end 'COMMAND'

Each option takes one shell command line. It runs through `/bin/sh -c`
(`cmd.exe /c` on Windows), so pipes, redirects and `$VARIABLES` work. Quote
it as a single argument.

### When they run

| Hook    | Runs when |
|---------|-----------|
| `start` | A sender connects (AirPlay, Google Cast, Miracast, DLNA or the AirShow sender app), **or** AirShow shows an approval prompt for a new sender. The prompt must be visible to be answered, so it counts as the start of a session. |
| `end`   | **5 seconds** after the last sender disconnects and no prompt is waiting. If a sender reconnects within those 5 seconds, the session continues and neither hook runs. This covers AirPlay reconnects and DLNA controllers switching tracks. |
| `end`   | When AirShow exits during a session: Ctrl+C, `SIGTERM` (e.g. `systemctl stop`), `SIGHUP`, or closing the window. |

Hooks always alternate, `start` then `end`, and never two of the same kind
in a row.

`end` does **not** run if AirShow is killed with `SIGKILL` or crashes. If
your script hid something in its `start` hook, also restore it when AirShow's
process or unit goes away, e.g. with `ExecStopPost=` in a systemd unit. See
the [example](#example-hide-a-kiosk-browser-while-casting).

A denied approval prompt ends the session like a disconnect: `end` runs 5
seconds later.

### Environment

The command inherits AirShow's environment, plus:

| Variable           | Value |
|--------------------|-------|
| `AIRSHOW_SESSION`  | `start` or `end` |
| `AIRSHOW_DEVICE`   | Sender's name as it reported it, e.g. `Jane's iPhone`; may be empty. The same value is passed to `end`. |
| `AIRSHOW_PROTOCOL` | `AirPlay`, `Cast`, `Miracast`, `DLNA` or `AirShow` |

Treat `AIRSHOW_DEVICE` as untrusted text: any sender on the network chooses
its own name. Quote it in shell (`"$AIRSHOW_DEVICE"`) and never `eval` it.

### How they run

- Hooks run **detached**. AirShow doesn't wait for them, look at their exit
  status, or stop them. A slow hook doesn't delay the stream.
- They run as AirShow's user, in AirShow's working directory. Their output
  goes to AirShow's stdout and stderr, which under systemd means the journal.
- Because they're detached, an `end` hook can still be running when the next
  `start` hook begins. Make hooks idempotent and fast, or serialize them
  yourself (e.g. with `flock`).
- AirShow logs each hook it runs, e.g. `Session start hook: …`.

## Readiness (systemd `Type=notify`)

When AirShow runs as a systemd service with `Type=notify`, it sends
`READY=1` after its receivers are listening **and** its first frame has been
presented on screen.

The window that maps last is normally on top, so this matters on a kiosk.
Start anything that should cover AirShow (e.g. a browser) in a unit ordered
`After=` AirShow's unit. It then starts only once AirShow's window is up.
Outside systemd (no `NOTIFY_SOCKET`), nothing is sent.

Without systemd, the closest signal is the log line
`WebInterface: listening on port 7401`, or a successful connection to TCP
port 7401. These come shortly before the first frame, so allow a moment
before starting the browser.

## Window identity

- On Wayland, AirShow's `app_id` is `airshow`, matching its
  `airshow.desktop` file.
- The window opens fullscreen; pass `--windowed` for a normal window.
- AirShow never raises itself or takes focus when a session starts. Your hook
  decides what becomes visible.

With labwc, for example, you could use a window rule to keep AirShow below
other windows instead of stopping the browser. Check your labwc version's
documentation for the actions it supports.

## Example: hide a kiosk browser while casting

This example is not installed by the package. Adapt it to your own setup. It
assumes a Wayland session, e.g. labwc on Raspberry Pi OS, where both programs
run as systemd **user** services. The browser sits on top of AirShow and is
stopped while someone is casting.

`~/.config/systemd/user/airshow.service`:

```ini
[Unit]
Description=AirShow receiver

[Service]
Type=notify
NotifyAccess=main
ExecStart=/usr/bin/airshow \
    --on-session-start 'systemctl --user stop kiosk-browser.service' \
    --on-session-end   'systemctl --user start --no-block kiosk-browser.service'
# Bring the browser back if AirShow dies mid-session (no end hook then).
ExecStopPost=-/usr/bin/systemctl --user start --no-block kiosk-browser.service
Restart=on-failure
```

`~/.config/systemd/user/kiosk-browser.service`:

```ini
[Unit]
Description=Kiosk browser
Wants=airshow.service
# Start only once AirShow's window is on screen, so the browser maps on top.
After=airshow.service

[Service]
ExecStart=/usr/bin/chromium --kiosk --ozone-platform=wayland --noerrdialogs \
    --disable-infobars --no-first-run https://example.org/signage
Restart=on-failure
```

Add these lines to the compositor's autostart (`~/.config/labwc/autostart`).
User services don't inherit the Wayland session's environment, so it's passed
to systemd first:

```sh
dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP &&
    systemctl --user start kiosk-browser.service &
```

The hooks in this example aren't idempotent in every case. For example,
`systemctl --user stop kiosk-browser.service` in a separate terminal would be
undone by the next `end` hook. If you need more control, point the hooks at
your own script, which can read `AIRSHOW_SESSION`, `AIRSHOW_DEVICE` and
`AIRSHOW_PROTOCOL`.

## Raspberry Pi notes

- The Raspberry Pi 5 has no H.264 hardware decoder, so AirShow decodes on the
  CPU (`avdec_h264`). 1080p works, but keep the Pi cool.
- Miracast discovery from Windows (Win+K) uses `wpa_supplicant`'s Wi-Fi
  Direct support, which can disturb the Pi's Wi-Fi connection. If you don't
  need it, pass `--no-beacon`. See *Miracast discovery (Linux)* in the main
  README.
- Audio goes to the session's default output, which is PipeWire on Raspberry
  Pi OS.
