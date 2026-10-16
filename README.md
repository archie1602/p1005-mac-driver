# p1005-mac-driver

A driver that makes the HP LaserJet P1005 print again on current macOS. It runs
natively on Apple Silicon (and on Intel Macs), so it does not need Rosetta.

## Why this exists

I have had this printer for well over ten years and it still prints perfectly.
After I updated my MacBook it stopped working. HP's last Mac driver is from 2021
and only runs on Intel, and its installer now refuses to run with the message
"This update requires macOS version 15.0 or earlier". The generic driver macOS
picks instead does not speak the printer's language, so nothing came out.
Apple is also phasing out Rosetta, so patching
HP's old installer would only have worked for a little while.

So I wrote a small native driver instead. To be honest about it: most of the
work was done by an AI coding agent (Claude Code). It went through the open
source foo2zjs driver, compared every page header against what HP's original
driver sends, and wrote the code and the tests. I steered it and checked the
results on my own printer.

I am sharing it in case someone else has the same printer sitting on a shelf.

## What you need

* HP LaserJet P1005 connected over USB
* A Mac with macOS 12 or newer, Apple Silicon or Intel
* An internet connection while installing (see the firmware section below)

I only have a P1005, so that is the only printer this was tested with (on
macOS 26, Apple Silicon). The P1006, P1007 and P1008 use the same protocol,
but I could not try them and the installer only sets up a P1005.

## Installing

There is a [step by step guide with screenshots](docs/install-guide.md) from
my own install. The short version:

1. Download `P1005-Driver-1.0.0.pkg` from the
   [Releases page](https://github.com/archie1602/p1005-mac-driver/releases/latest).
2. Plug in the printer, switch it on and double click the package.
3. macOS will refuse to open it the first time, because I don't pay Apple for
   a developer certificate. Open System Settings > Privacy & Security, scroll
   down, click Open Anyway and confirm.
4. Click through the installer. It asks for your password and downloads the
   firmware, which takes a minute or two on a normal connection.
5. Print from any app with Cmd+P and choose HP LaserJet P1005.

If the printer was not connected during the install, add it later in System
Settings > Printers & Scanners > Add Printer. macOS picks this driver on its own.

If you already had a P1005 set up, the installer switches it to this driver and
keeps a copy of the old one in `/Library/Printers/p1005-native/backup/`.

## The firmware

The P1005 forgets its firmware every time it is switched off, and the driver
has to send it again before the first job. That firmware file belongs to HP, so
it is not in this repository or in the release package.

During installation the package downloads HP's official driver package for Mac
(`HewlettPackardPrinterDrivers.dmg`, 584 MB) from Apple's servers, or from HP's
if Apple's are down. It checks the SHA256 of the download, takes the one
firmware file it needs (217 KB), checks that too, and deletes the download. If
HP's old driver is still installed on the Mac, it uses the file from there and
skips the download. If anything goes wrong, the installation stops before
anything is changed. The installer log (Window > Installer Log) shows why.

You need about 600 MB of free disk space for a moment while this happens.

## Removing it

```sh
sudo /Library/Printers/p1005-native/uninstall.sh
```

This puts back the previous driver on your printer queue if there was one,
otherwise it removes the queue, and then deletes everything the package
installed.

## How it works

```
app > PDF > cgpdftoraster (part of macOS) > rastertoxqx (this project) > USB > printer
```

macOS itself turns the page into a black and white bitmap. `src/rastertoxqx.c`
is a CUPS filter that compresses each page with JBIG and wraps it in HP's XQX
format, the same way foo2xqx from [foo2zjs](https://github.com/OpenPrinting/foo2zjs)
does. A copy of the foo2zjs and JBIG-KIT sources it builds on is in
`vendor/foo2zjs`.

Before each job the filter asks the printer for its device ID. If the answer
has no firmware version in it, the filter sends the firmware first and waits
until the printer reports that it is running. If you press Print before
switching the printer on, it waits for up to ten minutes.

Paper sizes, margins and media types are taken from HP's own PPD, and the page
headers were checked against what HP's original driver produces for every paper
size and media type (`tests/golden_hp_matrix.txt`).

## Print options

You find these under Printer Options in the print dialog.

| Option | Values |
|---|---|
| Resolution | FastRes 1200 (default), 600 dpi |
| Print Density | 1 to 5 (default 3) |
| EconoMode | Off, On |
| Media Type | HP's list (Plain, Bond, Labels, Envelope and so on) |
| Firmware Upload | Automatic (default), Always, Never |

The P1005 only prints black. Color documents come out in shades of gray made
of fine dots.

## Building from source

You need the Xcode Command Line Tools (`xcode-select --install`), nothing else.

```sh
make                # filter, PPD and test tools go into build/
make test           # offline tests, no printer and no HP files needed
make pkg            # build/P1005-Driver-1.0.0.pkg, same as the release
sudo ./install.sh   # or install straight from build/ without a package
sudo ./uninstall.sh # undo install.sh and restore the previous driver
```

`install.sh` gets the firmware the same way the package does. If you install on
several Macs and don't want the big download each time:

```sh
make firmware       # saves HP's firmware to firmware/HP1005.acl
make pkg-offline    # build/P1005-Driver-1.0.0-offline.pkg with the firmware inside
```

`make firmware HP_DMG=/path/to/HewlettPackardPrinterDrivers.dmg` uses a copy of
HP's package you already downloaded. Please keep the offline package to
yourself, since it contains HP's file.

Everything gets installed into these places:

| Path | What |
|---|---|
| `/Library/Printers/p1005-native/` | the filter, the firmware, `uninstall.sh` and `fetch-firmware.sh` |
| `/Library/Printers/PPDs/Contents/Resources/HP LaserJet P1005 native.gz` | the printer description (PPD) |

## Tests

`make test` takes about 40 seconds and checks four things: the firmware is
sent byte for byte the same way foo2zjs does it, the firmware logic behaves
right against a fake printer (`tools/fakeprinter.c`) in six situations, the page
headers match HP's driver for 23 paper sizes and 22 media types, and the JBIG
compression gives back exactly the input bitmap.

## Troubleshooting

The job says it printed but nothing came out: switch the printer off and on and
print again. `lpstat -p HP_LaserJet_P1005 -l` shows the last message from the
driver. Setting Firmware Upload to Always for one job forces a fresh upload.

The printer hangs with the orange light on: switch it off and on. Waiting jobs
continue by themselves.

For detailed logs run `sudo cupsctl --debug-logging`, print something, then look
at `/private/var/log/cups/error_log` (search for rastertoxqx). Turn it off again
with `sudo cupsctl --no-debug-logging`.

## Disclaimer

This is a personal hobby project. It is not made, endorsed or supported by HP
Inc. or Apple Inc. HP and LaserJet are trademarks of HP Inc., macOS is a
trademark of Apple Inc., and these names appear here only to say which printer
and system the driver is for.

The software is provided as is, without any warranty, as described in sections
11 and 12 of the license. You use it at your own risk. I am not liable for any
damage to your printer, your computer or your data, for lost paper or toner, or
for anything else that happens because you used it.

HP's firmware is not distributed here. The installer downloads HP's own driver
package from Apple or HP and extracts the file on your Mac. That file and its
use remain under HP's terms for its driver.

## License

GNU General Public License, version 2 or (at your option) any later version.
See `LICENSE`. The filter is based on foo2zjs by Rick Richardson and uses
JBIG-KIT by Markus Kuhn, both under the same license; their notices are in
`vendor/foo2zjs`.
