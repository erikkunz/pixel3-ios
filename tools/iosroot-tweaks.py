#!/usr/bin/env python3
"""Bring-up changes to the iOS root made by tools/mkiosroot.sh.

  sudo tools/iosroot-tweaks.py MOUNTED_ROOT

Idempotent; run as root (files keep their owners). What each change is for
(findings from ios-sweet's bring-up on the same iOS build, see their
NOTES.md; the code here is our own):

  launchd.plist   no daemon may panic the system when it crashes
                  (_PanicOnCrash renamed); CommCenter removed (no baseband);
                  jobs that crash and restart forever without the hardware
                  or services they expect removed (DENY, HackiOS's list)
  containers      /private/var/containers/... exist (normally the data volume)
  migration       recorded as done for this build, datamigrator not loaded
                  (as HackiOS does; lsd builds its database from /Applications)
  audio/media     VirtualAudio HAL plug-in moved aside; MediaToolbox does not
                  start the recorder server (both crash without audio/GPU)
  SpringBoard     no watchdogs (CPU rendering is slow), launch watchdogs x20,
                  no cloud-config UI, battery percentage shown
  Setup           marked done, mini-buddies off
  activation      mobileactivationd data ark says Activated (the kernel's
                  device tree selects its no-server "Hactivation" path)
"""
import glob
import os
import plistlib
import shutil
import sys

BUILD, VERSION = '20H392', '16.7.16'
MOBILE = 501

# Words in the labels of launchd jobs that are removed. HackiOS's list
# (hackios-xnu-userland, userland/mkjobs.py): each of these is missing its
# hardware or service here and crashes or is restarted over and over;
# syncdefaultsd aborts part-way through the migration pass.
# (mobileassetd stays: wallpapers and posters come as mobile assets.)
DENY = ('watchdog', 'CommCenter', 'wifi', 'WiFi', 'bluetooth', 'BTServer', 'locationd', 'softwareupdate',
        'datamigrator', 'backupd', 'thermalmonitord', 'keybagd', 'nfcd', 'biometrickitd', 'pearld',
        'obliterat', 'storage_mounter', 'com.apple.fud', 'sepd', 'seputil', 'camera', 'ReportCrash',
        'osanalytics', 'lockdown', 'audio', 'jetsamproperties', 'purplebuddy', 'restore', 'usb',
        'accessoryd', 'bird', 'homed', 'routined', 'duetexpertd', 'com.apple.dmd',
        'syncdefaultsd', 'photoanalysisd', 'avconferenced')


def read_plist(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def write_plist(path, data, uid, gid, mode, fmt=plistlib.FMT_BINARY):
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=fmt)
    os.chown(path, uid, gid)
    os.chmod(path, mode)


def rewrite(path, change, fmt=plistlib.FMT_BINARY):
    """Apply change(data) to a plist in place, keeping owner and mode."""
    st = os.stat(path)
    data = read_plist(path)
    change(data)
    write_plist(path, data, st.st_uid, st.st_gid, st.st_mode & 0o7777, fmt)


def preferences(root, domain, values):
    d = os.path.join(root, 'private/var/mobile/Library/Preferences')
    path = os.path.join(d, domain + '.plist')
    data = read_plist(path) if os.path.exists(path) else {}
    data.update(values)
    write_plist(path, data, MOBILE, MOBILE, 0o600)
    print('preferences', domain, ', '.join(sorted(values)))


def launchd_jobs(root):
    def change(plist):
        jobs = plist['LaunchDaemons']
        n = 0
        for job in jobs.values():
            if '_PanicOnCrash' in job:
                job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
                n += 1
        gone = sorted(k for k, j in jobs.items() if any(w in j.get('Label', '') for w in DENY))
        for k in gone:
            del jobs[k]
        print('launchd.plist: %d _PanicOnCrash renamed, %d jobs removed:' % (n, len(gone)))
        for k in gone:
            print('   ', k)
    rewrite(os.path.join(root, 'System/Library/xpc/launchd.plist'), change)


