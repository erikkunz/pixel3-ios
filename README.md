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

### First XNU boot on the phone (2026-10-21)

Kernel from CI run 39504429541 (ios-sweet 0001-0027 + our 0100), payload
16.3 MB, `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam`,
`fastboot boot` of `p2-shim/out/shim.img` built with the LineageOS DTBs.

**XNU runs on the Pixel 3 and prints to the framebuffer.** The photo shows
VM bootstrap and zone_init, OSLog/firehose, scheduler setup, "iBoot
version: pixel3-shim" (our ADT), the SWEET PRNG, pthread and AMFI,
"SWEET: platform expert registered", `ml_processor_register` and
`cpu_start() cpu: 0`, the BSD copyright banner, MAC framework, and as the
newest line `SWEET CPU: boot CPU 0 registered, SGI1 verified,
initialization complete` (so the GIC delivers a self-IPI). The early
marker band at the bottom is drawn too.

A later photo shows it kept going exactly as in QEMU: ktriage, SWEET HFS,
mcache/mbinit (64 MB pool), DLIL SUM16 self-tests, Skywalk self-tests (all
PASSED), lo0/gif0/stf0 attached, then `!BSD` and

    Waiting on <dict ...><key>IOProviderClass</key><string>IOMedia</string>
    <key>Content</key><string>Apple_HFS</string></dict>

**Step 2 is done: XNU boots on the Pixel 3 to "waiting for root device".**
Scheduler, timer interrupts (kernel threads get scheduled) and the GIC work;
the display needs nothing beyond copying into ABL's framebuffer. There is
no root disk yet, so this is the expected end. Hold Power + Volume Down to
get back to fastboot.

## Step 3: iOS root on userdata (in progress)

The user approved (2026-10-21) erasing LineageOS's `userdata` for the iOS
root; every flash is still confirmed separately. Nothing else on the phone
is touched.

| What | Value |
| --- | --- |
| `userdata` | `sda21` = UFS LUN 0, `0xCE4FFB000` bytes (51.6 GiB), fastboot type `raw` |
| Kernel | ios-sweet 0001-0059 + ours (CI run 39506022755): UFS root driver (writes only with `ufsrw=1`), AMFI/sandbox/keystore/watchdog stand-ins, IOSurface + framebuffer services, HID services, iPhone identity. 0060+ drive sweet-only keys/touch/regulators/USB and stay out |
| ADT | `mkadt.py --ufs` adds the UFS host; `--iphone DeviceTree.d22ap.im4p` the iPhone X identity and `/product` |

**Read-only UFS test (2026-10-21), passed.** `rd=ufsroot0` without
`ufsrw`: the kernel took over the UFS host ABL left running, found
`userdata` in LUN 0's GPT, attached it as `ufsroot0` and read the HFS+
header block: "unknown Volume Signature : 394f" (LineageOS's encrypted
data), then retried the mount in a loop. No write possible in this mode.

