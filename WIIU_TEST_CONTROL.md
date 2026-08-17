# Wii U debug telemetry and test control

This interface is included only in the `soh-wiiu-9.2.3-experimental-debug` artifact. It is development tooling for reproducing Wii U bugs and is not part of the normal PR-candidate build.

## Safety and prerequisites

- Use it only on a trusted local network. The debug build listens without authentication on TCP port `43385` while SoH is running.
- Keep a backup of `sd:/wiiu/apps/soh-9-2-wiiu/` before testing state changes.
- Load a save, close the pause menu and text boxes, and wait for scene transitions to finish before applying state.
- The tool does not explicitly write an applied state to disk. A later normal save or enabled autosave can persist the resulting game state.

The computer and Wii U must be on the same network. Substitute the Wii U's IPv4 address for `WII_U_IP` in these commands, run from the extracted artifact directory:

```sh
python3 test_control.py WII_U_IP ping
python3 test_control.py WII_U_IP get -o state.json
python3 test_control.py WII_U_IP apply state.json
python3 test_control.py WII_U_IP checkpoint 0
python3 test_control.py WII_U_IP restore 0
```

`ping` reports the running build. `get` captures a bounded JSON representation of the loaded save and current scene. `apply` validates and applies a captured or hand-edited state on the game thread. It refuses to change the save's quest type.

The JSON is a reproducible test setup, not a complete emulator snapshot. A full captured state reloads its saved entrance before restoring runtime fields; runtime restoration fails safely if that entrance does not lead to the captured room. To apply runtime fields in place, remove `save.entranceIndex` from a copy of the JSON and make sure its scene and room match the currently loaded scene and room.

`checkpoint` and `restore` use SoH's existing in-memory save-state slots `0`, `1`, or `2`. They provide a higher-fidelity short-term checkpoint during one launch, but disappear when SoH closes.

## Logs

The existing Aroma TCP syslog stream remains the diagnostic log channel. Test-control requests and failures are reported there with the prefix `[SoH][test-control]`; the JSON connection itself does not stream the general log.
