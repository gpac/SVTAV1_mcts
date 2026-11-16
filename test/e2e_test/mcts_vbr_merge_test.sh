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
        "$workdir/merged.mp4" "$workdir/moving.crop" "$workdir/static.crop" \
        "$workdir/tiled.y4m" "$workdir/tiled.obu" "$workdir/tiled.yuv"
    for y in 0 128 256 384; do
        for x in 0 128 256 384; do
            rm -f "$workdir/split_${x}x${y}.obu"
        done
    done
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

for depth in 8 10; do
    if [ "$depth" = 8 ]; then
        pixel_format=yuv420p
        bytes_per_sample=1
    else
        pixel_format=yuv420p10le
        bytes_per_sample=2
    fi
    ffmpeg -v error -f lavfi -i 'testsrc2=size=256x256:rate=30' \
        -frames:v 85 -pix_fmt "$pixel_format" -strict -1 -f yuv4mpegpipe -y "$workdir/moving.y4m"
    ffmpeg -v error -f lavfi -i 'color=gray:size=256x256:rate=30' \
        -frames:v 85 -pix_fmt "$pixel_format" -strict -1 -f yuv4mpegpipe -y "$workdir/static.y4m"

    # MCTS no longer chooses a common coding profile for independent encoders.
    # Coordinate these settings explicitly, then check every shared header below.
    for mode in vbr cbr cqp; do
        for source in moving static; do
            if [ "$source" = moving ]; then bitrate=80; else bitrate=1200; fi
            case "$mode" in
                vbr) set -- --rc 1 --tbr "$bitrate" --mcts-base-qp 25 --hierarchical-levels 5 ;;
                cbr) set -- --rc 2 --tbr "$bitrate" --mcts-base-qp 25 --pred-struct 1 --hierarchical-levels 2 ;;
                cqp) set -- --rc 0 --qp 25 --aq-mode 0 --pred-struct 1 --hierarchical-levels 3 \
                    --use-fixed-qindex-offsets 1 ;;
            esac
            "$encoder" -i "$workdir/$source.y4m" -b "$workdir/$source.obu" \
                -o "$workdir/$source.yuv" --mcts 1 --preset 10 --keyint 49 \
                --enable-dg 0 --scd 0 --enable-tf 0 --enable-overlays 0 \
                --scm 0 --tune 1 --lp 2 --progress 0 "$@"
            ffmpeg -loglevel repeat+trace -i "$workdir/$source.obu" -map 0:v:0 -c copy \
                -bsf:v trace_headers -f null - 2> "$workdir/$source.trace"
            extract_headers "$workdir/$source.trace" > "$workdir/$source.headers"
            if ! grep -q '^SEQ$' "$workdir/$source.headers" ||
                ! grep -q '^FRAME$' "$workdir/$source.headers" ||
                ! grep -q '^is_filter_switchable ' "$workdir/$source.headers"; then
                echo "Missing expected $mode traced headers for $source" >&2
                exit 1
            fi
            if grep '^base_q_idx ' "$workdir/$source.headers" | grep -qv '^base_q_idx 100$'; then
                echo "Frame-header QP changed in $depth-bit $mode for $source" >&2
                exit 1
            fi
            expected_bytes=$((85 * 256 * 256 * 3 / 2 * bytes_per_sample))
            actual_bytes=$(wc -c < "$workdir/$source.yuv")
            if [ "$actual_bytes" -ne "$expected_bytes" ]; then
                echo "Unexpected $mode reconstruction size for $source: $actual_bytes bytes" >&2
                exit 1
            fi
        done
        if ! cmp -s "$workdir/moving.headers" "$workdir/static.headers"; then
            echo "MCTS $depth-bit $mode headers differ" >&2
            diff -u "$workdir/moving.headers" "$workdir/static.headers" | head -n 100
            exit 1
        fi

        for layout in left right quad-static quad-moving; do
            if [ "$layout" = left ]; then
                gpac -i "$workdir/static.obu:#CropOrigin=0x0" \
                    -i "$workdir/moving.obu:#CropOrigin=256x0" av1merge -o "$workdir/merged.mp4" -quiet
                positions="0x0 256x0"
            elif [ "$layout" = right ]; then
                gpac -i "$workdir/moving.obu:#CropOrigin=0x0" \
                    -i "$workdir/static.obu:#CropOrigin=256x0" av1merge -o "$workdir/merged.mp4" -quiet
                positions="0x0 256x0"
            else
                neighbor=static
                if [ "$layout" = quad-moving ]; then neighbor=moving; fi
                gpac -i "$workdir/moving.obu:#CropOrigin=0x0" \
                    -i "$workdir/$neighbor.obu:#CropOrigin=256x0" \
                    -i "$workdir/$neighbor.obu:#CropOrigin=0x256" \
                    -i "$workdir/$neighbor.obu:#CropOrigin=256x256" \
                    av1merge -o "$workdir/merged.mp4" -quiet
                positions="0x0 256x0 0x256 256x256"
            fi
            for position in $positions; do
                source=static
                case "$layout:$position" in
                    left:256x0|right:0x0|quad-static:0x0|quad-moving:*) source=moving ;;
                esac
                x=${position%x*}
                y=${position#*x}
                ffmpeg -v error -xerror -i "$workdir/merged.mp4" -vf "crop=256:256:$x:$y" \
                    -pix_fmt "$pixel_format" -f rawvideo -y "$workdir/$source.crop"
                if ! cmp "$workdir/$source.yuv" "$workdir/$source.crop"; then
                    echo "MCTS $depth-bit $mode $layout tile $position differs" >&2
                    exit 1
                fi
            done
        done
        echo "MCTS $depth-bit $mode headers, relocated tiles, and replaced neighbors match"
    done

    for extraction in 2x2 4x4 rotated-2x2; do
        preset=4
        log2_tiles=1
        source_filter='testsrc2=size=512x512:rate=30'
        case "$extraction" in
            4x4) log2_tiles=2 ;;
            rotated-2x2) preset=0; source_filter="$source_filter,rotate=angle=0.02*n" ;;
        esac
        ffmpeg -v error -f lavfi -i "$source_filter" \
            -frames:v 50 -pix_fmt "$pixel_format" -strict -1 -f yuv4mpegpipe -y "$workdir/tiled.y4m"
        "$encoder" -i "$workdir/tiled.y4m" -b "$workdir/tiled.obu" \
            -o "$workdir/tiled.yuv" --mcts 1 --preset "$preset" --crf 30 --keyint 25 \
            --tile-columns "$log2_tiles" --tile-rows "$log2_tiles" --lp 2 --progress 0
        gpac -i "$workdir/tiled.obu" av1split -o "$workdir/split_"'$CropOrigin$.obu' -quiet
        size=$((512 >> log2_tiles))
        y=0
        while [ "$y" -lt 512 ]; do
            x=0
            while [ "$x" -lt 512 ]; do
                ffmpeg -v error -xerror -f rawvideo -video_size 512x512 -pixel_format "$pixel_format" \
                    -i "$workdir/tiled.yuv" -vf "crop=$size:$size:$x:$y" \
                    -pix_fmt "$pixel_format" -f rawvideo -y "$workdir/moving.crop"
                ffmpeg -v error -xerror -i "$workdir/split_${x}x${y}.obu" \
                    -pix_fmt "$pixel_format" -f rawvideo -y "$workdir/static.crop"
                if ! cmp "$workdir/moving.crop" "$workdir/static.crop"; then
                    echo "MCTS $depth-bit $extraction preset $preset tile $x,$y differs" >&2
                    exit 1
                fi
                x=$((x + size))
            done
            y=$((y + size))
        done
        echo "MCTS $depth-bit $extraction extracted tiles match at preset $preset"
    done
done
