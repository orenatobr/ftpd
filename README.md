# ftpd (fork with 3DS internal NAND access)

FTP Server for 3DS/Switch/Linux.

> **This is a fork of [mtheall/ftpd](https://github.com/mtheall/ftpd).**
> All the credit for the FTP server goes to the original authors: Michael Theall
> ([mtheall](https://github.com/mtheall)), Dave Murphy and TuxSH, plus the
> contributors of the upstream project. This fork is released under the same
> license (GPL-3.0) and only adds the feature described below.

## What this fork adds

On 3DS, the upstream server only exposes the SD card. This fork also exposes
the console's internal memory (NAND). The FTP root `/` lists the available
storage areas:

- `/sdmc`: SD card (formerly `/`)
- `/ctrnand`: internal CTRNAND
- `/twln`: internal TWLNAND
- `/twlp`: TWL photo partition

Notes:

- Breaking change: clients that used `/` for the SD card must now use `/sdmc`.
- The CIA requests the filesystem access rights needed for the NAND. A `.3dsx`
  only sees the NAND areas if the launcher grants those rights; otherwise they
  are hidden and the SD card keeps working.
- Writing to NAND can brick the console, so back it up first.
- Not tested on real hardware yet.
- The Switch version is not supported in this fork.

Downloads (`.3dsx` and `.cia`) are on the
[releases page](https://github.com/orenatobr/ftpd/releases/latest).

## Features

- Appears to work well with a variety of clients.
- Supports multiple simultaneous clients. The 3DS itself only appears to support enough sockets to perform 4-5 simultaneous data transfers, so it will help if you limit your FTP client to this many parallel requests.
- Cutting-edge [graphics](#dear-imgui).
- MODE Z

- Exit on NDS/3DS with START button
- Exit on Switch with PLUS button

- Toggle backlight on NDS/3DS with SELECT button
- Toggle backlight on Switch with MINUS button

- Emulation of a /dev/zero (/devZero) device for network performance testing
  - Example retrieve `curl ftp://192.168.1.115:5000/devZero -o /dev/null`
  - Example send `curl -T /dev/zero ftp://192.168.1.115:5000/devZero`

## Dear ImGui

ftpd uses [Dear ImGui](https://github.com/ocornut/imgui) as its graphical backend.

Standard Dear ImGui controller inputs are supported.

- A
  - Activate/Open/Toggle
  - Tweak value with D-Pad (+ L/R to tweak slower/faster)
- B
  - Cancel/Close/Exit
- X
  - Edit text / on-screen keyboard
- Y
  - Tap: Toggle menu
  - Hold + L/R: Focus windows
- Left Stick
  - Scroll
  - Move window (when holding Y)
- D-Pad
  - Move
  - Tweak values (when activated with A)
  - Resize window (when holding Y)

## Upstream Builds (without NAND access)

NDS: https://mtheall.com/~mtheall/ftpd.nds

CIA: https://mtheall.com/~mtheall/ftpd.cia

3DSX: https://mtheall.com/~mtheall/ftpd.3dsx

NRO: https://mtheall.com/~mtheall/ftpd.nro

CIA QR Code

![ftpd.cia](https://github.com/mtheall/ftpd/raw/master/ftpd-qr.png)

## Classic Builds

Classic builds use a console instead of Dear ImGui.

CIA: https://mtheall.com/~mtheall/ftpd-classic.cia

3DSX: https://mtheall.com/~mtheall/ftpd-classic.3dsx

NRO: https://mtheall.com/~mtheall/ftpd-classic.nro

CIA QR Code

![ftpd-classic.cia](https://github.com/mtheall/ftpd/raw/master/ftpd-classic-qr.png)

## Build and install

You must set up the [development environment](https://devkitpro.org/wiki/Getting_Started).

### NDS

The following pacman packages are required to build `nds/ftpd.nds`:

	devkitARM
	dswifi
	libfat-nds
	libnds

They are available as part of the `nds-dev` meta-package.

### 3DSX

The following pacman packages are required to build `3ds/ftpd.3dsx`:

    3dstools
    devkitARM
    libctru

They are available as part of the `3ds-dev` meta-package.

Build `3ds/ftpd.3dsx`:

    make 3dsx

### NRO

The following pacman packages are required to build `switch/ftpd.nro`:

    devkitA64
    libnx
    switch-tools
    switch-libzstd

The first three are available as part of the `switch-dev` meta-package. Additionally, ImageMagick is required for converting assets.

Build `switch/ftpd.nro`:

    make nro

## Supported Commands

- ABOR
- ALLO (no-op)
- APPE
- CDUP
- CWD
- DELE
- FEAT
- HELP
- LIST
- MDTM
- MKD
- MLSD
- MLST
- MODE
- NLST
- NOOP
- OPTS
- PASS (no-op)
- PASV
- PORT
- PWD
- QUIT
- REST
- RETR
- RMD
- RNFR
- RNTO
- SITE
- SIZE
- STAT
- STOR
- STRU (no-op)
- SYST
- TYPE (no-op)
- USER (no-op)
- XCUP
- XCWD
- XMKD
- XPWD
- XRMD

## Planned Commands

- STOU

## SITE commands

| Command              |                          |
|----------------------|--------------------------|
| SITE HELP            | Show help                |
| SITE USER <NAME>     | Set username             |
| SITE PASS <PASS>     | Set password             |
| SITE PORT <PORT>     | Set port                 |
| SITE HOST <HOSTNAME> | Set hostname<sup>1</sup> |
| SITE DEFLATE [0-9]   | Set deflate level        |
| SITE MTIME [0\|1]    | Set getMTime<sup>2</sup> |
| SITE SAVE            | Save config              |

<sup>1</sup>mDNS hostname not available on NDS

<sup>2</sup>getMTime only on 3DS. Enabling will give timestamps at the expense of slow listings.
