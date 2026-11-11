#!/bin/sh
# Puts the PAF headers in third_party/paf: GrapheneCt/ScePaf-RE b76d6ae,
# patched for vitasdk as Princess-of-Sleeping/vitasdk-paf-component 9a68b07
# does it. (ScePaf-RE has no licence file, so they aren't in this repository.)
set -e
cd "$(dirname "$0")/.."
rm -rf third_party/paf .paf
mkdir .paf
cd .paf
git clone -q https://github.com/GrapheneCt/ScePaf-RE.git
git -C ScePaf-RE checkout -q b76d6ae
git clone -q https://github.com/Princess-of-Sleeping/vitasdk-paf-component.git
git -C vitasdk-paf-component checkout -q 9a68b07
mkdir include
cp -R ScePaf-RE/include/. include/
patch -p1 -s < vitasdk-paf-component/vitasdk_2.patch
cp vitasdk-paf-component/include/map include/paf/std/map
cp vitasdk-paf-component/include/math.h include/paf/math/math.h
mkdir -p ../third_party/paf
mv include ../third_party/paf/
cd ..
rm -rf .paf
echo "PAF headers in third_party/paf/include."
