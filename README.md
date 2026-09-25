# turzx

Userspace Linux driver and CLI for **TURZX / Turing USB smart screens**.

This panel is **not** a USB monitor in the DisplayLink / DRM sense. It enumerates as a vendor-class bulk device (`1cbe:00xx`) and only accepts compressed frames (JPEG/PNG stills, H.264 chunks) wrapped in DES-encrypted 512-byte command packets. A kernel KMS driver cannot scan out to it. This program is the practical driver: it talks the vendor protocol and can put any JPEG, PNG, GIF, or MP4 on the panel, or mirror an X11/XWayland screen onto it.

Tested against the 8.0" panel on this machine:

| Field | Value |
| --- | --- |
| USB ID | `1cbe:0080` |
| Product | `TURZX1.0` |
| Native framebuffer | 800×1280 portrait |
| Logical landscape | 1280×800 |
| Endpoints | bulk OUT `0x01`, bulk IN `0x81` (512 B) |

Other TURZX USB models (`2.8"`, `4.6"`, `5.2"`, `8.8"`, `9.2"`, `12.3"`) use the same protocol and are auto-detected by product ID.

Protocol reverse-engineering credit: [phstudy/turing-smart-screen-cli](https://github.com/phstudy/turing-smart-screen-cli) and [mathoudebine/turing-smart-screen-python](https://github.com/mathoudebine/turing-smart-screen-python).

## Build

```
sudo dnf install gcc make libusb1-devel ffmpeg   # Fedora; already present here
make
make test      # DES-CBC known-answer test
make install   # copies to ~/.local/bin (and ~/.grok/bin if present)
```

## USB permissions

Without a udev rule the device node is root-only (`/dev/bus/usb/...`).

```
sudo cp udev/99-turzx.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger -s usb -a idVendor=1cbe
```

Then `ls -l /dev/bus/usb/001/*` should show the TURZX node as `crw-rw-rw-`. `sudo turzx` will not work: root’s PATH does not include `~/.local/bin`. Use `turzx` as your user after the udev rule, or `sudo /home/gabe/Documents/c-stuff/turzx/turzx info`.

## systemd user service

Same pattern as `tryx.service`: runs as your user on login, restarts if the panel is missing.

```
mkdir -p ~/.config/turzx ~/.config/systemd/user
cp systemd/turzx.env ~/.config/turzx/turzx.env
cp systemd/turzx.service ~/.config/systemd/user/turzx.service
# edit ~/.config/turzx/turzx.env  →  set TURZX_ARGS
systemctl --user daemon-reload
systemctl --user enable --now turzx.service
systemctl --user status turzx.service
```

Change the image/video later with:

```
nano ~/.config/turzx/turzx.env
systemctl --user restart turzx.service
```

`WantedBy=default.target` starts it at login. To also start it when the USB cable is plugged in, reinstall `udev/99-turzx.rules` (it sets `SYSTEMD_USER_WANTS=turzx.service`). To start at boot before login: `loginctl enable-linger $USER`.

## Usage

```
turzx info
turzx brightness 40
turzx test                          # SMPTE bars
turzx show photo.jpg                # jpeg / png / webp / bmp
turzx show anim.gif --loop
turzx show clip.mp4 --loop          # re-encoded to H.264, hardware-decoded on panel
turzx show photo.png --portrait     # native 800x1280
turzx show photo.png --fit cover
turzx mirror --fps 10               # X11/XWayland capture → JPEG frames
turzx clear
turzx stop
```

`show` picks still / GIF / video from the file extension. Stills are letterboxed into the panel and sent as JPEG (the firmware rejects payloads over 1 MiB; PNG is used by the vendor app but JPEG is the reliable path). Video is transcoded with ffmpeg to baseline H.264 at the panel's native portrait size, then streamed with command 121.

Default orientation is **landscape**: content is composed at 1280×800 and rotated 90° clockwise so it is upright when the 8" panel is mounted on its side.

## Treating it as a monitor

True multi-monitor desktop extension needs a DRM connector. This hardware has none. Closest options:

1. **`turzx mirror`** — capture `$DISPLAY` and push JPEG frames (~10 fps is comfortable on USB 2.0).
2. Open a window on your real desktop and `mirror` that region (use `--display`).
3. A virtual DRM device (evdi) plus a daemon that JPEG-encodes each frame is possible later, but it is still this same USB path underneath.

USB 2.0 bulk cannot carry uncompressed 800×1280@60. JPEG stills at 10–15 fps or H.264 at 24–25 fps are the real limits.

## Protocol (short)

Every command is a 500-byte header (`cmd`, magic `1A 6D`, ms-since-midnight) encrypted with **DES-CBC**, key and IV `slv3tuzx`, zero-padded to 504 bytes, placed in a 512-byte packet ending with `A1 1A`. Image/video bytes follow the header unencrypted on the bulk OUT pipe.

| ID | Meaning |
| --- | --- |
| 10 | sync |
| 11 | restart |
| 14 | brightness (0–102) |
| 15 | frame rate |
| 101 | JPEG still |
| 102 | PNG still |
| 121 | H.264 chunk |
| 122 | stream queue depth |
| 123 | stop stream |
