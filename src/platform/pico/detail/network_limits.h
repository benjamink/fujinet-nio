#pragma once
// SDK ALTCP credits application ciphertext only after a complete TLS record
// decodes. TCP must admit a full 16KiB record plus maximum TLS expansion/header.
#define FN_PICO_TLS_RECORD_BYTES 16384
#define FN_PICO_TLS_WIRE_OVERHEAD 2053
#define FN_PICO_TCP_WINDOW_BYTES 20480
#define FN_PICO_RAW_TCP_HANDLES 4