**First flash and boot (2026-10-21).** `fastboot flash userdata` (sparse,
45 x 256 MiB; this ABL's max-download-size is `0x10000000`, so no `-S 512M`)
took 334 s. Boot with `--ufs --iphone`, `rd=ufsroot0 ufsrw=1 rootrw=1
launchd_unsecure_cache=1 ignition_level=1 unentitled_ios_sim_launch=1 ...`:
UFS read-write, persistent log at the last 4100 KiB, **HFS+ root mounted**,
`/sbin/launchd` executed, libignition started, then `cs_invalid_page` on a
launchd page and pid 1 died of SIGSEGV -> "initproc failed to start".
Cause: our `img2simg.py` wrote all-zero blocks as DONT_CARE, so zero blocks
inside files kept LineageOS's old bytes. Fixed (FILL chunks with value 0);
reflash needed.

**Second flash and boot (2026-10-21 00:50): iOS userland runs.** Reflash
with the fixed sparse image: 445 s (zero blocks now written). ABL then hung
receiving the boot image (as on sweet after big flashes); Power + Volume
Down, then `fastboot boot` worked. The Apple logo appeared briefly between
the kernel output, then launchd kept spawning daemons: `exec xpcproxy`,
`exec bluetoothd`, `fairplayd.H2`, ... in a loop (daemons for absent
hardware crash and are relaunched). Waiting to see whether it reaches
backboardd/SpringBoard (sweet: about 4-5 minutes on first boot).

**Log of that boot** (`tools/phone.sh log`, 2212 lines): ~100 daemons start
once and stay up; `bluetoothd` and `fairplayd.H2` crash-loop (no hardware /
no SEP), `driverkitd` was killed once. backboardd opens AppleCLCD
1080x2160, creates IOSurfaces and presents 5 frames (the Apple logo), then
backboardd and SpringBoard sit blocked (process sampler). No data
migration runs (no `migrationpluginwrapper`). Kernel text keeps drawing
over the panel because nothing mutes the console yet.

**SMP: all eight cores (2026-10-21).** Kernel = ios-sweet 0001-0059 + 0065
+ our 0101 (CI 39511860472), ADT `--cpus 8`, `cpus=8`: CPU_ON rc=0 and
"came alive; SGI1 OK" for MPIDR 0x100..0x700, then
`SWEET CPU: 8/8 CPUs initialized, SGI1 verified`. Silver and Gold run as
one homogeneous cluster (the scheduler does not prefer the Gold cores).

**8 cores are slow (display kernel, CI 39512538196, `cpus=8`).** Every
hand-off between processes stalls for minutes: launchd's data-protection
task (`seputil`, 0.16 s on one core) took 16:01:45 -> 16:04:51, the next
gap was to 16:07:03, and the kernel's log thread, which sleeps one second
per flush, flushed only 8 times in ~7 minutes. All 8 cores passed their
SGI test, and each core enables INTID 19 + SGI 1 on its own
redistributor, so suspect: the virtual timer interrupt on secondary cores
not arriving (timed wakeups on those cores lost). Test: same kernel with
`cpus=1` and `cpus=4` (Silver only).

**Hardware is fine; the stall is in software (probe, 2026-10-21).** The
probe now starts all 8 cores: every core's CNTPCT-CNTVCT is 0-1 tick (no
per-core offset from the hypervisor), and with the GIC set up as XNU does,
a 10 ms virtual timer wakes every core from WFI with INTID 19 pending.

**Kernel 0103 (per-CPU stats every 5 s) boots fast to the Apple logo** on
8 cores with the console muted (1125x2436 frames, so 0061 works). The
periodic stats thread seems to mask the lost wakeups. After ~9 min all
cores had taken 25k-107k timer interrupts and 10k-28k IPIs. backboardd
presented 4 frames (the logo). Symbolicated against the root image's
shared cache (`ipsw dyld a2s CACHE ADDR --slide 0x4705c000`), SpringBoard's
main thread waits in `_LSContextInitCommon` (LaunchServices database lock)
from Control Center's `CCSModuleRepository` app lookup: lsd builds the
LaunchServices database on first boot. Next: more time, sample lsd.

**8 cores, migration boot (2026-10-21 02:20): stuck again.** Stopped at
`exec seputil` like the first 8-core boot, and the log ends at t=223 s. The
per-CPU stats show the Gold cores (cpu4-7) never took an IPI or a timer
interrupt after start-up (counts 1/1), while cpu0-3 were busy. In the
earlier 0103 run they did get thousands later, so it is intermittent:
something in how cores hand work to each other. Probe tests so far only
cover self-SGIs, not one core waking another. Back to `cpus=1` for the
migration run (19:36); SMP debugging is a separate track.

