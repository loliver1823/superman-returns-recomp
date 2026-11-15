# Getting the game files from your disc

> Drafted with AI assistance (Claude) for the project owner.

The port needs the files of the **USA** Xbox 360 disc of *Superman Returns* (2006), extracted to a
folder named `Superman Returns (USA)` that contains `default.xex` and `DATA/`. Use your own disc.
This project does not provide game files and does not link to places that do.

## What you need

- Your *Superman Returns* Xbox 360 disc (USA).
- One of:
  - an Xbox 360 that can run homebrew, with a file-transfer route to your PC; or
  - a DVD drive that can read Xbox 360 game discs (a drive with Xbox-compatible firmware).
- An XISO extraction tool on your PC.

## Steps

1. Make an image (`.iso`) of the disc with your console or your drive.
2. Extract the image's game partition with an XISO extraction tool. The result is a folder with
   `default.xex`, `DATA/` and a few other files.
3. Rename the folder `Superman Returns (USA)`.
4. Check that you have the right executable. In PowerShell:

   ```
   Get-FileHash "Superman Returns (USA)\default.xex" -Algorithm SHA256
   ```

   The USA retail `default.xex` is 9,973,760 bytes with SHA-256
   `c8f243acd99de9a91f5ae4f409721c0e954e3d5eb96861419d3da07b8106db2b`. If yours differs, it is
   another region or a modified file.

   (Checked 2026-10-09: a fresh extraction from the owner's disc image matches an earlier record of
   the same file; checked again 2026-11-16.)

## Notes

- Do not modify the files. Mods go in the port's own `mods/` folder, never into the disc folder.
- To build, the folder goes in the repository root ([`BUILDING.md`](BUILDING.md) step 2); the build reads
  `default.xex` from it, and the game reads `DATA/` at run time.
- The port never writes to this folder. Saves go to `%LOCALAPPDATA%\SupermanRecomp\content`.
- Discs from other regions have a different `default.xex` and are not supported.
