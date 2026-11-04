# Official Discord DAVE dependency

The included `lib/libdave.so` is a locally built Linux x86_64 library. It was built against this machine's OpenSSL 3 and C++ runtime. Rebuild for other distributions or architectures with `./build.sh /path/to/build-work` (requires Git, CMake, a C++ compiler, and OpenSSL development headers). The script fetches pinned source revisions and writes only to the requested build directory and this dependency directory.

- Discord libdave: `8de72b1f8a2ac3c5a5270755bb8091a62e3c6169`
- Cisco MLS++: `1cc50a124a3bc4e143a787ec934280dc70c1034d` (the libdave dependency pin)
- nlohmann JSON: `55f93686c01528224f448c19128836e7df245f72` (v3.12.0)
- OpenSSL: system shared library; persistent signature storage disabled.

Upstream source: https://github.com/discord/libdave. License notices are in `licenses/`.

The binary includes MLS++ (BSD-2-Clause) and uses nlohmann JSON headers (MIT). Both notices are included. OpenSSL 3 is dynamically linked from the host (Apache-2.0); its notice is also included for reference. The MLS++ optional MPark variant compatibility header is not enabled in this C++17 build. The library also uses the host's standard C/C++ runtimes; those system libraries are not copied into this folder.

Validation performed locally: all nine official libdave C API tests passed, including two-member MLS group setup, matching authenticators and fingerprints, and encrypted media round trips. This does not validate a live Discord call.
