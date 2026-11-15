# Third-party code

| Where | What | Licence |
|---|---|---|
| `pico2w/certs/isrg-root-x1.pem` | ISRG Root X1 self-signed public certificate, copied from the installed trust store and checked against the [issuer certificate record](https://letsencrypt.org/certs/isrgrootx1.txt). SHA-256 fingerprint `96BCEC06264976F37460779ACF28C5A7CFE8A3C0AAE11A8FFCEE05C0BDDF08C6`. [Official PEM](https://letsencrypt.org/certs/isrgrootx1.pem). | Public X.509 certificate data |
| `third_party/esp/esp_littlefs/src/littlefs` | LittleFS 2.11, reused directly by the Pico backend without ESP VFS. Copyright The littlefs authors and Arm Limited; see its LICENSE.md. | BSD-3-Clause |
| `src/lib/disk/xex_boot_image.cpp` (`kPicoBoot`) | `picobootcode-dd.bin` (PicoBoot boot sectors, double-density variant) from MyPicoDOS 4.06, Copyright (C) 1992-2017 Matthias Reichl, embedded unmodified. The layout of the synthesised disk follows his `dir2atr`. | GPL version 2 or later |

This repository is licensed under the GNU General Public License; the "or later"
of the original permits it. The source of the boot code is `picobootcode.src` in MyPicoDOS.
