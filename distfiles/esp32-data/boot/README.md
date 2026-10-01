Boot/config disk assets are generated here for ESP32 LittleFS uploadfs images.

Files in this directory are packaged into the ESP32 `storage` partition when
`./build.sh -f` runs through PlatformIO.
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
