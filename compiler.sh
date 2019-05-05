#!/bin/bash
echo "Start compiler"
now=$(date +'release_%d_%m_%Y_%H_%M')
echo "Compiling in " $now
mkdir releases/$now
./package-release.sh master releases/$now --no-package