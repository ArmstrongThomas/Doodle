# Mozilla CA bundle

`data/cacert.pem` is the Mozilla CA Extract bundle distributed by curl at
https://curl.se/docs/caextract.html. It is linked into the 3DS application as
read-only data; the client does not rely on an SD-card trust-store file.

Run `scripts/update-ca-bundle.ps1` to download both the bundle and curl's
published SHA-256, verify them before replacement, and update the local hash
record. Never update the PEM file without its matching hash record.
