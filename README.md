# pixel3-ios

Groundwork for porting [ios-sweet](https://github.com/dipa-studio/ios-sweet)
(Apple's XNU + the iOS 16 userland on a Redmi Note 10 Pro) to the
**Google Pixel 3 (blueline, Snapdragon 845)**.

## Step 1: the probe (`p1-probe/`)

A tiny bare-metal program the Pixel 3 bootloader runs like a Linux kernel.
It touches nothing on the phone's storage. One boot answers:

1. Does the bootloader run our image, and is the framebuffer at `0x9d400000`?
2. Are colours `a8r8g8b8`? (the three bars should read RED, GREEN, BLUE)
3. **Does the command-mode panel keep refreshing by itself?** A yellow bar
   sweeps along the bottom and a `t=Ns` counter counts for 30 s.
4. CPU ID, 16K page support, PSCI version, where the image and DTB landed.
5. The live memory map and reserved carve-outs, to plan where XNU goes.

After 90 more seconds it reboots straight back into fastboot.

### Run it

Phone in fastboot (power off, then hold Power + Volume Down), USB connected:

```sh
fastboot boot p1-probe/out/probe-v1.img
```

`fastboot boot` only loads the image into RAM for one boot; nothing is
flashed. If the phone stays in fastboot or says the image is invalid, try
`probe-v2.img` (same probe, DTB in a separate section).

If the auto-reboot lands in LineageOS instead of fastboot, that's fine:
`adb reboot bootloader` gets you back.

### What to report back (a photo of the screen is ideal)

| You see | Meaning |
| --- | --- |
| Probe screen, bar sweeps, counter climbs | Display path works like on the Redmi. Best case. |
| Probe screen appears but frozen (bar/counter stuck) | Panel needs a refresh "kick" per frame (MDP command-mode trigger). Solvable, adds a small display driver. |
| Fastboot screen stays, phone seems hung | Image runs but the display doesn't show our writes, or it crashed early. Hold Power + Vol Down ~10 s to recover. |
| Fastboot rejects the image | Header/DTB format issue: try v2, or rebuild with your stock `boot.img` (below). |
| Blue "EXCEPTION" screen | Photo it; ESR/ELR/FAR say exactly what faulted. |

### Build

```sh
cd p1-probe
./build.sh                      # uses the mainline DTB in ../dtb/
./build.sh /path/to/boot.img    # or reuse a stock (e.g. LineageOS) boot.img's DTBs and addresses
```

Needs LLVM (`clang`, `ld.lld`, `llvm-objcopy`, `llvm-nm`, `llvm-readelf`)
and `python3`. On macOS: `brew install llvm`, then
`LLVM=$(brew --prefix llvm)/bin ./build.sh`.

Emulator check (no phone needed): `EXTRA_CFLAGS=-DQEMU ./build.sh`, then run
`out/Image` with `qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a55 -m 4G -kernel out/Image`
and `pmemsave 0x9d400000 9331200 fb.raw` from the QEMU monitor to grab the screen.

`tools/bootimg.py info boot.img` shows any boot image's header, load
addresses and DTBs.

## Pixel 3 facts used here

| What | Value | Source |
| --- | --- | --- |
| Framebuffer | `0x9d400000`, 1080×2160, a8r8g8b8, stride 4320 | mainline `sdm845-google-blueline.dts`, downstream `cont_splash_region` |
| Panel | LG SW43408, DSC, **command mode** (`dsi_cmd_mode`, TE on GPIO 12) | downstream `dsi-panel-sw43408-dsc-fhd-cmd.dtsi` |
| Reboot reason | IMEM `0x146bf65c` = `0x77665500` → fastboot | downstream `sdm845.dtsi` `restart_reason@65c` |
| PS_HOLD | `0x0c264000` | downstream `sdm845.dtsi` |
| GIC | GICD `0x17a00000`, GICR `0x17a60000` (same as SM7150) | mainline `sdm845.dtsi` |
| UFS | `0x1d84000` (same as SM7150) | mainline `sdm845.dtsi` |
| Image header | `text_offset = 0x80000` (recent Pixel 3 bootloaders need it) | mainline Pixel 3 series cover letter |

`dtb/sdm845-google-blueline.dtb` is built from Linux master (2026-10-19).

## Licensing note

ios-sweet has no license file, so its code isn't copied here; this probe is
written from scratch. Ask its author before publishing a fork that reuses
their patches.
