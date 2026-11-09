# Source and compatibility

Version: 0.1.0 (experimental).

For tagged Git installs, use Zinc compiler commit
[`1617fe6adf41d457b1389421d353214bb2a7719b`](https://github.com/actualdev-here/Zinc/commit/1617fe6adf41d457b1389421d353214bb2a7719b)
or a compatible later compiler. It fixes shared transitive Git dependencies;
the earlier source snapshot below is not sufficient for Git diamond installs.

Exported from [Zinc commit 6baeeb0025b264055766ede56cdd8246d3532891](https://github.com/actualdev-here/Zinc/commit/6baeeb0025b264055766ede56cdd8246d3532891),
directory `packages/zinc-net`, plus the matching `runtime/zinc_net.c`.
Use a compiler/runtime built from that snapshot: older compilers lacking
`net.call` are incompatible. Linux/LLVM only; libcurl >=8.2 built with WebSockets,
libcrypto and development headers are required. Installing package source does
not install system dependencies or replace the compiler runtime.

The driver links `ZINC_ROOT/runtime/zinc_net.c`. The copy in this repository is
reference source, not an install/build hook. Tests live in the Zinc source
snapshot: `sh tests/net_package.sh` uses localhost TLS/WSS fake servers and test
credentials only. No live Discord requests were used for verification.

Published manifest change: the development sibling-path JSON dependency is
replaced by its immutable `v0.1.0` HTTPS Git source.

License: MIT, see LICENSE.
