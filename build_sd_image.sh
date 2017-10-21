#!/bin/bash
# This script builds an SD card on Linux containg the required Neverball data for
# running the game in Dolphin Emulator

./mksdcard 512M sd.raw
sudo mount -o defaults,umask=000 sd.raw /mnt
mkdir -p /mnt/apps/neverball
cp -r data /mnt/apps/neverball
sudo umount /mnt
cp sd.raw ~/.dolphin-emu/Wii/sd.raw

