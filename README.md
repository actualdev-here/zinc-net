# zinc-net

Optional native HTTPS and WSS transport for Linux/LLVM Zinc programs. Requires
libcurl >= 8.2 compiled with WebSocket support and development headers, plus
OpenSSL libcrypto and its development headers (for example
`libcurl4-openssl-dev libssl-dev` on Debian/Ubuntu). Some distro libcurl builds omit
WebSocket support even when their version is new enough: check the installed
library/build, not just the version number. No network is performed at import.

## Install

```sh
zinc add zinc-net --git https://github.com/actualdev-here/zinc-net.git --tag v0.1.0
zinc install
```

Requires the matching [Zinc compiler/runtime](https://github.com/actualdev-here/Zinc)
with `net.call` support; older compiler builds are not compatible. Package
installation fetches source only: it does not install Clang, libcurl, libcrypto
or replace the compiler's runtime. The standalone release also includes the
matching native C source under `runtime/` for reference; the driver links its
own `ZINC_ROOT/runtime/zinc_net.c`, not package-provided build hooks.

The Zinc driver detects calls to the experimental `net.call` ABI in emitted LLVM
IR and links `runtime/zinc_net.c`, libcurl, libcrypto, libm and pthread only for those programs.
Compiler bootstrap and non-network programs remain independent of libcurl.
The package's typed wrappers are the supported interface; the five-string
`net.call` operation ABI is experimental and may change.

- `http url, method, authorization, body` returns a `Response`. `ok` means the
  transfer completed; inspect HTTP `status` yourself. Authorization is optional.
- `http_start` returns an `Integer` handle. `http_poll` returns `Transfer` with
  `ready` and `response`; completed handles are consumed. `http_cancel` requests
  cancellation and joins the worker. Up to four pending HTTP transfers are allowed.
- `http_start_timed url, method, authorization, body, budget_ms` starts a worker
  with a per-transfer total timeout of 1–15,000 ms (including connect). It does
  not mutate defaults for other requests. A timeout cannot prove that a POST
  was not applied remotely; never treat it as an automatic replay permission.
- `ws_open` returns an `Integer` handle. `ws_send` sends UTF-8 text. `ws_receive`
  returns a `Frame` whose kind is text/timeout/close; `ok = false` is a failure.
  Timeout preserves partial fragments. libcurl supplies client masking and
  automatic pong replies. The runtime additionally checks HTTP 101, Upgrade /
  Connection headers and the exact Sec-WebSocket-Accept digest using OpenSSL
  SHA-1/Base64; it does not rely solely on libcurl's upgrade validation.
  Binary messages are not supported.
  `websocket_supported []` checks the linked libcurl's actual WSS capabilities.
- `ws_close` drops the connection without a normal close handshake, preserving
  Discord resume semantics. Up to sixteen WSS handles are supported; handles
  are not reused, so stale handles cannot address a replacement connection.
- `clock_ms`, `elapsed`, `sleep_ms`, `random` and `seconds_ms` support scheduling.
  Clock values wrap at 1,000,000,000 ms; use `elapsed` and sample within one wrap.
  Random uses Linux getrandom with rejection sampling, not a predictable PRNG.

HTTPS/WSS only, certificate and hostname verification always on. Redirects,
URL userinfo/fragments and automatic environment proxies are disabled. Custom
trust roots can be explicitly set with `ZINC_NET_CA_FILE`; this changes the
trusted CA file, **not** certificate verification. Never use a test CA for a
real deployment. Generic transport permits arbitrary HTTPS origins; a client
sending credentials must impose its own origin policy (zinc-discord does).

Limits: connect 5s, HTTP/handshake 15s, WSS send 5s, receive wait 0..30s,
64 KiB bodies/messages, 4 KiB outgoing WSS text, 32 KiB aggregate HTTP headers.
Invalid UTF-8 and raw NUL are rejected. Fractional rate delays round upward;
nonfinite/negative/>24h delay metadata is rejected. HTTP cancellation is
cooperative and may wait for the bounded transfer timeout during connection
setup. Networking is single-threaded from Zinc; only native HTTP workers run
concurrently. Close/cancel all handles before discarding them.

Native ABI results are borrowed thread-local strings until the next ABI call;
wrappers copy response fields before another call. Application-visible Zinc
strings follow the current Zinc runtime's ownership behavior, which still needs
long-running memory qualification. Not available on direct native ELF or Wasm.

Run `sh tests/net_package.sh` for local TLS/WSS transport and connected-client
tests. References: [libcurl WebSockets](https://curl.se/docs/websocket.html),
[receive](https://curl.se/libcurl/c/curl_ws_recv.html),
[send](https://curl.se/libcurl/c/curl_ws_send.html).
The test harness lives in the Zinc source checkout, not the standalone package.

## License

[MIT](LICENSE).
