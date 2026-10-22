#!/bin/bash
# Change one preference in the iOS root and make a diff flash.
#
#   tools/prefs-fix.sh DOMAIN KEY [VALUE]   (asks for the sudo password once)
#
# DOMAIN is a file name under /private/var/mobile/Library/Preferences
# without .plist (e.g. com.apple.Accessibility). With VALUE (true/false/an
# integer/a string) the key is set, without it the key is removed. Shows
# the domain's text- and font-related keys before and after. First, in
# recovery: tools/phone.sh sync work/ios-root.img. Then flash
# work/diff.simg to userdata and mv work/ios-root-new.img work/ios-root.img.
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -ge 2 ] || { sed -n '2,12p' "$0"; exit 2; }
domain=$1 key=$2 value=${3-}
plist=work/root/private/var/mobile/Library/Preferences/$domain.plist
rm -f work/ios-root-new.img work/diff.simg
cp -c work/ios-root.img work/ios-root-new.img
sudo tools/rootimg.sh mount work/ios-root-new.img
trap 'sudo tools/rootimg.sh umount' EXIT
show() { sudo plutil -p "$plist" | grep -i "text\|font\|legib\|bold\|$key" || echo "(no matching keys)"; }
echo "== before"; show
if [ -n "$value" ]; then
	case $value in
	true|false) type=-bool ;;
	*[!0-9]*) type=-string ;;
	*) type=-integer ;;
	esac
	sudo plutil -replace "$key" $type "$value" "$plist"
else
	sudo plutil -remove "$key" "$plist" || true
fi
echo "== after"; show
sudo tools/rootimg.sh umount
trap - EXIT
python3 tools/simgdiff.py work/ios-root.img work/ios-root-new.img work/diff.simg
ls -la work/diff.simg