def containers(root):
    for sub, owner in (('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)):
        d = os.path.join(root, 'private/var/containers', sub)
        if not os.path.isdir(d):
            os.makedirs(d, 0o755)
            os.chown(d, owner, owner)
            print('created', os.path.relpath(d, root))


def system_container(root, identifier):
    pattern = os.path.join(root, 'private/var/containers/Data/System/*/'
                           '.com.apple.mobile_container_manager.metadata.plist')
    for meta in glob.glob(pattern):
        if read_plist(meta).get('MCMMetadataIdentifier') == identifier:
            return os.path.dirname(meta)
    return None


def activation(root):
    c = system_container(root, 'com.apple.mobileactivationd')
    if not c:
        print('note: no mobileactivationd container in the image yet; data ark skipped')
        return
    d = os.path.join(c, 'Library/internal')
    os.makedirs(d, exist_ok=True)
    os.chown(d, MOBILE, MOBILE)
    write_plist(os.path.join(d, 'data_ark.plist'), {
        '-ActivationState': 'Activated', '-BrickState': False,
        '-BuildVersion': BUILD, '-ProductVersion': VERSION,
        '-DataMigrationCompleted': True, '-PreSydroDataMigrationCompleted': True,
    }, MOBILE, MOBILE, 0o600)
    print('activation data ark written')


def migration_done(root):
    """Record the data migration as done for this build (undoes migration-reset.py)."""
    preferences(root, 'com.apple.migration', {
        'LastSystemVersion': BUILD,
        'DMLastMigrationResults': {'success': True, 'buildVersion': BUILD}})
    sentinel = os.path.join(root, 'private/var/datamigrator/migrate')
    if os.path.exists(sentinel):
        os.remove(sentinel)
        print('datamigrator sentinel removed')


def audio_media(root):
    plugin = os.path.join(root, 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin')
    if os.path.exists(plugin):
        aside = os.path.join(root, 'Library/Audio/Plug-Ins/HAL.disabled')
        os.makedirs(aside, exist_ok=True)
        shutil.move(plugin, aside)
        print('VirtualAudio.plugin moved aside')
    for conf in glob.glob(os.path.join(
            root, 'System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist')):
        rewrite(conf, lambda d: d.__setitem__('StartRecorderServer', False), plistlib.FMT_XML)
        print('MediaToolbox recorder server off')


def main():
    if len(sys.argv) != 2 or not os.path.isdir(os.path.join(sys.argv[1], 'System/Library/xpc')):
        sys.exit(__doc__)
    if os.geteuid() != 0:
        sys.exit('run as root')
    root = sys.argv[1]
    launchd_jobs(root)
    containers(root)
    activation(root)
    audio_media(root)
    migration_done(root)
    preferences(root, 'com.apple.springboard', {
        'BKNoWatchdogs': True, 'FBLaunchWatchdogScale': 20, 'SBShowBatteryPercentage': True,
        'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
        'MCForceShowCloudConfigurationUI': False})
    preferences(root, 'com.apple.purplebuddy', {
        'SetupDone': True, 'SetupFinishedAllSteps': True, 'SetupVersion': 11,
        'ForceNoBuddy': True, 'SetupState': 'SetupUsingAssistant',
        'daemonizeNeedsToRun': False})
    preferences(root, 'com.apple.purplebuddy.notbackedup', dict.fromkeys((
        'ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy', 'ShouldRunKeychainSyncMiniBuddy',
        'ShouldRunMesaMiniBuddy', 'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
        'SilentICDPUpgrade'), False))
    preferences(root, 'com.apple.frontboard', {'FBLaunchWatchdogScale': 20})
    preferences(root, 'com.apple.managedconfiguration.notbackedup', {
        'LockdownActivationIndicatesCloudConfigurationAvailable': False})


if __name__ == '__main__':
    main()
