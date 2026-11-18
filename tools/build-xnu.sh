#!/bin/bash
# Build the kernel locally on a Mac, the way .github/workflows/xnu.yml does.
#
#   tools/build-xnu.sh [WORKDIR]          (default work/kbuild)
#     -> WORKDIR/out/kernel.development.sweet, patch-list.txt, SHA256SUMS
#
# For when CI is not an option: GitHub retired the macos-14 runner image and
# the newer images only have Xcode 16+, whose iig emits IORPC.kernelContent,
# a field xnu-8796 does not have. Xcode 15.x is needed.
#
# Needs:
# - Xcode 15.x installed next to your normal Xcode (`xcodes install 15.4`).
#   Found as /Applications/Xcode-15*.app or Xcode_15*.app, or set
#   DEVELOPER_DIR. Only this build uses it; xcode-select is left alone.
#   Tested on macOS 26.6 with Xcode 15.4: its command-line tools run there
#   even though the app does not.
# - brew install jq gum xcodes bash cmake ninja
#
# The pins (repos, commits, patch selection, MACOS_VERSION) are read from
# xnu.yml so there is one place to change them. No sudo: the Kernel Debug
# Kit is unpacked into WORKDIR and passed to build.sh as KDKROOT instead of
# being installed into /Library/Developer/KDKs.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$(pwd)
wf=.github/workflows/xnu.yml
work=${1:-work/kbuild}
mkdir -p "$work"
work=$(cd "$work" && pwd)

pin() {	# NAME: the value of NAME from xnu.yml's env block
	sed -nE "s/^  $1: *\"?([^\"#]*)\"?.*/\1/p" "$wf" | sed 's/[[:space:]]*$//' | head -1
}
DXB_REPO=$(pin DXB_REPO) DXB_SHA=$(pin DXB_SHA)
SWEET_REPO=$(pin SWEET_REPO) SWEET_SHA=$(pin SWEET_SHA)
SWEET_LAST=$(pin SWEET_LAST) SWEET_EXTRA=$(pin SWEET_EXTRA)
SAILFISH_REPO=$(pin SAILFISH_REPO) SAILFISH_SHA=$(pin SAILFISH_SHA)
SAILFISH_PATCHES=$(pin SAILFISH_PATCHES)
MACOS_VERSION=$(pin MACOS_VERSION)
for v in DXB_SHA SWEET_SHA SWEET_LAST SAILFISH_SHA MACOS_VERSION; do
	[ -n "${!v}" ] || { echo "could not read $v from $wf" >&2; exit 1; }
done

# Xcode 15
if [ -z "${DEVELOPER_DIR:-}" ]; then
	xc=$(ls -d /Applications/Xcode-15*.app /Applications/Xcode_15*.app 2>/dev/null | sort -V | tail -1 || true)
	[ -n "$xc" ] || { echo "no Xcode 15 in /Applications (xcodes install 15.4), or set DEVELOPER_DIR" >&2; exit 1; }
	DEVELOPER_DIR=$xc/Contents/Developer
fi
export DEVELOPER_DIR
ver=$(DEVELOPER_DIR=$DEVELOPER_DIR xcodebuild -version 2>/dev/null | sed -n 's/^Xcode //p')
case $ver in 15.*) echo "Xcode $ver at $DEVELOPER_DIR" ;; *) echo "need Xcode 15.x, $DEVELOPER_DIR is '$ver'" >&2; exit 1 ;; esac

fetch() {	# DIR REPO SHA
	if [ "$(git -C "$1" rev-parse HEAD 2>/dev/null)" != "$3" ]; then
		rm -rf "$1"
		git init -q "$1"
		git -C "$1" fetch -q --depth 1 "$2" "$3"
		git -C "$1" checkout -q FETCH_HEAD
	fi
	git -C "$1" reset -q --hard && git -C "$1" clean -ffdxq	# a clean tree each run (also the ignored, already patched xnu/ source)
}
cd "$work"
fetch dxb "$DXB_REPO" "$DXB_SHA"
fetch sweet "$SWEET_REPO" "$SWEET_SHA"
fetch sailfish "$SAILFISH_REPO" "$SAILFISH_SHA"