Root image tools (ours; which iOS details matter is from ios-sweet's notes):

| Tool | What |
| --- | --- |
| `tools/mkiosroot.sh` | macOS: mounts the IPSW's OS, SystemOS and AppOS APFS images (owners on), builds a raw 16 GiB case-sensitive journaled HFS+ image with `newfs_hfs`, copies them decompressed with `rsync -aH` as root, links the shared cache and cryptex files, creates logd stores and the system keybag, runs the tweaks. Needs `sudo` |
| `tools/iosroot-tweaks.py` | No crash panics, no CommCenter, containers, migration done, Setup skipped, audio/media crash paths off, activation data ark |
| `tools/img2simg.py` | Raw image -> Android sparse image for `fastboot flash userdata` (zero blocks as FILL 0) |
| `tools/simgdiff.py` | Sparse image of only the blocks where a new image differs from the flashed one: a change flashes in seconds |
| `tools/rootimg.sh` | `sudo` mount/umount of a raw root image on the Mac to change files (owners kept) |
| `tools/phone.sh` | From LineageOS recovery (adb root): `log` pulls the kernel's persistent console log, `sync BASE.img` copies the 4 MiB pieces that differ from the phone into BASE so it is a valid diff base again |
| `tools/phonelog.py` | Decodes that log ring ("SWEETLOG" header + 4 MiB ring at the end of userdata) |

Changing the root without a full reflash (iOS writes to the volume when
booted with `rootrw=1`, so the base must be synced from the phone first):

```sh
fastboot reboot recovery                       # then Advanced -> Enable ADB
tools/phone.sh log                             # -> work/phone-log.txt
tools/phone.sh sync work/ios-root.img
cp -c work/ios-root.img work/ios-root-new.img
sudo tools/rootimg.sh mount work/ios-root-new.img   # edit under work/root
sudo tools/rootimg.sh umount
tools/simgdiff.py work/ios-root.img work/ios-root-new.img work/diff.simg
adb reboot bootloader && fastboot flash userdata work/diff.simg
mv work/ios-root-new.img work/ios-root.img
```

## Step 4: input (in progress)

`p3-input/` (probe, read-only on the PMIC), run 2026-10-21:

| What | Result |
| --- | --- |
| SPMI arbiter | v5 (`0x50000000`), 512 APIDs; PON APID `0xac`, GPIO 6 APID `0xbe` |
| Power | PON INT_RT_STS (`0x810`) bit 0: works |
| Volume down | `0x810` bit 1 (RESIN): works |
| Volume up | GPIO 6: input, enabled (`mode 0x00`, `en 0x80`, subtype `0x10`), but STATUS1 stays `0x80` and INT_RT_STS `0x00` when pressed (no pull-up/interrupt configured by ABL); left out for now |
| Touch | `st_fts@49`, `st,fts`, on `i2c@888000` (`qcom,i2c-geni`); IRQ GPIO 125, reset GPIO 99, switch GPIO 136, `vdd`/`avdd` supplies, max 1079x2159. ST FTS protocol as in ios-raphael's 0086 (fts_521) |

Kernel `0104-blueline-keys.patch`: power + volume down, polled every 20 ms,
fed to the HID services (`bluekeys=0` off).

Event stream (probe): 100 WFE take 82 us (Silver) / ~412 us (Gold) on every
core, so it is not the 8-core stall either. lsd (sampled) is blocked in
`__LAUNCH_SERVICES_CLIENTS_ARE_WAITING_FOR_THE_DATABASE_TO_FINISH_SEEDING__`
and `-[_LSInstallProgressService restoreInactiveInstalls]`: the
LaunchServices database is never seeded. Suspect: our tweaks skip the
first-boot data migration from the start (sweet ran it once first).
Fix applied 2026-10-21 02:20: `phone.sh sync` (142/4096 pieces had
changed on the phone), `tools/migration-reset.py` on a clone (markers
removed, `/private/var/datamigrator/migrate` sentinel), `simgdiff` = 101
blocks / 404 KB, flashed in 0.5 s. `iosroot-tweaks.py` no longer skips the
migration.

Touch: `p3-input` now also enables GENI I2C on QUP wrapper 0 / SE2
(`0x888000`; clocks from `gcc-sdm845.c`: vote `0x5200c` bits 6/7/12, SE2
RCG `0x17294` on the 19.2 MHz crystal; SDA/SCL GPIO 27/28 = `qup2`), reads
the FTS chip ID (`0xFA 20 00 00 00`) and shows FIFO events (`0x86`) while
the screen is tapped. Not run yet. Kernel `0105-blueline-touch.patch` is
the same as a polled driver, off unless `bluetouch=1`. Open question: is
the chip powered and sensing (supplies `vdd`/`avdd`, reset GPIO 99, and
`st,switch_gpio` 136, which routes the touch bus to the AP or the SLPI
sensor hub)?

Probe runs (2026-10-21 evening): SE2 clocks on (s2 CBCR `0x4220`, wrapper
core `0x1700c`/`0x17010` on), firmware is I2C (`0x300`), writes stick (SCL
readback = written value), `GENI_CGC_CTRL`/`OUTPUT_CTRL`/`FORCE_DEFAULT`
and `M_IRQ_EN` set, reset GPIO 99 high, switch GPIO 136 low. But an I2C
command never starts: GENI status and M_IRQ_STATUS stay 0 (no done, no
NACK). Next suspect: FIFO interface disabled for this SE
(`SE_GENI_IF_DISABLE_RO` 0x064, as Linux checks), which would mean GPI DMA.
Volume up: GPIO 6 STATUS1 reads `0x81` released and pressed alike.

**SMP hardware fully checked (probe, 2026-10-21):** SGI 1 sent from core 0
with XNU's encoding (`ICC_SGI1R` Aff1 = core, TargetList bit 0) reaches
every core in 4-7 us (Gold included), on top of synchronized counters,
working timer IRQs and the event stream. The 8-core hangs (Gold cores
never take an IPI in XNU) are therefore in XNU's SMP path with MMU and
caches on, not the hardware. iOS boots stay on `cpus=1` meanwhile.

**Migration boot on 1 core hangs at `usermanagerd`:** keybag task done
(`DONE MKB_INIT`), launchd execs `usermanagerd` (16:00:56) and nothing
follows for minutes; no crash. Next boot samples `usermanagerd`.

**Data migration runs (1 core, kernel with 0103-0105, 20:05).** The next
boot passed `usermanagerd` and showed iOS's migration screen (Apple logo +
progress bar); the bar reached ~98% in ~30 min, then crawled. Log after
~1 h 50 min: `migrationpluginwrapper` ran 3x, backboardd had presented
**193,000 frames** (~60 Hz, a full 10 MB copy each, on the only core: the
slowdown sweet also hit), 2x `hfs_clonefile: cluster_write failed - 34`.
**Keys work in iOS**: power and volume down reach the HID services with a
client (`SWEET HID: key 0x30/0xea ... (1 clients)`). Rebooted 21:56 with
`sweetfb_hz=10` (0056's panel-copy throttle) to give the migration the CPU.

### Other ports worth following

- **ios-sailfish** (Pixel 1, Snapdragon 821; owner known to us): GPU
  acceleration on the Adreno 530. Own kernel GPU driver (power, clocks,
  SMMU, CP microcode, zap shader via TrustZone, buffer objects + command
  submission), Mesa freedreno (GLES 3.1) injected into backboardd as
  `sweetgl.dylib` behind an EAGL shim, so CoreAnimation's GLES renderer
  runs on the GPU (no Metal). MDP double-buffered flips instead of a CPU
  copy, 15-35 fps UI. For our Adreno 630 (A6xx) the user-space side
  (freedreno a6xx) exists, but the kernel side needs the GMU (A6xx's GPU
  management unit: its firmware, HFI messaging, RPMh votes), which A5xx
  does not have; Linux's `drm/msm/adreno/a6xx_gmu.c` is the map. A large but
  well-documented project, after touch/SMP.
- Also from sailfish (credited to ios-topaz): IOKit never quiesces on iOS
  (no `kernelmanagerd` to drop the IOKit-daemon busy hold), and QuartzCore
  polls `IOAcceleratorES` 300x/100 ms per process. Our
  `0107-iokit-quiet-ioacceleratores.patch` removes the hold and registers an
  empty `IOAcceleratorES` (`sweetaccel=0` off). Candidate for the
  `usermanagerd`/LaunchServices waits.

- **HackiOS** ([hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel),
  [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland);
  POCO X5 Pro, SM7325): lock screen in ~20 s on 8 cores, Adreno via Mesa,
  Wi-Fi, sound, camera. A different foundation from ours: xnu-8796.121.2
  built on Linux with stock clang, their own platform expert/GIC/PSCI SMP,
  APFS and ramfs drivers, MiniIOSurface/framebuffer/keystore, AMFI/Sandbox
  MAC stand-ins, an EFI loader started from Mu-Silicium UEFI, and an iOS 16.5
  root (we run 16.7.16). Their `docs/PORTING.md` starts with a UEFI port;
  Mu-Silicium has SDM845 devices (polaris, equuleus) but no Pixel 3.
  Taken so far: `0112-blueline-idle-no-setway-flush.patch`. XNU's
  `cpu_idle()` on non-Apple cores cleans the whole D-cache by set/way on
  every WFI, which is unsafe with other cores running and traps to
  Qualcomm's hypervisor. That is the likely cause of our `wfi=0` workaround
  and the 8-core stalls. Next candidates: their small-core scheduler pairing
  (A55s run only when the big cores are busy), the memorystatus thresholds
  fix, and their keystore.

### 8 cores + keychain fix (2026-10-22)

`shim-aks8.img` (`cpus=8 wfi=0`, 0111): all 8 cores ran the whole session (cores 5-7
about 1 M IPIs each), securityd's `aks_wrap_key` now returns 0. The run was stopped at
68,000 frames, before the ~193,000 where the bar used to stall, so the migration
question is still open; bluetoothd (106) and fairplayd (70) restarted all along. Next:
`shim-idle8.img` (0112, `cpus=8` with real WFI), booted 21:24.

### HackiOS userland changes (flashed 2026-10-22)

`tools/iosroot-tweaks.py` now does what HackiOS's userland does. It marks the
data migration done for 20H392 (`LastSystemVersion` plus
`DMLastMigrationResults = {success, buildVersion}` in `com.apple.migration`;
our earlier skip lacked the second key). It removes datamigrator's sentinel
and drops the launchd jobs on their deny list (syncdefaultsd, routined,
homed, duetexpertd, datamigrator, keybagd, ReportCrash, mobileassetd and
others that crash or restart forever here). It also sets
`FBLaunchWatchdogScale = 20`, `ForceNoBuddy` and battery percentage. To be
tested: whether lsd builds LaunchServices from /Applications without the
migration, as it does for them. Cycle: `tools/phone.sh sync
work/ios-root.img` (recovery), `tools/tweak-diff.sh`, then flash
`work/diff.simg` (with approval).
Flashed as a 2.6 MiB diff: 52 launchd jobs removed (all Bluetooth, the Wi-Fi
daemons, routined, syncdefaultsd, homed, keybagd, watchdogd, ...). No job label contains
"datamigrator", so only the done-records skip the migration. fairplayd is not on the
list. `shim-idle8.img` booted on it at 21:58. The run before it (idle8 on the old root)
had 8 cores with real WFI, all handling timers and IPIs.

### Migration stuck at the end of the bar: keychain (2026-10-22)

Overnight 4-core run with 0107: the bar stopped at the same point (the last
frame at ~193,000 swaps, then static all night). Behind it, routined,
mobiletimerd, liveactivitiesd, fitnesscoachingd, remotepairingdeviced,
calaccessd (~49 launches each), healthd and tccd (~25) kept restarting
without crashing, and `securityd` called AppleKeyStore selector 10
(`aks_wrap_key`) 20 times, which the keystore stand-in refused
(`0xe00002c7`): no keychain item can be written. sweet never hit this
because its migration had completed before the stand-in mattered.
`0111-aks-wrap-unwrap.patch`: wrap = 8-byte marker + key (the size AES key
wrap adds), unwrap checks and strips it (self-consistent, no secrecy, like
the rest of the stand-in). Also: with `wfi=0` and no charger driver the
battery ran flat overnight; let it charge in fastboot/recovery between runs.

### USB networking and live log (built, not yet run)

ios-sweet's USB driver (0072-0077, 0081: DWC3 device mode, CDC-ECM network
adapter, console streamed as UDP) uses only addresses the Pixel 3 shares
with sweet: DWC3 `0xa600000`, qscratch `0xa6f8800`, QUSB2 v2 PHY `0x88e2000`
(`qcom,sdm845-qusb2-phy`), GCC `usb30_prim_gdsc` `0xf004`; both ABLs leave
the controller and PHY initialized from fastboot. CI builds `sweet_usb.c`
with `xnu/patches/0108-stage-sweet-usb.sh` (fetches ios-sweet at the pinned
commit, applies only that file) and `0108-blueline-usb-hooks.patch` (build
entry, start call, log helpers). Boot-args: `sweetusb=1` (default) reports
the controller state only, `sweetusb=2` starts the USB device; then on the
Mac `tools/usblog.sh` finds the adapter (host MAC `02:00:5e:77:ee:01`), sets
`192.168.7.1/24` (sudo) and prints/saves the console (UDP 5140) live.

