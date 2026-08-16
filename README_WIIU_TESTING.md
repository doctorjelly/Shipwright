# Ship of Harkinian 9.2.3 for Wii U (experimental)

This is a forward-port of the Wii U backend to Ship of Harkinian 9.2.3. It is an experimental build, not an official Harbour Masters release.

## Safe installation

1. Keep your existing stable Wii U installation in place.
2. Copy the artifact's `soh-9-2-wiiu` directory to `sd:/wiiu/apps/`.
3. Do not rename it to `soh` and do not overwrite the stable app directory.
4. Add a current 9.2.3-compatible `oot.o2r` or `oot-mq.o2r`, generated from your legally owned game, to `sd:/wiiu/apps/soh-9-2-wiiu/`.
5. Leave the included `soh.o2r` in that same directory.

The experimental build deliberately uses `sd:/wiiu/apps/soh-9-2-wiiu/` for its configuration and save files. It does not intentionally write to Wii U NAND. Removing that directory rolls back the experiment without touching the stable installation.

## First test sequence

Use the Release artifact only. Then test in this order:

1. Launch from the Homebrew Launcher and confirm the title says `Ship of Harkinian 9.2.3 (Experimental)`.
2. Reach the file-select screen without copying an old save into the experimental directory.
3. Create a temporary save, enter Kokiri Forest, pause, and save normally.
4. Return to the Wii U Menu using the HOME button, then relaunch and verify the temporary save.
5. Test the GamePad, one external controller if available, audio, suspend/resume, and a normal software shutdown.

If the console freezes, hold POWER only as a last resort. Remove `sd:/wiiu/apps/soh-9-2-wiiu/` before returning to the stable build if any repeatable crash or save corruption appears.

## Known validation boundary

GitHub Actions can verify compilation and produce RPX/WUHB artifacts, but real hardware testing is still required for GX2 rendering, controller behavior, suspend/resume, and long-session stability.
