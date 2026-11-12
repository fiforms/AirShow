# AirShow kiosk mode

These steps set up a display that normally shows a fullscreen web page and
switches to AirShow while someone is casting to it. They're written for
Raspberry Pi OS / Debian 13 (trixie) with the labwc Wayland compositor, and
should work with any Wayland compositor that has an autostart file.

## How it works

AirShow has to keep running the whole time: senders can only find it while
it's running. So both programs run fullscreen, with AirShow underneath:

| Unit (systemd `--user`)   | Runs                        | Stacking |
|---------------------------|-----------------------------|----------|
| `airshow.service`         | `airshow` with session hooks | bottom   |
| `airshow-browser.service` | the kiosk browser (Chromium) | top      |

When a sender connects, or AirShow shows an approval prompt, AirShow runs
its `--on-session-start` hook (`airshow-kiosk session-start`). That stops the
browser, so AirShow is visible. Five seconds after the sender disconnects,
`--on-session-end` (`airshow-kiosk session-end`) starts the browser again.

Order matters, because the window that maps last ends up on top.
`airshow.service` is `Type=notify`: AirShow tells systemd it's ready once its
first frame is on screen, and `airshow-browser.service` starts only after
that. If AirShow restarts after a crash, it restarts the browser so the
browser is on top again.

## Setup

1. Install the package. Dependencies are pulled in automatically:

       sudo apt install ./airshow_*_arm64.deb

2. Turn off whatever starts your browser now, such as a `chromium` line in
   `~/.config/labwc/autostart` or `/etc/xdg/labwc/autostart`, or an existing
   systemd unit. From now on the browser runs as `airshow-browser.service`.

3. Set the page to show, along with any other options, in
   `~/.config/airshow/kiosk.conf`. Use `/etc/airshow/kiosk.conf` instead to
   apply them to every user:

       KIOSK_URL="https://example.org/signage"
       # or replace the browser command entirely:
       #BROWSER_COMMAND="chromium --kiosk --ozone-platform=wayland --incognito https://example.org"
       # extra AirShow options:
       #AIRSHOW_ARGS="--no-beacon"

4. Add this line to `~/.config/labwc/autostart`:

       airshow-kiosk start &

   This copies `WAYLAND_DISPLAY` and related variables into the systemd user
   manager, then starts both units. The desktop needs to log in
   automatically, which is the usual setting for a kiosk (`raspi-config` →
   System Options → Boot / Auto Login).

5. Log out and back in, or reboot.

## Day to day

    airshow-kiosk status                 # state of both units
    journalctl --user -u airshow -f      # AirShow's log
    airshow-kiosk stop                   # stop both (until the next login)
    systemctl --user restart airshow     # restart AirShow; the browser comes back on top

## Notes for Raspberry Pi

- **Video decoding.** The Raspberry Pi 5 has no H.264 hardware decoder, so
  AirShow decodes on the CPU with `avdec_h264`. 1080p mirroring works, but
  keep the Pi cool. The Raspberry Pi 4 has hardware H.264 decoding, which
  GStreamer's `decodebin` uses when it's available.
- **Miracast / Windows "Cast" (Win+K)** works through `wpa_supplicant`'s
  Wi-Fi Direct support, and needs AirShow's user to have access to
  `wpa_supplicant`'s D-Bus API. On a Pi whose Wi-Fi is managed by
  NetworkManager, this can disturb the Wi-Fi connection. If you don't need
  Miracast, set `AIRSHOW_ARGS="--no-beacon"` in `kiosk.conf`.
- **Audio** plays through the session's default output, which is PipeWire
  on Raspberry Pi OS. Choose HDMI or the headphone jack in the desktop's
  volume menu, or with `wpctl`.
- **Firewall.** Raspberry Pi OS doesn't run one by default. If you add one,
  open at least UDP 5353 (mDNS), UDP 1900 (SSDP), and TCP 7000 (AirPlay),
  8009 (Google Cast), 7400 (AirShow) and 7401 (web interface). AirPlay and
  Miracast also use ports chosen at run time, so it's simplest to trust the
  local network.

## Using the hooks without the kiosk units

`--on-session-start` and `--on-session-end` are ordinary AirShow options.
Each takes a shell command, and the command gets `AIRSHOW_SESSION`
(`start` or `end`), `AIRSHOW_DEVICE` and `AIRSHOW_PROTOCOL` in its
environment:

    airshow --on-session-start 'notify-send "Casting from $AIRSHOW_DEVICE"' \
            --on-session-end   'logger "AirShow session ended"'
