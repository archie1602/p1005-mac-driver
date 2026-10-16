# P1005 firmware

The printer needs HP's firmware file `HP1005.acl` (version 20080505). It
belongs to HP, so it is not in this repository. Get it with

    make firmware

which runs `tools/fetch-firmware.sh` and saves the file here as
`firmware/HP1005.acl`. The script downloads HP's official Mac driver package
(`HewlettPackardPrinterDrivers.dmg`, version 5.1.1, 584 MB) from Apple, or from
HP if that fails, and takes the file out of

    Library/Printers/hp/laserjet/hplaserjetzjs.bundle/Contents/Resources/HP1005.acl

If you downloaded the package yourself, pass it along:
`make firmware HP_DMG=/path/to/HewlettPackardPrinterDrivers.dmg`.

Expected checksums (SHA256):

    HewlettPackardPrinterDrivers.dmg  523836b630431bc39b0170a17099099d6f821ef62ff29e6ec64ebb69b9954133
    HP1005.acl                        6c45cc78aa81cf969d0a5bd819828bdfea95930b03b249365af769de16c4ea3d

The file is an 8 digit version stamp followed by an ARM ELF image. The printer
keeps it only until it is switched off, so the driver sends it again whenever
the printer reports no `FWVER:` in its device ID.

You don't need this folder for the normal package (`make pkg`), which fetches
the firmware during installation. It is only used by `make pkg-offline` and
`install.sh`. Files ending in `.acl` here are ignored by git.
