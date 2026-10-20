#!/usr/bin/env python3
"""Generate the Apple Device Tree for XNU on the Pixel 3 (blueline).

  mkadt.py OUT.adt [--qemu] [--cpus N] [--ufs] [--iphone DT.im4p] [--dump]

The kernel is ios-sweet's SWEET machine config, so this provides what
that boot path reads: /chosen (RAM range, entropy), /cpus, /arm-io/gic,
the "sweet-fbcon" framebuffer node and /defaults. Values come from the
README (probe runs, mainline sdm845.dtsi).

Names the kernel matches on: the root lists "xiaomi,sweet" and the CPUs
list "qcom,kryo470" because ios-sweet's platform expert and CPU driver
match those strings; the blueline names come first.

Storage only with --ufs: adds /sweet-ufs, the UFS host at 0x1d84000 that
ios-sweet's root driver uses (rd=ufsroot0; read-only unless the boot-arg
ufsrw=1 is also given). Without it the kernel cannot reach the phone's UFS.

--iphone presents the board as the iPhone X (iPhone10,3 / D22AP) whose
iOS 16.7.16 userland runs on it: root identity, the /chosen, /buttons,
/arm-io/wdt and /arm-io/disp0 properties its daemons read, and /product
copied from the real D22AP device tree (DeviceTree.d22ap.im4p from the
IPSW; Apple's file, never stored here). Which properties matter and why is
from ios-sweet's bring-up notes.

--qemu swaps in QEMU virt's GIC, timer frequency and timer interrupt
(-M virt,gic-version=3) for emulator tests.
"""
import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from adt import Node, dump, load, parse, serialize, strs, u32, u64  # noqa: E402

# Kernel-managed RAM: above every fixed carve-out in both the mainline and
# the LineageOS device tree (README, "Second run"), 32 MiB aligned at both
# ends (XNU maps it with 16K-granule L2 blocks).
DRAM_BASE = 0xf4000000
DRAM_SIZE = 0x17e000000 - DRAM_BASE		# RAM ends at 0x17e0a0000

# /arm-io children are relative to this. It must not be 0: ios-sweet's
# serial setup treats a zero SoC base as "no SoC".
SOC_BASE = 0x01000000

FB, FB_W, FB_H = 0x9d400000, 1080, 2160
FB_STRIDE = FB_W * 4
FB_SIZE = 0x900000				# 1080*2160*4 = 0x8e6200, rounded up

PHONE = dict(gicd=0x17a00000, gicr=0x17a60000, timebase=19200000,
             vtimer=19)			# Qualcomm PPI 3 = INTID 19
QEMU = dict(gicd=0x08000000, gicr=0x080a0000, timebase=62500000,
            vtimer=27)			# QEMU virt: PPI 11 = INTID 27
GICD_SIZE, GICR_SIZE = 0x10000, 0x100000	# 8 redistributors x 0x20000

# Kryo 385: cores 0-3 Silver (A55-class), 4-7 Gold (A75-class), MPIDR
# core number in Aff1 (boot CPU MPIDR 0x81000000).
CPU_HZ = [1766400000] * 4 + [2803200000] * 4

# ufshc@1d84000 "std" registers (mainline sdm845.dtsi: size 0x2500)
UFS, UFS_SIZE = 0x01d84000, 0x3000


# --iphone: synthetic ECID (not any real device's), A11 chip id
ECID = 0x504958454c330001			# 'PIXEL3' + 1
CHIP_ID = 0x8015


def d22ap_product(im4p):
    """/product of the real D22AP tree, minus iBoot placeholders and bookkeeping."""
    root, _ = parse(load(im4p))
    for c in root.children:
        if c.name == 'product':
            return [(k, v) for k, v, ph in c.props if not ph and k not in ('name', 'AAPL,phandle')]
    sys.exit('%s: no /product node' % im4p)


