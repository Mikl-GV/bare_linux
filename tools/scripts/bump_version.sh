#!/bin/sh
# Инкремент номера версии прошивки при каждой сборке.
#
# Читает include/version.h, увеличивает FW_VERSION_MINOR на 1
# и записывает обратно. Формат: W.<minor> (minor — 3-значное число).
#
# Использование: bump_version.sh [path-to-version.h]

VERFILE="${1:-include/version.h}"

if [ ! -f "$VERFILE" ]; then
    echo "error: $VERFILE not found" >&2
    exit 1
fi

# Текущее значение FW_VERSION_MINOR
CUR=$(grep -oE 'FW_VERSION_MINOR[[:space:]]+[0-9]+' "$VERFILE" | grep -oE '[0-9]+' | head -1)
if [ -z "$CUR" ]; then
    echo "error: FW_VERSION_MINOR not found in $VERFILE" >&2
    exit 1
fi

NEW=$((CUR + 1))

# Собрать новый FW_VERSION_STR: W.<N> с ведущими нулями до 3 знаков
MINOR=$(printf '%03d' "$NEW")

# Перезаписать файл аккуратно (sed -i на месте)
sed -e "s/FW_VERSION_MINOR[[:space:]]\+[0-9]\+/FW_VERSION_MINOR $NEW/" \
    -e "s/FW_VERSION_STR[[:space:]]*\"[^\"]*\"/FW_VERSION_STR  \"W.$MINOR\"/" \
    "$VERFILE" > "$VERFILE.tmp" && mv "$VERFILE.tmp" "$VERFILE"

echo "version bumped to W.$MINOR"
exit 0