# Patches, in the same order as the workflow
: > patch-list.txt
for f in sweet/xnu/patches/*; do
	n=$(basename "$f"); num=${n%%-*}
	if [[ $num > $SWEET_LAST ]] && ! echo " $SWEET_EXTRA " | grep -q " $num "; then continue; fi
	cp "$f" dxb/patches/
	case $n in *.patch) echo "$n" >> patch-list.txt ;; esac
done
for f in "$repo"/xnu/patches/*.sh; do [ -e "$f" ] && cp "$f" dxb/patches/; done
for f in "$repo"/xnu/patches/*.patch; do
	cp "$f" dxb/patches/
	basename "$f" >> patch-list.txt
done
i=200
for n in $SAILFISH_PATCHES; do
	f=$(ls sailfish/xnu/patches/$n-*.patch)
	dst="0${i}-sf-$(basename "$f")"
	cp "$f" "dxb/patches/$dst"
	echo "$dst" >> patch-list.txt
	i=$((i + 1))
done
# libfirehose_kernel is built arm64e-only; the SWEET kernel is plain arm64
sed -i '' 's/ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"/ARCHS="x86_64 arm64e arm64" VALID_ARCHS="x86_64 arm64e arm64"/' dxb/build.sh
grep -q 'VALID_ARCHS="x86_64 arm64e arm64"' dxb/build.sh || { echo "build.sh arm64 edit did not apply" >&2; exit 1; }
echo "$(wc -l < patch-list.txt | tr -d ' ') patches listed"

# Kernel Debug Kit, unpacked here (build.sh would sudo-install it)
kdk_name=$(sed -n "/'$MACOS_VERSION')/,/;;/s/.*KDK_NAME='\(.*\)'/\1/p" dxb/build.sh)
kdk_dir=$(sed -n "/'$MACOS_VERSION')/,/;;/s/.*KDKROOT='.*\/\(KDK_[^']*\)'/\1/p" dxb/build.sh)
[ -n "$kdk_name" ] && [ -n "$kdk_dir" ] || { echo "no KDK for macOS $MACOS_VERSION in build.sh" >&2; exit 1; }
if [ ! -d "$kdk_dir/System/Library/Kernels" ]; then
	url=$(curl -fsSL https://raw.githubusercontent.com/dortania/KdkSupportPkg/gh-pages/manifest.json |
		jq -r --arg n "$kdk_name" '.[] | select(.name==$n) | .url')
	[ -n "$url" ] || { echo "no download for '$kdk_name'" >&2; exit 1; }
	echo "downloading $kdk_name"
	curl -fL --progress-bar -o KDK.dmg "$url"
	mnt=$(mktemp -d)
	hdiutil attach -quiet -readonly -nobrowse -mountpoint "$mnt" KDK.dmg
	rm -rf kdkpkg
	pkgutil --expand-full "$mnt/KernelDebugKit.pkg" kdkpkg
	hdiutil detach -quiet "$mnt"
	rm -rf "$kdk_dir"
	mv kdkpkg/KDK.pkg/Payload "$kdk_dir"
	rm -rf kdkpkg KDK.dmg
fi
export KDKROOT=$work/$kdk_dir MACOS_VERSION
export KERNEL_CONFIG=${KERNEL_CONFIG:-DEVELOPMENT} MACHINE_CONFIG=SWEET

# Build (build.sh wants bash 4+; macOS's /bin/bash is 3.2)
BASH4=$(command -v /opt/homebrew/bin/bash /usr/local/bin/bash | head -1)
[ -n "$BASH4" ] || { echo "brew install bash" >&2; exit 1; }
status=0
(cd dxb && "$BASH4" build.sh) 2>&1 | tee build.log || status=$?
[ "$status" = 0 ] || { echo "build.sh failed ($status); see $work/build.log" >&2; exit "$status"; }

# build.sh skips a patch that does not apply, with only a warning
missing=0
while read -r p; do
	grep -q "Applying patch: .*/$p" build.log || { echo "NOT APPLIED: $p"; missing=1; }
done < patch-list.txt
[ "$missing" = 0 ] || exit 1

rm -rf out && mkdir out
cp -R dxb/fakeroot/System/Library/Kernels/. out/
cp patch-list.txt out/
(cd out && shasum -a 256 kernel.*.sweet > SHA256SUMS && cat SHA256SUMS)
echo "kernel: $work/out/kernel.$(echo "$KERNEL_CONFIG" | tr '[:upper:]' '[:lower:]').sweet"
