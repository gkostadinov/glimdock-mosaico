# main

The entry point. `app_main()` runs the ESP-Iris boot probe, initialises NVS
(never erasing it: it holds the pairings), starts Recovery so the board stays
reachable even if the UI cannot start, marks the boot stages, and hands over to
the shell (`components/mosaico_shell`).

`idf_component.yml` pins the project's managed dependencies and points the
board support package and ESP-Iris at the workspace's submodules.
