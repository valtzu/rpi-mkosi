#!/bin/bash -e

# See https://github.com/systemd/mkosi/issues/2024 and https://github.com/systemd/mkosi/issues/1638

echo -n "Patching UKI name to comply with systemd-sysupdate... "

img=${1:-$(mkosi --json summary|jq -r '.Images[0]|(.OutputDirectory+"/"+.Output+".raw")')}
IMAGE_ID=${IMAGE_ID:-$(mkosi --json summary|jq -r '.Images[0].ImageId')}
OUTPUT=${OUTPUT:-$(mkosi --json summary|jq -r '.Images[0].Output')}

if ! [ -f "$img" ] ; then
  echo "$img does not exist"
  exit 1
fi

esp=$(mktemp)
dd status=none of=$esp if=$img $(sfdisk -J $img|jq -r '.partitiontable|("bs="+(.sectorsize|tostring)+" skip=")+(.partitions[0]|((.start|tostring)+" count="+(.size|tostring)))')
if mdir -i $esp "EFI/Linux/$IMAGE_ID-*.efi" 2>/dev/null ; then
  mren -i $esp "EFI/Linux/$IMAGE_ID-*.efi" "EFI/Linux/${OUTPUT}.efi"
  dd conv=notrunc status=none if=$esp of=$img $(sfdisk -J $img|jq -r '.partitiontable|("bs="+(.sectorsize|tostring)+" seek=")+(.partitions[0]|((.start|tostring)+" count="+(.size|tostring)))')
  echo "done"
else
  echo "nothing to patch"
fi
rm $esp
