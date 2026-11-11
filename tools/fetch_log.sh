#!/bin/sh
# make log: the plugin's log, and the screenshots taken since the last time,
# from the Vita over VitaShell's FTP (press SELECT in VitaShell first). Each
# screenshot is deleted from the Vita once it is safely in build/screenshots.
# usage: fetch_log.sh <Vita IP> <build folder>
ip="$1"
out="$2"
base="ftp://$ip:1337"

curl --fail -sS -o "$out/startscreenmusic_log.txt" "$base/ur0:/tai/startscreenmusic_log.txt" || {
    echo "Couldn't reach the Vita at $ip: is VitaShell's FTP on (SELECT)?"
    exit 1
}
echo "Log: build/startscreenmusic_log.txt"
# What is in ur0:tai and ux0:music, so test files nobody needs can be found.
{
    echo "ur0:tai"
    curl -sS "$base/ur0:/tai/"
    echo "ux0:music"
    curl -sS "$base/ux0:/music/"
    echo "ur0:tai/config.txt"
    curl -sS "$base/ur0:/tai/config.txt"
} | tr -d '\r' > "$out/vita_files.txt"

# Screenshots are ux0:picture/SCREENSHOT/<folder>/<date>-<time>.<ext>; their
# names sort by when they were taken. The first time, the last 3 hours'.
shots="$out/screenshots"
mkdir -p "$shots"
last=$(cat "$shots/.last" 2>/dev/null || date -v-3H +%Y-%m-%d-%H%M%S)
newest="$last"
count=0
names() { curl --fail -sS "$1" | tr -d '\r' | awk -v want="$2" 'substr($1, 1, 1) == want { print $NF }'; }
for dir in $(names "$base/ux0:/picture/SCREENSHOT/" d); do
    for file in $(names "$base/ux0:/picture/SCREENSHOT/$dir/" -); do
        stamp="${file%.*}"
        if expr "$stamp" \> "$last" > /dev/null; then
            if curl --fail -sS -o "$shots/$file" "$base/ux0:/picture/SCREENSHOT/$dir/$file" && [ -s "$shots/$file" ]; then
                count=$((count + 1))
                curl --fail -sS -o /dev/null -Q "DELE /ux0:/picture/SCREENSHOT/$dir/$file" "$base/" ||
                    echo "Couldn't delete $file from the Vita"
            fi
            if expr "$stamp" \> "$newest" > /dev/null; then newest="$stamp"; fi
        fi
    done
done
echo "$newest" > "$shots/.last"

# Files on the Vita to delete, one path a line (ux0:..., ur0:...), listed in
# build/vita_delete.txt: deleted, then the list is emptied.
if [ -s "$out/vita_delete.txt" ]; then
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        dev="${path%%:*}"
        rest="${path#*:}"
        if curl --fail -sS -o /dev/null -Q "DELE /$dev:/${rest#/}" "$base/"; then
            echo "Deleted $path from the Vita"
        else
            echo "Couldn't delete $path"
        fi
    done < "$out/vita_delete.txt"
    : > "$out/vita_delete.txt"
fi
echo "$count new screenshot(s) in build/screenshots (deleted from the Vita)"
