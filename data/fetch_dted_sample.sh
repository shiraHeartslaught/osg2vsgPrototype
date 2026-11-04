#!/bin/sh
# Makes genuine DTED cells to try the terrain layer with: two one-degree Copernicus GLO-30 DEM tiles
# over San Francisco Bay (public, AWS open data) converted with GDAL into DTED level 2 (3601 x 3601
# posts, one arc second) and one DTED level 1 cell (1201 x 1201, three arc seconds).
# DEST (default: the data drive) ends up holding dted/n37_w123.dt2, dted/n37_w122.dt2, dted/n37_w123.dt1.
# Point the viewer at one file or at the whole dted/ directory (mosaicked):
#   ive_poc --projection globe --terrain DEST/dted --center -122.4 37.6 --zoom 11
set -e
DEST="${1:-/run/media/heartslaught/games/ive_test}"
GDAL_TRANSLATE="${GDAL_TRANSLATE:-gdal_translate}"
command -v "$GDAL_TRANSLATE" >/dev/null 2>&1 || GDAL_TRANSLATE=/home/heartslaught/vcpkg/vcpkg_installed/x64-linux-release-clang/tools/gdal/gdal_translate
mkdir -p "$DEST/dted_src" "$DEST/dted"
half=0.000138888888889 # half a one-arc-second post: Copernicus pixels are areas, DTED posts are points
for t in N37_00_W123_00:-123 N37_00_W122_00:-122; do
    name=${t%%:*}
    west=${t##*:}
    src="$DEST/dted_src/Copernicus_DSM_COG_10_${name}_DEM.tif"
    if [ ! -f "$src" ]; then
        curl -sSL --max-time 900 -o "$src" "https://copernicus-dem-30m.s3.amazonaws.com/Copernicus_DSM_COG_10_${name}_DEM/Copernicus_DSM_COG_10_${name}_DEM.tif"
    fi
    east=$((west + 1))
    out="$DEST/dted/n37_w$(( -west )).dt2"
    "$GDAL_TRANSLATE" -q -of DTED -ot Int16 -outsize 3601 3601 -r bilinear \
        -a_ullr "$(echo "$west - $half" | bc -l)" "$(echo "38 + $half" | bc -l)" "$(echo "$east + $half" | bc -l)" "$(echo "37 - $half" | bc -l)" \
        "$src" "$out"
    echo "wrote $out"
done
# a level 1 cell (three arc seconds) of the western tile, averaged down
half1=0.000416666666667
"$GDAL_TRANSLATE" -q -of DTED -ot Int16 -outsize 1201 1201 -r average \
    -a_ullr "$(echo "-123 - $half1" | bc -l)" "$(echo "38 + $half1" | bc -l)" "$(echo "-122 + $half1" | bc -l)" "$(echo "37 - $half1" | bc -l)" \
    "$DEST/dted_src/Copernicus_DSM_COG_10_N37_00_W123_00_DEM.tif" "$DEST/dted/n37_w123.dt1"
echo "wrote $DEST/dted/n37_w123.dt1"
rm -f "$DEST"/dted/*.aux.xml
