# Cordless

A native homebrew Discord client for the PlayStation Vita. It talks straight to
Discord's REST API and gateway over TLS, with no bridge server and nothing
self-hosted. Every network call runs on a vendored mbedTLS stack over raw
`SceNet` sockets.

> Cordless drives a personal Discord user token (a self-client), so it breaks
> Discord's Terms of Service. It's for personal use, not automation. It isn't
> affiliated with or endorsed by Discord or Sony, and you run it at your own risk.

## Features

Servers, DMs, group DMs, and threads. You can send, edit, reply, react, and
forward, and older history loads as you scroll up. It renders images, GIFs, and
WebP, and plays back video and voice messages. On top of that: Twemoji, live
presence, typing indicators, friends, profile popouts, mentions, invite joins,
blocking, multiple accounts, and permission-gated channel visibility and
moderation.

Voice (sound) channels are still in progress and untested. The code is there but
stays off behind `VOICE_JOIN_ENABLE`, since non-stage voice now needs Discord's
DAVE end-to-end encryption, and none of it has been exercised on hardware yet.

## Build

```bash
bash scripts/build.sh
```

That drops a `cordless.vpk` in the repo root. You'll need
[VitaSDK](https://vitasdk.org/) in WSL at `/usr/local/vitasdk`. mbedTLS and mlspp
come pre-vendored under `third_party/`, so you only rebuild them with
`scripts/build_tls.sh` or `scripts/build_dave.sh` if you actually change them.

The build flags are all off by default. `-DCORDLESS_INSECURE_TLS=ON` skips TLS
hostname matching for the emulator, `-DCLIENT_BUILD_NUMBER=<n>` sets the build
number Cordless advertises, and `-DVOICE_JOIN_ENABLE=1` together with
`-DVOICE_DAVE_ENABLE=1` turn voice back on (throwaway account only).

## Install and run

Install `cordless.vpk` in VitaShell on a Vita running HENkaku (firmware 3.60 to
3.74). Launch it and sign in with a token or a QR code; both are covered in
[docs/login.md](docs/login.md). Logs land at `ux0:data/cordless/log.txt`.

## Licenses

This project is MIT; see [LICENSE](LICENSE). Everything vendored keeps its own
license: mbedTLS (Apache-2.0), mlspp (BSD-2, (c) Cisco), jsmn (MIT), stb_image
(public domain), QR-Code-generator (MIT, (c) Nayuki), the Inter font (OFL 1.1),
Twemoji artwork (CC-BY 4.0), and the Mozilla CA bundle (MPL-2.0). The photo and
camera dialog layouts are adapted from LifeLua.

Disclosure: AI was used for API research and live area assets.
