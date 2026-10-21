#!/bin/zsh
# Publishes the Bobble setup page (https://<owner>.github.io/<repo>/) with one release's firmware, for flashing a new
# Bobble from the browser. The page can't fetch release downloads (GitHub doesn't allow that across sites), so the
# four pieces go up beside it, on the gh-pages branch: one commit, replaced each time (no history of binaries).
#   ./publish_pages.sh <folder with the bobble-*.bin pieces> <version>      (build_app.sh --release runs it)
set -e -o pipefail
cd "${0:A:h}"
bins=${1:?folder with the bobble-*.bin pieces}
version=${2:?version}
repo=${BOBBLE_REPO:-$(cat update_repo)}
page=${BOBBLE_PAGE:-../amoled_demo/flasher/index.html}
out=$(mktemp -d)
cp $page $out/index.html
for f in bootloader partitions otadata firmware; do cp $bins/bobble-$f.bin $out/; done
# ESP Web Tools' manifest: where each piece goes (as in the firmware's flash_args)
cat > $out/manifest.json <<JSON
{
  "name": "Bobble",
  "version": "$version",
  "new_install_prompt_erase": true,
  "builds": [{
    "chipFamily": "ESP32-S3",
    "parts": [
      {"path": "bobble-bootloader.bin", "offset": 0},
      {"path": "bobble-partitions.bin", "offset": 32768},
      {"path": "bobble-otadata.bin", "offset": 61440},
      {"path": "bobble-firmware.bin", "offset": 131072}
    ]
  }]
}
JSON
touch $out/.nojekyll
git -C $out init -q -b gh-pages
git -C $out add -A
git -C $out -c user.name="Alexander Maat" -c user.email=107603244+alexandermaat99@users.noreply.github.com commit -q -m "Bobble $version setup page"
git -C $out push -q -f https://github.com/$repo.git gh-pages
rm -rf $out
owner=${repo%%/*} name=${repo#*/}
echo "setup page: https://$owner.github.io/$name/ (Bobble $version)"
