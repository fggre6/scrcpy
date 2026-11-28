#!/bin/bash
set -ex
cd "$(dirname ${BASH_SOURCE[0]})"
. build_common
cd .. # root project dir

if [[ $# != 2 ]]
then
    # <target_name>: for example win64
    # <format>: zip or tar.gz
    echo "Syntax: $0 <target> <format>" >&2
    exit 1
fi

FORMAT=$2

if [[ "$FORMAT" != zip && "$FORMAT" != tar.gz ]]
then
    echo "Invalid format (expected zip or tar.gz): $FORMAT" >&2
    exit 1
fi

BUILD_DIR="$WORK_DIR/build-$1"
ARCHIVE_DIR="$BUILD_DIR/release-archive"
TARGET_DIRNAME="scrcpy-$1-$VERSION"

rm -rf "$ARCHIVE_DIR/$TARGET_DIRNAME"
mkdir -p "$ARCHIVE_DIR/$TARGET_DIRNAME"

cp -r "$BUILD_DIR/dist/." "$ARCHIVE_DIR/$TARGET_DIRNAME/"
cp "$WORK_DIR/build-server/server/scrcpy-server" "$ARCHIVE_DIR/$TARGET_DIRNAME/"

# 正式版内置按键映射相关资源：
# - tools/mapping-designer: 浏览器里拖拽生成映射模板 JSON 的可视化工具（离线、免构建）
# - templates + scrcpy_mapping_config.json: 工具导出的模板与目标目录，开箱即用
mkdir -p "$ARCHIVE_DIR/$TARGET_DIRNAME/tools"
cp -r tools/mapping-designer "$ARCHIVE_DIR/$TARGET_DIRNAME/tools/mapping-designer"
cp -r templates "$ARCHIVE_DIR/$TARGET_DIRNAME/templates"
cp scrcpy_mapping_config.json "$ARCHIVE_DIR/$TARGET_DIRNAME/scrcpy_mapping_config.json"

mkdir -p "$OUTPUT_DIR"

cd "$ARCHIVE_DIR"
rm -f "$OUTPUT_DIR/$TARGET_DIRNAME.$FORMAT"

case "$FORMAT" in
    zip)
        zip -r "$OUTPUT_DIR/$TARGET_DIRNAME.zip" "$TARGET_DIRNAME"
        ;;
    tar.gz)
        tar cvzf "$OUTPUT_DIR/$TARGET_DIRNAME.tar.gz" "$TARGET_DIRNAME"
        ;;
    *)
        echo "Invalid format (expected zip or tar.gz): $FORMAT" >&2
        exit 1
esac

rm -rf "$TARGET_DIRNAME"
cd -
echo "Generated '$OUTPUT_DIR/$TARGET_DIRNAME.$FORMAT'"