First run (2026-10-22, `sweetusb=2`): controller found and powered, `ue0
192.168.7.2/24` up, soft reset fine, then `ep0 cmd 0x1 timeout` (SETEPCONFIG)
-> no enumeration. ABL had run fastboot at **SuperSpeed** (`DSTS` speed 4,
QMP USB3 PHY; sweet's ABL used USB 2), and the driver's clock table has
SM7150 offsets (SDM845: master `0xf00c`, sleep `0xf010`, mock UTMI `0xf014`,
aux `0xf04c`). `0110-blueline-usb-superspeed-phy.patch`: let the USB3 PHY
suspend (`GUSB3PIPECTL` bit 17, as Linux does for a high-speed-only DWC3;
`blueusb3susp=0` skips), optional `blueusbreset=0` (no soft reset), 20 ms
command timeout, SDM845 clock/PHY diagnostics.
Also from that log: 0106 found the Silver cluster already at its top level
(17 = 1766 MHz); the Gold cluster's cpufreq controller reports "not enabled".

### Volume up (prepared, needs one PMIC write)

GPIO 6 is an enabled input but has no pull-up, so the key changes nothing
readable. `p3-input/out/input-volup.img` (built with `-DVOLUP_PULLUP`)
writes GPIO 6 `DIG_PULL_CTL` (`0xc542`) = 0 (30 uA pull-up, as Linux's
`bias-pull-up`) via the arbiter's write channel, if the apps EE owns that
APID, then shows the key live. Only after approval; the kernel driver gets
the same write behind `bluevolup=1` once the probe shows which status bit
follows the key.

Probe run 2026-10-22: **volume up works** with STATUS1 bit 0 (`0x81`
released, `0x80` pressed). The pull-up write reported `0x00 -> 0x00`: it
was already set. Kernel `0109-blueline-volume-up.patch` reads it as key 1
(writes the pull-up once anyway, `bluevolup=0` skips).

**Touch: SE2 has its FIFO interface disabled** (`SE_GENI_IF_DISABLE_RO` = 1,
"DMA only"); after an I2C command the engine sits active (`GENI_STATUS`
`0x41`) waiting for GPI DMA. Touch therefore needs a GPI DMA I2C path (Linux
`qcom-gpi-dma` + `i2c-qcom-geni` GPI mode).

### Wi-Fi: what it would take (research, nothing built)

The Pixel 3's Wi-Fi is the WCN3990 on SDM845's SNOC bus (Linux `ath10k_snoc`).
Unlike a self-contained PCIe/SDIO chip it needs, in order:
1. **The modem DSP running**: WLAN firmware is loaded and managed through
   the modem subsystem (MPSS). That means remoteproc bring-up through
   TrustZone's PAS (peripheral authentication: signed `modem.mdt`/`.bNN`
   from the vendor/modem partition, `qcom,msa` memory), plus the
   `wlan_msa_mem` carve-out.
2. **QMI over QRTR over GLINK/SMEM** to talk to the WLAN firmware service
   (`wlfw`) and switch the firmware into mission mode.
3. **The ath10k data path**: copy engines on SNOC, the WCN3990 IRQs, SMMU
   context for its DMA.
4. **The iOS side**: iOS's `wifid` expects Apple's IO80211 family with
   Apple's Broadcom driver interface; there is no generic Wi-Fi driver to
   plug a Qualcomm chip into. Scanning/association UI would need a large
   IO80211-compatible implementation.

Each of 1-3 is its own driver project on XNU, and 4 is the hardest. ios-sweet
lists Wi-Fi as its own next big item with no code yet. **The realistic way
to get networking first is USB**: ios-sweet's 0072-0077 bring up DWC3 USB
device mode with CDC-ECM (the phone appears as a USB network adapter; sweet
already pings over it and streams its console log live). SDM845 has DWC3 at
`0xa600000` like SM7150, with a different PHY (QUSB2 v2 + QMP), so that is a
much smaller port and would also replace the recovery round-trips for logs.

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
