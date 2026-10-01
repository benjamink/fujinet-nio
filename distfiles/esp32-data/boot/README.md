Boot/config disk assets are generated here for ESP32 LittleFS uploadfs images.

Files in this directory are packaged into the ESP32 `storage` partition when
`./build.sh -f` runs through PlatformIO. All platforms' images together don't
fit the partition, so choose the ones a board needs in the `[fujinet]` section
of `platformio.local.ini`, as paths under `boot/` (a platform directory or a
single file), separated by spaces or commas:

```ini
[fujinet]
boot_images = amiga/wb32
; boot_images = bbc/FN-BOOT.ssd
; boot_images = msdos
```

Unset or `all` packs everything, `none` packs no images, and
`FN_BOOT_IMAGES=... ./build.sh -f` overrides the setting for one build. The
build lists what it packs, and stops before uploading if a name doesn't exist or
the selection won't fit (`pio-build/scripts/stage-fs-data.py`).
Do not commit generated `.atr`, `.img`, or `.ssd` files from this directory.
Generate them from the workspace boot-disk build tasks when packaging a local
build or release.

BBC FujiBus hosts use `bbc/FN-BOOT.ssd`; on ESP32 set:

```yaml
boot:
  mode: config
  config_uri: "flash:/boot/bbc/FN-BOOT.ssd"
  readonly: true
```

Amiga default disks are profile-specific FFS/DD ADFs.  After flashing the
storage image, select one explicitly, for example:

```yaml
boot:
  mode: config
  config_uri: "flash:/boot/amiga/wb13/FujiNet-Default-WB13.adf"
  readonly: true
```
