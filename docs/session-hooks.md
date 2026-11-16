# Integrating AirShow with kiosk and signage setups

AirShow can tell your own scripts when someone starts and stops casting, so a
kiosk or signage setup can hand the screen to AirShow only while it's in use.
For example, it can hide a fullscreen browser during a session. AirShow
doesn't manage anything else on the display. It only runs your commands and
reports when it's ready.

It provides these things for this:

- [Session hooks](#session-hooks): `--on-session-start` and `--on-session-end`
- [Readiness notification](#readiness-systemd-typenotify) for systemd
  `Type=notify` units
- [A fixed window identity](#window-identity) for compositor window rules
- [Control over who may cast](#who-may-cast): `--approval`, and
  `--list-remembered` / `--forget-remembered`
- [Quit keys](#leaving-airshow-with-a-key): `--quit-keys`, to leave AirShow
  with a remote control button

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

## Who may cast

New senders get an Allow/Deny prompt, and approved senders are remembered:
their next connection starts without one. `--approval` chooses, for one run,
what happens to senders that aren't remembered:

| `--approval` | Senders that aren't remembered |
|---|---|
| `prompt` | Asked with the approval prompt; remembered if allowed |
| `known-only` | Rejected without a prompt (logged as `SecurityManager: rejecting ...`) |
| not given | The `requireApproval` setting decides (prompt by default) |

With `known-only` no prompt appears, so no `start` hook runs for a rejected
sender: the screen stays with whatever the kiosk shows. A remembered sender
starts a session as usual.

Remembered senders are listed and forgotten from the command line, without a
display and while AirShow runs. They act on the settings of the user who runs
them, so for a kiosk unit run them as that user:

```bash
airshow --list-remembered
airshow --forget-remembered "Jane's iPhone"   # by device name or identifier
airshow --forget-remembered all
sudo runuser -u kiosk -- airshow --list-remembered
```

`--list-remembered` prints one sender per line: its identifier, then its name
and protocol as they were when it was approved (empty for senders remembered
by older versions).

## Leaving AirShow with a key

`--quit-keys` makes AirShow exit (status 0, as on `SIGTERM`) when one of the
given keys is pressed while its window has focus. A kiosk can use it to leave
an AirShow mode with a remote control button:

```bash
airshow --approval prompt --quit-keys 172,158
```

Keys are Linux key codes, as `evtest` shows them, or Qt key names (`Escape`,
`F11`, `HomePage`), separated by commas. On a common 2.4 GHz remote that acts
as a USB keyboard, Home is `172` (`KEY_HOMEPAGE`) and Back is `158`
(`KEY_BACK`). Remotes like these often have a mouse mode too, in which their
OK button is a mouse click instead of a key.

- Don't use the approval prompt's keys (Enter, Return, Esc, Space, the arrow
  keys, Tab, Y, N): a quit key quits even while the prompt is open.
- Avoid the Power key (`116`): systemd-logind or the kiosk may already act on
  it.
- Linux key codes are matched on Linux only; key names work everywhere.

### Two kiosk modes

Together these allow a kiosk with an attended AirShow mode and an unattended
background mode, sharing the same remembered senders:

- **AirShow mode**, opened from the kiosk's menu: AirShow is shown full
  screen, new senders get the approval prompt (answered with the remote), and
  a remote button returns to the kiosk:

  ```bash
  airshow --approval prompt --quit-keys 172,158
  ```

  The kiosk starts the mode, and switches back when AirShow exits.

- **Background mode**, while the kiosk shows its own content: AirShow runs
  behind it, admits only remembered senders, and the session hooks bring it
  to the front while one is casting:

  ```bash
  airshow --approval known-only \
          --on-session-start 'my-kiosk show-airshow' \
          --on-session-end 'my-kiosk show-browser'
  ```

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

- AirShow decodes H.264 on the CPU (`avdec_h264`). The Raspberry Pi 5 has no
  H.264 hardware decoder, and the Pi 4's (V4L2) is slower here, because every
  frame is copied to the screen on the CPU. A Pi 4 can't keep up with 1080p
  Miracast, so on ARM boards AirShow offers Miracast senders at most 720p
  (`maxResolution` under `[miracast]` in the settings file changes it).
- Miracast discovery from Windows (Win+K) uses `wpa_supplicant`'s Wi-Fi
  Direct support. The Pi's single radio leaves your network's channel while
  it listens for Wi-Fi Direct scans (200 ms of every 2 s, paused while
  someone is casting). If you don't need Win+K or Android Miracast, pass
  `--no-beacon`. See *Miracast discovery (Linux)* in the main README.
- Turn Wi-Fi power saving off on a kiosk that is always plugged in: with it
  on, streams arrive in bursts with lost packets. See *Android (Wi-Fi
  Direct)* in the main README.
- Audio goes to the session's default output, which is PipeWire on Raspberry
  Pi OS.
