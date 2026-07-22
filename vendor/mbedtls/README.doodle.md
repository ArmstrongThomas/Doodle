# Vendored Mbed TLS

This directory contains the unmodified `include/` and `library/` trees from
the official Mbed TLS 3.6.7 release tag (`mbedtls-3.6.7`).

- Upstream: https://github.com/Mbed-TLS/mbedtls
- Commit: `068ff080b369adfac81509f9b57b2afabaf82dc5`
- License: Apache-2.0 OR GPL-2.0-or-later (see `LICENSE`)

Collab Doodle selects its minimal TLS 1.2 client feature set through
`include/doodle_mbedtls_config.h`; no upstream files are patched.
