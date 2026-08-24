# Third-party dependencies

The native Linux build fetches these projects at pinned revisions. They are **not vendored in this repository**.

| Project | Revision | Purpose |
| --- | --- | --- |
| encounter/aurora | `8b690b60e699c92e3327886ebd84cf7f05c5d36c` | GameCube OS/GX/PAD/DVD/CARD compatibility and host rendering/input |
| fuzziqersoftware/phosg | `891c3444b2084947eebf705c167b6279438114e9` | utility dependency used by resource_dasm |
| fuzziqersoftware/resource_dasm | `6ea3bd51ca4f9782d6c3c74003df1a4c8a0c4798` | JAudio bank/sequence decoding and software synthesis |

Aurora also resolves its own pinned dependencies (including SDL3, Dawn/WebGPU and Nod) through its CMake dependency providers.

Each dependency remains subject to its upstream license. The build scripts do not copy third-party license terms into this repository; consult the checked-out dependency directories or their upstream repositories after bootstrap.
