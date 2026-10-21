#!/bin/zsh
# Build the firmware and send it to the board over Wi-Fi.
#   ./ota.sh                  # board at bobble.local
#   ./ota.sh 192.168.68.62    # or by IP if .local names don't resolve
set -e -o pipefail
cd "${0:A:h}"
export PATH=/opt/homebrew/opt/python@3.12/bin:/opt/homebrew/bin:$PATH
. ~/esp/esp-idf/export.sh > /dev/null 2>&1
if ! idf.py build > build/last_build.log 2>&1; then     # a failed build must never upload the old binary
    grep -E "error:" build/last_build.log | sed 's|.*/main/|main/|' | head -20
    echo "Build failed - nothing uploaded (full output: build/last_build.log)"
    exit 1
fi
grep -E "warning:" build/last_build.log | grep "/main/" | sed 's|.*/main/|main/|' | sort -u || true
host=${1:-bobble.local}
token=$(sed -n 's/.*OTA_TOKEN "\(.*\)".*/\1/p' main/secrets.h)
echo "Uploading $(stat -f%z build/amoled_demo.bin) bytes to $host..."
# right after an update the board refuses another until it has confirmed itself healthy (~30 s): wait and retry
for try in 1 2 3 4 5 6; do
    reply=$(curl --silent --show-error --max-time 180 -H "X-OTA-Token: $token" -H "Content-Type: application/octet-stream" \
                 --data-binary @build/amoled_demo.bin -w "\n%{http_code}" "http://$host/ota") || reply=$'\n000'
    code=${reply##*$'\n'}
    [[ $code == 200 ]] && break
    echo "Board said HTTP $code (${reply%$'\n'*}) - retrying in 10 s..."
    [[ $try == 6 ]] && { echo "Gave up."; exit 1; }
    sleep 10
done
echo "Done - the board restarts into the new firmware (and rolls back by itself if it doesn't come up)."
