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
fastboot boot p1-probe/out/probe.img
```

`fastboot boot` only loads the image into RAM for one boot; nothing is
flashed. The kernel must be gzip-compressed: the Pixel 3 bootloader
rejects an uncompressed Image with "Error verifying the received
boot.img: Unsupported".

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
and `python3`. On macOS: `brew install llvm lld` (Homebrew ships `ld.lld`
separately; `build.sh` takes any tool missing from `$LLVM` from `PATH`), then
`PATH=$(brew --prefix lld)/bin:$PATH LLVM=$(brew --prefix llvm)/bin ./build.sh`.

Emulator check (no phone needed): `EXTRA_CFLAGS=-DQEMU ./build.sh`, then run
`out/Image` with `qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a55 -m 4G -kernel out/Image`
and `pmemsave 0x9d400000 9331200 fb.raw` from the QEMU monitor to grab the screen.

`tools/bootimg.py info boot.img` shows any boot image's header, load
addresses and DTBs.

### Result of the first run (2026-10-20, LineageOS bootloader, mainline DTB)

Everything works, including the refresh test: the bootloader leaves the
command-mode panel refreshing from `0x9d400000`, so ios-sweet's
copy-to-framebuffer display path works unchanged.

| Item | Value |
| --- | --- |
| CPU | MIDR `0x517f803c` (Kryo 385 Silver), boot MPIDR `0x81000000` (core number in Aff1, MT set) |
| Pages / PA | 16K granule supported, PARange 40-bit, GIC sysreg interface |
| Firmware | PSCI 1.1, SMCCC 1.2, CNTFRQ 19.2 MHz, EL1 with MMU off |
| Loaded at | image `0x80080000`, DTB `0x84500000`, initrd `0x84700000` |
| RAM | one range `0x80000000`, size `0xfe0a0000` (4064 MiB) |
| Carve-outs | all below `0x9f800000` except rmtfs `0xf2700000`+`0x202000` |
| XNU candidate | `0xf2a00000`..`0x17e0a0000` (~2.2 GiB contiguous); fallback `0x9f800000`..`0xf2700000` (~1.3 GiB) |

### Second run: LineageOS DTB (2026-10-20)

Built with `./build.sh boot.img` from `lineage-22.2-20261005-nightly-blueline`
(sha256 `15b794c4…69b6f5ba`, kept in the gitignored `stock/`). That boot.img is
header v2 with an lz4 kernel and two SoC-only DTBs (SDM845 v2 and v2.1); the
bootloader picked one and applied the Pixel overlay from the `dtbo` partition:
MODEL `Google Inc. MSM sdm845 B1 DVT1.1`, COMPAT `google,b1c1-sdm845`, DTB
487342 bytes. CPU, firmware, load addresses and RAM (one range, 4064 MiB) are
the same as with the mainline DTB; the display and refresh test work as before.

Downstream carve-outs (27, read off the screen photo; names cut to 9 chars):

| Base | Size | Name | | Base | Size | Name |
| --- | --- | --- | --- | --- | --- | --- |
| `0x85700000` | `0x600000` | hyp | | `0x97800000` | `0x500000` | video |
| `0x85e00000` | `0x100000` | xbl | | `0x97d00000` | `0x800000` | cdsp |
| `0x85fc0000` | `0x2f40000` | removed | | `0x98500000` | `0x200000` | mba |
| `0x8ab00000` | `0x1400000` | qseecom | | `0x98700000` | `0x1400000` | slpi |
| `0x8bf00000` | `0x500000` | camera | | `0x99b00000` | `0x100000` | pil_spss |
| `0x8c400000` | `0x10000` | ips_fw | | `0x9d400000` | `0x2400000` | cont_splash |
| `0x8c410000` | `0x5000` | ipa_gsi | | `0xa1800000` | `0x10000` | debug_info |
| `0x8c415000` | `0x2000` | gpu | | `0xa1810000` | `0x200000` | ramoops |
| `0x8c500000` | `0x1a00000` | adsp | | `0xa1a10000` | `0x200000` | alt_ramoops |
| `0x8df00000` | `0x100000` | wlan_fw | | `0xa1c10000` | `0x1000` | ramoops_meta |
| `0x8e000000` | `0x9800000` | modem | | | | |

Dynamic (size only, placed by Linux): adsp `0x1000000`, qseecom_ta
`0x1000000`, secure_sp `0x800000`, secure_display `0x900000`, mem_dump
`0x2400000`, linux,cma `0x2000000`. XNU doesn't need these.

Differences from mainline: the Pixel overlay grows modem to `0x9800000`, which
moves video..pil_spss up by `0x2000000` (node names like
`video_region@0x95800000` are stale), and adds ramoops/debug_info at
`0xa1800000`..`0xa1c11000`. Downstream has no fixed rmtfs; mainline's
`0xf2700000` is a Linux-side choice.

**XNU memory range:** every fixed carve-out in either tree ends by
`0xa1c11000`, apart from mainline's rmtfs. Step 4 uses the range both trees
agree is free, `0xf2a00000`..`0x17e0a0000` (~2.2 GiB). If we need more, or RAM
below 4 GiB for DMA, `0xa2000000`..`0xf2700000` (~1.3 GiB) is also free; rmtfs
only matters once something runs the modem.

## Step 2: XNU to its framebuffer console (`p2-shim/`, `tools/`, `xnu/`)

```
ABL (unlocked, LineageOS bootloader)
  -> fastboot boot p2-shim/out/shim.img     RAM only, nothing flashed
     -> shim: checks the live DTB, copies the payload linked into its own image
        -> XNU (ios-sweet's SWEET kernel + our blueline patch), ADT, boot_args
```

| Piece | What |
| --- | --- |
| `tools/mkadt.py` | Apple Device Tree for blueline: RAM `0xf4000000`+`0x8a000000`, GIC, timer INTID 19, `sweet-fbcon` at `0x9d400000` 1080×2160, one CPU. No storage node, so the kernel cannot touch UFS. `--qemu` swaps in QEMU virt's GIC/timer |
| `tools/mkpayload.py` | Kernel Mach-O + ADT + boot-args into one payload; applies the kernel's chained fixups (nothing else would) |
| `tools/adt.py` | ADT serializer/parser, `adt.py dump X.adt` |
| `p2-shim/` | The loader. The payload is linked into the shim's Image, which ABL unpacks at `0x80080000` with ~68 MiB free below the DTB, so the ramdisk's 16 MiB gap below `hyp` at `0x85700000` does not matter. Before copying, it refuses kernel RAM that is outside `/memory`, overlaps a fixed `/reserved-memory` region, the framebuffer, itself or the DTB. Then a 3 s pause (photo the screen), copy, jump with MMU off, `x0` = boot_args |
| `xnu/patches/` | Our XNU patches, applied after ios-sweet's. `0100-blueline-early-framebuffer.patch`: the early debug console uses the blueline framebuffer (same 32 MiB block at `0x9c000000` as sweet's, so the mappings stay) |
| `.github/workflows/xnu.yml` | Kernel build on GitHub's macOS 14 runners: blacktop/darwin-xnu-build, xnu-8796.141.3, ios-sweet patches 0001-0027 fetched at a pinned commit, then ours. Fails if any patch was skipped |
| `common/` | Entry code, framebuffer console, FDT walker, PSCI/reboot and build steps shared by probe and shim |

Only ios-sweet patches up to 0027 (RAM-disk root) are used. 0028 and later
add the UFS root device and writes to `userdata`, which on this phone is
LineageOS's data; they come in only as a deliberate later step.

### Build and run

```sh
gh workflow run xnu.yml                          # or push a change under xnu/
gh run download RUN_ID -n xnu-kernel -D xnu/out
python3 tools/mkadt.py p2-shim/out/blueline.adt
python3 tools/mkpayload.py xnu/out/kernel.development.sweet p2-shim/out/blueline.adt \
    p2-shim/out/payload.bin --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam'
p2-shim/build.sh p2-shim/out/payload.bin stock/boot.img
fastboot boot p2-shim/out/shim.img
```

QEMU check of the whole loader path with a stand-in kernel
(`p2-shim/test/testkernel.c`, an arm64 Mach-O with chained fixups):
`p2-shim/test/run-qemu.sh` must print `screen: GREEN (pass)`. Status
2026-10-20: passes, and fails as it should with a wrong cmdline.

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

ios-sweet has no license file, so its code isn't copied here. The probe,
shim, ADT and payload tools are written from scratch (the ADT and boot_args
layouts are Apple's formats; the payload/ADT values follow from the
hardware). The kernel build fetches ios-sweet's XNU patches from their
repository at build time and never stores them here; `xnu/patches/` holds
only our own changes on top. Ask the author before publishing kernels or a
fork that reuses their patches.
