# phi-adapter-webos

LG webOS TVs for phi, over the TV's local SSAP WebSocket. No cloud.

## What it does (0.1)

- Connects to `wss://<tv>:3001/` (`ws://<tv>:3000/` on sets from before 2018).
- Pairs on first contact: the TV shows a prompt, and the client key it hands
  back is kept in the instance settings. Clear the key to pair again.
- Pins the TV's own certificate (SHA-256) on first contact and refuses a
  different one later. Clear the pinned value if the TV got a new one.
- One device of class `MediaPlayer` with three read-only channels:
  - `connectivity`: whether the TV answers,
  - `playState`: `playing`, `paused`, `idle` or `unknown`, from
    `ssap://com.webos.media/getForegroundAppInfo`,
  - `app`: the app in front, by title where the TV lists it.
- Reconnects after the retry interval while the TV is off; pings every 20 s
  and drops a connection that has been silent for 65 s.

Not yet: power, volume, input, transport commands, Wake-on-LAN.

## Known limits

- Some webOS 4.5 sets answer the media subscription without a state; the
  channel then reads `unknown` rather than guessing.
- "LG Connect Apps" has to be on in the TV's network settings.

## Layout

| File | What |
|------|------|
| `src/webos_socket.*` | TCP with optional TLS on the phi runtime loop |
| `src/webos_ws_client.*` | RFC 6455 client (from phi-adapter-test), ws:// and wss:// |
| `src/webos_protocol.*` | SSAP messages and what the answers mean |
| `src/webos_settings.*` | Instance settings from the adapter and its meta |
| `src/webos_model.*` | Device and channels |
| `src/webos_schema.*` | Name, icon, capabilities, settings form |
| `src/webos_instance.*` | Connection, pairing, subscriptions |

The register payload follows aiowebostv (Apache-2.0), which Home Assistant ships.

## Build

```sh
cmake -S . -B build -G Ninja && ninja -C build && ctest --test-dir build
```
