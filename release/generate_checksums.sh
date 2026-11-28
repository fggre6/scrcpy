#!/bin/bash
set -e
cd "$(dirname ${BASH_SOURCE[0]})"
. build_common

cd "$OUTPUT_DIR"

# 只校验实际构建成功的压缩包：某个平台/架构不兼容被跳过后不应让发布失败
shopt -s nullglob
files=(scrcpy-*.zip scrcpy-*.tar.gz scrcpy-server*)
if [ ${#files[@]} -eq 0 ]
then
    echo "No release archives found in $OUTPUT_DIR" >&2
    exit 1
fi

rm -f SHA256SUMS.txt
sha256sum "${files[@]}" | tee SHA256SUMS.txt
echo "Release checksums generated in $PWD/SHA256SUMS.txt"
