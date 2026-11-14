# Flashing the hybrid image

This guide covers the ready-to-flash image from the
[Releases](https://github.com/klarnet2009/yeelight-ceiling3-firmware-patch/releases) page:
`yeelink.light.ceiling3-hybrid-2.0.2_0048.bin`. Russian version: [FLASHING_RU.md](FLASHING_RU.md).

> **Read this first.**
> - The image was tested on **one** lamp: YLXD17YL, model `yeelink.light.ceiling3`, MT7697N module, stock firmware
>   `2.0.2_0048`. Other board revisions and other models are untested.
> - It is unofficial firmware. If it does not boot on your lamp, the only way back is a UART connection on the
>   board. Nobody here can help you recover a bricked lamp remotely.
> - The image is the Yeelight firmware `2.0.2_0048` with about 2.5 KB of changes. It is not affiliated with or
>   endorsed by Yeelight, Xiaomi or MediaTek.

## What you need

- The lamp set up in Mi Home, on your Wi-Fi, and **already on firmware `2.0.2_0048`** (update it in Mi Home first).
- A computer in the same LAN as the lamp, with **TCP port 80 free and reachable from the lamp** (allow it in the
  firewall; on Linux/macOS binding port 80 needs root). The lamp downloads the image from this computer and can
  only use port 80.
- Python 3 and three packages:

  ```
  pip install cryptography requests qrcode
  ```

- This repository (clone it or download the ZIP) and the image file from the release, placed in the repository
  root.

## 1. Check the downloaded file

Compare the checksum with the one in the release notes:

```
certutil -hashfile yeelink.light.ceiling3-hybrid-2.0.2_0048.bin SHA256      (Windows)
sha256sum yeelink.light.ceiling3-hybrid-2.0.2_0048.bin                      (Linux/macOS)
```

`miio_ota.py` also refuses any file that fails the container checks (header SHA-1, segment SHA-1, CRC, LZMA).

## 2. Get the lamp's token

```
python xiaomi_qr_token.py de
```

Replace `de` with your Mi Home region (`cn`, `de`, `us`, `ru`, `sg`, `i2`, ...). The script writes a login QR code
to `xiaomi_login_qr.png`; scan it with the Mi Home app within about 5 minutes. It then lists your devices and writes
the token of the `yeelink.light.ceiling3` lamp to `token.txt`. If you have several ceiling3 lamps, set `LAMP_IP` at
the top of the script to the address of the one you want.

`token.txt` and the QR image are secrets: do not publish them (both are in `.gitignore`).

Check that this is the lamp the image was made for (replace the address with your lamp's):

```
python miio_ota.py check --ip=192.168.0.20
```

It compares what the lamp reports about itself with the tested configuration: model `yeelink.light.ceiling3`,
firmware `2.0.2_0048`, chip `MTK7697`, Wi-Fi core firmware `20190110183129`. All four lines must say `OK`. Stop if
any line says `MISMATCH`.

The `flash` command runs the same check and refuses to send the image to any other device or firmware version. It
can be overridden with `--force`; do that only if you can recover the lamp over UART. The check cannot tell board
revisions apart if they report the same values.

## 3. First flash: through the Xiaomi cloud

The stock firmware rejects an update command that arrives over the LAN, so the first flash has to be relayed
through your Xiaomi account. The image itself is still downloaded from your computer:

```
python miio_ota.py flash yeelink.light.ceiling3-hybrid-2.0.2_0048.bin --ip=192.168.0.20 --cloud=de
```

Scan the QR code again when asked, then press Enter to send the command. The script prints the progress:
`downloading` with a percentage, `installed`, a pause of about a minute while the lamp reboots, and finally
`lamp rebooted; back online`. The light turns off and on once during the reboot. Do not cut the power while it
installs.

If the script reports `NO REBOOT within 10 min`, nothing was installed: the lamp could not download the file. Check
that port 80 is open and that no other program uses it.

## 4. Later flashes: local

Once the patched firmware is running, the update command is accepted over the LAN and no cloud login is needed:

```
python miio_ota.py flash <image.bin> --ip=192.168.0.20 --yes
```

## 5. Check the result

The version string stays `2.0.2_0048`, so check the behaviour:

- In day mode, brightness 1 is now a faint night-light (moon LEDs) and brightness 10 is the dimmest level of the main
  LEDs; the app keeps showing day mode.
- The event log is readable:

  ```
  python tools/read_bblog.py --ip=192.168.0.20
  ```

  It should show a boot record and the network going to "cloud".

## Going back to stock

Flash the original Xiaomi OTA file `yeelink.light.ceiling3-stock-2.0.2_0048.bin`
(MD5 `13b7471a0f84e3cfbac78c116fae7a1c`) the same way as in step 4. The file is not distributed here; README_RU.md
lists the sources that describe how to request it from the Xiaomi cloud.

## What changes for daily use

- Day-mode brightness 1-9 uses the moon LEDs, 10-100 the main LEDs. With warm white the step between 9 and 10 is not
  visible; with neutral and cold white brightness 10 is clearly brighter than 9.
- If the lamp has no network for 5 minutes it reboots itself (the light blinks once). If the network stays away,
  the next reboots come after 30 minutes and then every 6 hours.
- The night-time schedule, power-on behaviour, remote control and Mi Home integration are unchanged.
