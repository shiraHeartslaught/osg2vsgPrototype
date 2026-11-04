#!/bin/sh
# Downloads the Natural Earth administrative boundaries at all three scales (public domain) into
# DEST (default: the data drive), as the shapefiles the viewer's LOD groups are demonstrated with:
#   ne_110m_ / ne_50m_ / ne_10m_admin_0_countries   (international boundaries, 1:110M / 1:50M / 1:10M)
#   ne_110m_ / ne_50m_ / ne_10m_admin_1_states_provinces (state / province boundaries)
#   ne_10m_admin_2_counties                          (US counties, local boundaries)
# The viewer reads the scale from the ne_<N>m_ prefix and puts the three scales of a set into one
# LOD group, so the coarsest set that still resolves at the current zoom is the one shown.
set -e
DEST="${1:-/run/media/heartslaught/games/ive_test/naturalearth}"
mkdir -p "$DEST"
cd "$DEST"
for f in 110m/cultural/ne_110m_admin_0_countries 50m/cultural/ne_50m_admin_0_countries 10m/cultural/ne_10m_admin_0_countries \
         110m/cultural/ne_110m_admin_1_states_provinces 50m/cultural/ne_50m_admin_1_states_provinces 10m/cultural/ne_10m_admin_1_states_provinces \
         10m/cultural/ne_10m_admin_2_counties; do
    n=$(basename "$f")
    if [ -f "$n.shp" ]; then echo "have $n"; continue; fi
    curl -sSL --max-time 600 -o "$n.zip" "https://naciscdn.org/naturalearth/$f.zip"
    unzip -oq "$n.zip" && rm "$n.zip"
    echo "got $n"
done
