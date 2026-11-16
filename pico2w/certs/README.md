# Trusted roots for the Pico 2 W

`mozilla-roots.pem` is the Mozilla CA root store, unmodified, as published by curl:

- Source: <https://curl.se/ca/cacert.pem>, "Certificate data from Mozilla as of: Thu Nov  5 04:12:01 2026 GMT"
- SHA-256: `427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285`, equal to the hash curl publishes in <https://curl.se/ca/cacert.pem.sha256>
- 118 roots; the build keeps the 114 this firmware's mbedTLS can use (see `../tools/make_ca_bundle.py`). The four it drops are Certum Trusted Network CA, TWCA Root Certification Authority and ACCVRAIZ1 (SHA-1 signatures) and e-Szigno TLS Root CA 2023 (P-521)
- Licence: MPL-2.0 (the certificate data is Mozilla's)

It is deliberately **not** the machine's own trust store, which can hold locally added CAs.

## Refreshing

```sh
curl -fsS -o pico2w/certs/mozilla-roots.pem https://curl.se/ca/cacert.pem
curl -fsS https://curl.se/ca/cacert.pem.sha256     # must match sha256sum of the file
```

Then set `NIO_PICO_CA_DATE` in `pico2w/CMakeLists.txt` to the "as of" date in the file's header,
update the hash and counts above and in `THIRD_PARTY.md`, and run the native tests
(`tests/pico-ca`, and `tests/test_pico_ca_bundle.py`). They fail if the set shrinks to a
handful of roots or no longer fits the flash budget.
