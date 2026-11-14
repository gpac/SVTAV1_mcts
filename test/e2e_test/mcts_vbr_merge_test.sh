#!/bin/sh
# Run with: sh test/e2e_test/mcts_vbr_merge_test.sh [path/to/SvtAv1EncApp]
# Requires ffmpeg (with trace_headers and AV1 decoding) and GPAC.
set -eu

encoder=${1:-SvtAv1EncApp}
workdir=$(mktemp -d "${TMPDIR:-/tmp}/svt-mcts-vbr.XXXXXXXX")
cleanup() {
    rm -f "$workdir/moving.y4m" "$workdir/static.y4m" \
        "$workdir/moving.obu" "$workdir/static.obu" \
        "$workdir/moving.yuv" "$workdir/static.yuv" \
        "$workdir/moving.trace" "$workdir/static.trace" \
        "$workdir/moving.headers" "$workdir/static.headers" \
        "$workdir/merged.mp4" "$workdir/moving.crop" "$workdir/static.crop"
    rmdir "$workdir"
}
trap cleanup EXIT

extract_headers() {
    awk '
        /\[trace_headers / && /Sequence Header$/ { section="seq"; print "SEQ"; next }
        /\[trace_headers / && /Frame Header$/ { section="frame"; print "FRAME"; next }
        /\[trace_headers / && /OBU header$/ { section="obu"; print "OBU"; next }
        /\[trace_headers / && /Tile Group$|Packet:|Temporal Delimiter$/ { section="" }
        section && $4 ~ /^[0-9]+$/ && $7 == "=" {
            key=$5
            if (key == "zero_bit" || key == "trailing_one_bit" || key == "trailing_zero_bit") next
            if (section == "obu" && key != "obu_type" && key != "obu_extension_flag" &&
                key != "temporal_id" && key != "spatial_id") next
            print key, $NF
        }
    ' "$1"
}

ffmpeg -v error -f lavfi -i 'testsrc2=size=960x544:rate=30' \
    -frames:v 85 -f yuv4mpegpipe -y "$workdir/moving.y4m"
ffmpeg -v error -f lavfi -i 'color=gray:size=960x544:rate=30' \
    -frames:v 85 -f yuv4mpegpipe -y "$workdir/static.y4m"

for mode in vbr cbr cqp; do
    for source in moving static; do
        if [ "$source" = moving ]; then bitrate=80; else bitrate=1200; fi
        case "$mode" in
            vbr) set -- --rc 1 --tbr "$bitrate" --mcts-base-qp 35 --hierarchical-levels 5 ;;
            cbr) set -- --rc 2 --tbr "$bitrate" --mcts-base-qp 35 ;;
            cqp) set -- --rc 0 --qp 35 ;;
        esac
        "$encoder" -i "$workdir/$source.y4m" -b "$workdir/$source.obu" \
            -o "$workdir/$source.yuv" --mcts 1 --preset 10 --keyint 49 \
            --lp 2 --progress 0 "$@"
        ffmpeg -loglevel repeat+trace -i "$workdir/$source.obu" -map 0:v:0 -c copy \
            -bsf:v trace_headers -f null - 2> "$workdir/$source.trace"
        extract_headers "$workdir/$source.trace" > "$workdir/$source.headers"
        if ! grep -q '^SEQ$' "$workdir/$source.headers" ||
            ! grep -q '^FRAME$' "$workdir/$source.headers" ||
            ! grep -q '^is_filter_switchable ' "$workdir/$source.headers"; then
            echo "Missing expected $mode traced headers for $source" >&2
            exit 1
        fi
        expected_bytes=$((85 * 960 * 544 * 3 / 2))
        actual_bytes=$(wc -c < "$workdir/$source.yuv")
        if [ "$actual_bytes" -ne "$expected_bytes" ]; then
            echo "Unexpected $mode reconstruction size for $source: $actual_bytes bytes" >&2
            exit 1
        fi
    done
    if ! cmp -s "$workdir/moving.headers" "$workdir/static.headers"; then
        echo "MCTS $mode headers differ" >&2
        diff -u "$workdir/moving.headers" "$workdir/static.headers" | head -n 100
        exit 1
    fi

    for first in static moving; do
        if [ "$first" = static ]; then
            gpac -i "$workdir/static.obu:#CropOrigin=0x0" \
                -i "$workdir/moving.obu:#CropOrigin=960x0" av1merge -o "$workdir/merged.mp4" -quiet
        else
            gpac -i "$workdir/moving.obu:#CropOrigin=960x0" \
                -i "$workdir/static.obu:#CropOrigin=0x0" av1merge -o "$workdir/merged.mp4" -quiet
        fi
        ffmpeg -v error -xerror -i "$workdir/merged.mp4" -vf 'crop=960:544:0:0' \
            -pix_fmt yuv420p -f rawvideo -y "$workdir/static.crop"
        ffmpeg -v error -xerror -i "$workdir/merged.mp4" -vf 'crop=960:544:960:0' \
            -pix_fmt yuv420p -f rawvideo -y "$workdir/moving.crop"
        cmp "$workdir/static.yuv" "$workdir/static.crop"
        cmp "$workdir/moving.yuv" "$workdir/moving.crop"
    done
    echo "MCTS $mode headers and merged tiles match"
done
