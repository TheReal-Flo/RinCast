# Contributing to RinCast

Bug reports, fixes and new features are welcome. For anything large, open an
issue first so we can agree on the approach.

## Getting started

[docs/BUILDING.md](docs/BUILDING.md) covers the toolchains and how to build and
test each component. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) explains how
the pieces fit together, and [docs/PROTOCOL.md](docs/PROTOCOL.md) describes the
wire protocol.

## Before you open a pull request

- Match the style of the code around your change.
- Run the tests for what you touched:
  - `rincast-core-tests` for `shared/` (configure `pc` with `-DRINCAST_CORE_TESTS=ON`).
  - `go test ./...` in `server/`.
- Build the PC targets (`cmake --build`) and, for headset changes, `gradlew assembleDebug`.
- If you change the wire protocol, update `docs/PROTOCOL.md`.
- Add a line to `CHANGELOG.md` for anything users will notice.
- Don't add third-party code without listing it in `THIRD_PARTY_NOTICES.md`.
  Its licence must allow redistribution in this repository and in release builds.

## Versions

The version lives in the `VERSION` file. The PC build stamps it into
`pc/streamer/src/version.h`, and the installer and Gradle read it. Release tags
are `v<VERSION>`.

## Licence

By contributing, you agree that your contribution is licensed under the MIT
licence in [LICENSE](LICENSE), except for changes to `pc/audio-driver/src`,
which stay under the Microsoft Public License.
