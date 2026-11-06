#!/bin/sh
# Build ftpd.cia from an existing build3ds/ (run `arm-none-eabi-cmake` + `cmake --build` first).
# Needs bannertool and makerom on PATH. Run from the repository root.
set -eu

root=$(pwd)
out=$root/out
mkdir -p "$out"

# makerom needs an absolute RomFS path
sed "s#RootPath *:.*#RootPath : $root/build3ds/romfs#" meta/ftpd-cia.rsf > "$out/ftpd-cia.rsf"

bannertool makebanner -i meta/banner.png -a meta/audio.wav -o "$out/banner.bnr"
bannertool makesmdh -s ftpd -l "FTP server" -p "mtheall" -i meta/icon.png -o "$out/icon.icn"

makerom -f cia -o "$out/ftpd.cia" -elf build3ds/ftpd.elf -rsf "$out/ftpd-cia.rsf" \
	-icon "$out/icon.icn" -banner "$out/banner.bnr" -exefslogo -target t
