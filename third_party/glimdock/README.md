# Glimdock firmware sources (vendored)

These files come from [Glimdock](https://github.com/gkostadinov/glimdock)'s
`firmware/` folder, under its MIT licence ([LICENSE](LICENSE)). They are the
data layer the Mosaico build compiles unchanged: `network.cpp` (Wi-Fi, collector
polling, private reader, node settings, Kordlet chat) and the headers it and
the shell include (`display_clock.h` is shared with the launcher and Home).

The copy was taken on 2026-11-25 from a Glimdock working tree that is ahead of
the public repository's `baa992c` (2026-11-17): it adds the Mosaico platform
Wi-Fi hooks and board switches in `config.h` and the Kordlet chat
(`agent_chat.h` and its use in `network.cpp`).

Only the files the build uses are here. Private build defaults
(`local_credentials.h`) are never included: the Mosaico build defines
`GLIMDOCK_PUBLIC_FIRMWARE`, so `config.h` skips them and every board starts
unpaired.

To build against a live Glimdock checkout instead, point the
`GLIMDOCK_FIRMWARE_DIR` environment variable at its `firmware/` folder:

```sh
GLIMDOCK_FIRMWARE_DIR=/path/to/glimdock/firmware idf.py build
```

To refresh this copy, copy the same files over from that folder:

```sh
for f in include/config.h src/{agent_chat,audio,battery_status,board,brand,brand_portal,display_clock,model,network_recovery,operations_model,operations_parse,pairing_policy,runtime_health}.h src/network.cpp; do
  cp "$GLIMDOCK_FIRMWARE_DIR/$f" "firmware/$f"
done
```
