#!/bin/sh
# package.sh <version> : builds and zips ddraw.dll + swat3d11.ini + README.txt
set -e
cd "$(dirname "$0")"
./build.sh
V=$1; OUT=build/SWAT3-D3D11-$V
rm -rf $OUT && mkdir -p $OUT
cp build/ddraw.dll dist/swat3d11.ini dist/README.txt $OUT/
(cd $OUT && rm -f ../SWAT3-D3D11-$V.zip && zip -q ../SWAT3-D3D11-$V.zip *)
echo "build/SWAT3-D3D11-$V.zip"
