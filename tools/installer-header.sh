#! /bin/bash

if (( $EUID != 0 )); then
    echo "Run as root!"
    exit -1
fi

embedded_file_offset=$(( $(grep -abo "__[C]UT_HERE__" $0 | head -n1 |cut -d: -f1) + 13 ))

tmpdir=$(mktemp -d)
tmptar="$tmpdir/ttkmd.tar"

trap 'rm -rf "tmptar"' EXIT

dd "if=$0" iflag=skip_bytes skip=$embedded_file_offset 2> /dev/null | gzip -cd > "$tmptar"

driver_version=$(tar -xOf "$tmptar" ./dkms_source_tree/dkms.conf | sed -nE "s/^[[:space:]]*PACKAGE_VERSION[[:space:]]*=[[:space:]]*\"([^\"]+)\"/\\1/p")

installed=($(dkms status | awk -F "[,:][[:blank:]]*" '$1 == "tenstorrent" { print $2 }'))

# Uninstall previous installations
for ver in "${installed[@]}"
do
	dkms remove tenstorrent/$ver --all
done

dkms ldtarball "$tmptar"
dkms install tenstorrent/$driver_version

echo "Reloading tenstorrent module"
modprobe -r tenstorrent
modprobe tenstorrent

exit 0
# __CUT_HERE__
