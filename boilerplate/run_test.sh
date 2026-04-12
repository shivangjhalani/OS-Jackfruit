#!/bin/bash

echo "=== Building Project ==="
make

echo -e "\n=== Setting up dummy RootFS ==="
mkdir -p rootfs-base/bin rootfs-base/proc
cp /bin/sh rootfs-base/bin/
cp -a rootfs-base rootfs-test

echo -e "\n=== Starting Supervisor ==="
./engine supervisor ./rootfs-base &
SUPERVISOR_PID=$!
sleep 1

echo -e "\n=== Testing IPC: PS Command ==="
./engine ps

echo -e "\n=== Testing IPC: Start Container ==="
./engine start alpha ./rootfs-test /bin/sh --soft-mib 50 --hard-mib 100
sleep 1

echo -e "\n=== Cleaning Up ==="
kill $SUPERVISOR_PID
rm -rf rootfs-test rootfs-base

echo "Done!"