def build(qemu=False, cpus=1, seed=None, ufs=False, iphone=None):
    hw = QEMU if qemu else PHONE
    phandles = iter(range(1, 1 << 16))

    def node(name, props=None, children=None):
        n = Node(name, props, children)
        n.set('AAPL,phandle', u32(next(phandles)))
        return n

    chosen = node('chosen', {
        'dram-base': u64(DRAM_BASE),
        'dram-size': u64(DRAM_SIZE),
        'random-seed': seed if seed is not None else os.urandom(64),
        'debug-enabled': u32(1),
        'firmware-version': strs('pixel3-shim'),
        'system-firmware-version': strs('pixel3-shim'),
    }, [
        node('memory-map'),		# mkpayload.py adds RAMDisk here
        # Boot policy as with `csrutil disable` plus an unsealed root;
        # XNU reads it from /chosen/asmb (kern_csr.c).
        node('asmb', {'lp-sip0': u64(0x6f), 'lp-sip1': u32(1)}),
    ])

    cpu_nodes = []
    for core in range(cpus):
        cpu_nodes.append(node('cpu%d' % core, {
            'device_type': strs('cpu'),
            'compatible': strs('qcom,kryo385', 'qcom,kryo470', 'ARM,v8'),
            'reg': u32(0 if qemu else core << 8),	# MPIDR Aff1:Aff0
            'cpu-id': u32(core),
            'cluster-id': u32(0),
            'cluster-type': u32(0),		# one homogeneous SMP cluster
            'state': strs('running' if core == 0 else 'stopped'),
            'timebase-frequency': u32(hw['timebase']),
            'clock-frequency': u32(CPU_HZ[core]),
            'bus-frequency': u32(100000000),
            'peripheral-frequency': u32(hw['timebase']),
            'fixed-frequency': u32(hw['timebase']),
        }))
    cpus_node = node('cpus', {'#address-cells': u32(1), '#size-cells': u32(0)}, cpu_nodes)

    gic = node('gic', {
        'compatible': strs('arm,gic-v3'),
        'interrupt-controller': b'',
        'virtual-timer-intid': u32(hw['vtimer']),
        'reg': u64(hw['gicd'] - SOC_BASE, GICD_SIZE, hw['gicr'] - SOC_BASE, GICR_SIZE),
    })
    arm_io = node('arm-io', {
        'device_type': strs('sweet-io'),
        'compatible': strs('sweet-io', 'arm-io'),
        '#address-cells': u32(2),
        '#size-cells': u32(2),
        'ranges': u64(0, SOC_BASE, (1 << 32) - SOC_BASE),
    }, [gic])

    fbcon = node('sweet-fbcon', {
        'reg': u64(FB, FB_SIZE),		# absolute physical address
        'width': u32(FB_W),
        'height': u32(FB_H),
        'stride': u32(FB_STRIDE),
    })

    # pmap_compute_max_asids() needs /defaults to exist.
    defaults = node('defaults')

    root_props = {
        'compatible': strs('google,blueline', 'xiaomi,sweet', 'qcom,sdm845', 'AppleARM'),
        'model': strs('Pixel3,1'),
        'manufacturer': strs('Google'),
    }
    extra = []
    if iphone:
        # compatible[0] -> hw.model/hw.target, model -> hw.machine/hw.product.
        # "xiaomi,sweet" stays: ios-sweet's platform expert matches it.
        root_props = {
            'compatible': strs('D22AP', 'iPhone10,3', 'google,blueline', 'xiaomi,sweet',
                               'qcom,sdm845', 'AppleARM'),
            'model': strs('iPhone10,3'),
            'target-type': strs('D22'),
            'target-sub-type': strs('D22AP'),
            'manufacturer': strs('Apple Inc.'),
        }
        for k, v in {
            # /sbin/mount -P 1 wants the boot manifest hash; cryptexd wants
            # the hash method (stand-in values, as a dev board would have).
            'boot-manifest-hash': hashlib.sha384(b'pixel3 boot manifest').digest(),
            'crypto-hash-method': strs('sha2-384'),
            # MobileGestalt: screen scale; IOPlatformUUID comes from these two.
            'display-scale': u32(3),
            'unique-chip-id': struct.pack('<Q', ECID),
            'chip-id': u32(CHIP_ID),
            # Security status of a dev-fused board: mobileactivationd then
            # takes its local "Hactivation" path (no Apple servers).
            'certificate-production-status': u32(0),
            'effective-production-status-ap': u32(0),
            'certificate-security-mode': u32(1),
            'effective-security-mode-sep': u32(1),
        }.items():
            chosen.set(k, v)
        # keybagd: no effaceable storage -> plain-plist system keybag.
        defaults.set('no-effaceable-storage', u32(1))
        # watchdogd: no PMU to report boot stages to.
        arm_io.children.append(node('wdt', {'no-pmu': u32(1)}))
        # IOMobileFramebuffer counts these; canvas = what iOS draws: the
        # iPhone X panel, which the kernel (patch 0061) presents and crops
        # to the 1080x2160 Pixel panel.
        arm_io.children.append(node('disp0', {
            'device_type': strs('display-subsystem'),
            'compatible': strs('sweet,display'),
            'canvas-width': u32(1125),
            'canvas-height': u32(2436),
        }))
        # MobileGestalt HomeButtonType 2: no Home button (gesture UI).
        extra.append(node('buttons', {
            'button-names': strs('volup', 'voldown', 'hold'),
            'home-button-type': u32(2),
            'opposed-power-vol-buttons': u32(1),
        }))
        product = node('product')
        for k, v in d22ap_product(iphone):
            product.set(k, v)
        extra.append(product)

    storage = []
    if ufs:
        storage.append(node('sweet-ufs', {
            'compatible': strs('qcom,ufshc'),
            'reg': u64(UFS, UFS_SIZE),		# absolute physical address
        }))

    root_props.update({'#address-cells': u32(2), '#size-cells': u32(2)})
    return node('device-tree', root_props, [chosen, defaults, cpus_node, arm_io, fbcon] + extra + storage)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='Pixel 3 Apple Device Tree generator')
    ap.add_argument('out')
    ap.add_argument('--qemu', action='store_true', help="QEMU virt's GIC and timer")
    ap.add_argument('--cpus', type=int, default=1, help='CPU nodes (default 1)')
    ap.add_argument('--ufs', action='store_true', help='add the UFS host (root on userdata)')
    ap.add_argument('--iphone', metavar='DT.im4p', help='iPhone X identity, /product from this tree')
    ap.add_argument('--dump', action='store_true')
    a = ap.parse_args()
    if not 1 <= a.cpus <= 8 or (a.qemu and a.cpus != 1):
        sys.exit('--cpus: 1..8 on the phone, 1 in QEMU')
    if a.qemu and a.ufs:
        sys.exit('--ufs: QEMU virt has no UFS host')
    root = build(a.qemu, a.cpus, ufs=a.ufs, iphone=a.iphone)
    blob = serialize(root)
    open(a.out, 'wb').write(blob)
    if a.dump:
        dump(root)
    print('%s: %d bytes' % (a.out, len(blob)), file=sys.stderr)
