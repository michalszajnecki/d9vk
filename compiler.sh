#!/bin/bash
echo "Start compiler"
now=$(date +'release_%d_%m_%Y_%H_%M')
echo "Compiling in " $now
mkdir $now
./package-release.sh master $now --no-package