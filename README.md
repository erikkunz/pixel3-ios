# pixel3-ios

**Apple's iOS 16 running on a Google Pixel 3.**

A port of [ios-sweet](https://github.com/dipa-studio/ios-sweet) to the Pixel 3 (`blueline`, Qualcomm
SDM845 / Snapdragon 845, Adreno 630). Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation), with
ios-sweet's patches and the Pixel 3 ones on top, runs the **unmodified iOS 16.7.16 (20H392) userland of the
iPhone X**. Google's own bootloader starts it like a Linux kernel; the iOS root lives in the `userdata`
partition.

> Experimental bring-up, not a usable phone OS. It **erases Android's user data**, Apple's security model is
> mostly switched off (permissive AMFI and sandbox, a stand-in keystore), and much is missing. Do not put
> anything personal on it. No Apple software is stored in this repository: the iOS root is built on your own
> Mac from Apple's IPSW. Not affiliated with or endorsed by Apple or Google.

## What works (2026-10-23)

| Area | State |
| --- | --- |
| Boot | Google ABL (unlocked) starts our shim, which loads XNU with an Apple device tree. Test kernels go in with `fastboot boot`; a kernel flashed to `boot_a` starts at power-on, while slot B keeps LineageOS and its recovery |
| CPU | All 8 cores (4x Kryo 385 Silver at 1.77 GHz, 4x Gold at 2.65 GHz), PSCI |
| Storage | UFS; the iOS root is an HFS+ image on `userdata`, read/write. Changes go to the phone as small sparse diffs (seconds) |
| Display | The 1080x2160 command-mode panel ABL leaves running. iOS sees an iPhone X (1125x2436); the frame is scaled to fit (thin side bars), copied by the CPU at 30 Hz |
| iOS | Data migration, lock screen, SpringBoard, Settings and the stock core apps; software rendering (no GPU yet) |
| Touch | ST FTM5 over bit-banged I2C (the QUP's GPI DMA path is not usable yet), its 3.3 V supply asked for over RPMh, interrupt-driven, edge gestures (home, app switcher) |
| Buttons | Power, volume up and down (SPMI PMIC) |
| Battery | A static battery (100 %, on power) so iOS shows one; no fuel gauge yet |
| Logs | The kernel console in a ring on `userdata`, read from LineageOS recovery |

**Being worked on:** USB Ethernet (live kernel log, command and file servers; ported from ios-flame, not yet
working on the phone), the removable system apps and wallpapers after migration.
**Not working yet:** GPU / Metal, Wi-Fi, real battery readings, the RTC (time of day), audio, camera,
Bluetooth, cellular, sensors, Face ID / SEP.

**Why iOS 16:** every iOS for newer iPhones is built for arm64e, which uses ARMv8.3 pointer authentication.
The Pixel 3's cores are ARMv8.2 and have none, so iOS 16 for the iPhone X (plain arm64) is the newest iOS that
can run natively here.

## How it boots

```
ABL (Google's bootloader, unlocked)
  -> shim (p2-shim/): a gzip'd arm64 Image + the stock DTB, header v1
       checks memory and the carve-outs, sets up the payload and jumps to XNU
  -> XNU (xnu/patches + ios-sweet's patches, built in CI)
       built-in drivers: UFS, framebuffer, touch, keys, cpufreq, RPMh, USB, battery
  -> launchd and the iPhone X userland from userdata (rd=ufsroot0)
```

## Repository

| Path | What |
| --- | --- |
| `p1-probe/` | Bare-metal probe: display, memory map, SMP and timer checks |
| `p2-shim/` | The shim ABL boots, which carries the XNU payload; QEMU test |
| `p3-input/` | Input probe: SPMI keys, touch (bit-banged I2C, RPMh LDO vote, FTM5 start-up), GPI/SMMU experiments |
| `common/` | Shared start-up, framebuffer console, FDT and build code |
| `xnu/patches/` | Our kernel patches (0100-0122) on top of ios-sweet's |
| `.github/workflows/xnu.yml` | Builds the kernel on GitHub's macOS runners: fetches ios-sweet, ios-sailfish and ios-flame code at pinned commits and applies ours |
| `tools/` | Device tree (`mkadt.py`) and payload, boot images, the iOS root (`mkiosroot.sh`, `iosroot-tweaks.py`), sparse diffs (`simgdiff.py`), phone access (`phone.sh`, `fbboot.sh`), preference fixes, USB tools |
| `dtb/` | The mainline blueline DTB used by the first probes |
| [docs/LOG.md](docs/LOG.md) | The full engineering log: every step, measurement and dead end |

## Our kernel patches

| Patch | What |
| --- | --- |
| 0100-0102 | Early framebuffer, 8-core SMP, panel size |
| 0103, 0116 | SMP diagnostics (per-CPU timers, IPIs, scheduler and GIC state) |
| 0104, 0109 | Power and volume keys |
| 0105, 0113, 0114, 0118, 0121 | Touch: bit-banged I2C, LDO19 via RPMh, FTM5 start-up, edge gestures, interrupt-driven |
| 0106 | CPU frequency for both clusters |
| 0107 | Quiet IOAcceleratorES polling, IOKit quiesce (after ios-sailfish) |
| 0108, 0122 | USB: ios-sweet's and ios-flame's driver, adapted to SDM845 (clocks, QUSB2 PHY, SMMU stream 0x740) |
| 0111 | AppleKeyStore key wrap/unwrap, so securityd can write keychain items |
| 0112 | No set/way cache flush on idle (from HackiOS) |
| 0115 | iPhone X frame scaled to fit the panel; touch mapped to match |
| 0117 | Breaks user fault loops caused by stale table-walk entries (the cause of the 8-core boot stalls) |
| 0119, 0120 | Static battery registered as AppleSmartBattery |

## Building and running (macOS)

Rough outline; [docs/LOG.md](docs/LOG.md) has the details and the reasons.

1. **Phone:** Pixel 3 with an unlocked bootloader and LineageOS recovery in slot B. Back up first
   (`tools/backup-phone.sh`).
2. **Kernel:** push to your fork, and the `xnu` workflow builds `kernel.development.sweet` (Xcode 15 on GitHub's
   macOS runners).
3. **Payload and shim:** `tools/mkadt.py` (the iPhone X device tree from the IPSW, plus the Pixel 3's
   CPUs, memory and display), `tools/mkpayload.py KERNEL ADT OUT --cmdline '...'`, then
   `p2-shim/build.sh PAYLOAD stock/boot.img`. Toolchain: `brew install llvm lld`.
4. **iOS root:** `sudo tools/mkiosroot.sh OS.dmg SYSTEMOS.dmg APPOS.dmg work/ios-root.img` from the
   iPhone X 16.7.16 IPSW, then sparse it (`tools/img2simg.py`) and `fastboot flash userdata` (this erases
   Android's data).
5. **Boot:** `tools/fbboot.sh p2-shim/out/shim.img`, or flash the shim to `boot_a` and
   `fastboot --set-active=a`. For logs, switch to slot B, boot recovery with ADB on, and run
   `tools/phone.sh log`.

## Credits

This builds directly on other people's work:

- [ios-sweet](https://github.com/dipa-studio/ios-sweet) (dipa-studio, MIT): the XNU port this is based on.
  Its patches and its USB driver are fetched at build time.
- [ios-flame](https://github.com/NWylynko/ios-flame) (Pixel 4, MIT): the USB driver, SMMU handling and USB
  host tools, and the roadmap for GPU and Wi-Fi.
- [ios-sailfish](https://github.com/joranv1/ios-sailfish) (Pixel 1): generic XNU fixes, GIC SPI support,
  touch-interrupt and performance ideas.
- [HackiOS](https://github.com/oosexgang/hackios-xnu-kernel) (OOSEX GANG,
  [userland](https://github.com/oosexgang/hackios-xnu-userland)): the idle-flush fix, the migration and
  daemon findings, and the bit-banging idea.
- [ios-raphael](https://github.com/Gsjsjzhznsz/ios-raphael): the ST FTS event format and the battery
  registration name.
- Linux (`gcc-sdm845.c`, `gpi.c`, `i2c-qcom-geni.c`, `cmd-db.c`, `rpmh-rsc.c`, `arm-smmu-qcom.c`) and
  Google's Pixel 3 FTM5 driver, used as hardware references.

Apple, iOS and iPhone are trademarks of Apple Inc.
