#!/bin/sh
# Инкремент номера версии прошивки при каждой сборке.
#
# Читает include/version.h, увеличивает FW_VERSION_MINOR на 1 и записывает
# обратно. Формат: WU.<major>.<minor> (minor — 3-значное число, при переходе
# 999 -> 1000 инкрементируется major и minor сбрасывается в 0).
#
# Сборки нумеруются с WU.1.000 (этап эмуляторов).
#
# Использование: bump_version.sh [path-to-version.h]

VERFILE="${1:-include/version.h}"

if [ ! -f "$VERFILE" ]; then
    echo "error: $VERFILE not found" >&2
    exit 1
fi

# Текущие значения
CURMAJ=$(grep -oE 'FW_VERSION_MAJOR[[:space:]]+[0-9]+' "$VERFILE" | grep -oE '[0-9]+' | head -1)
CURMIN=$(grep -oE 'FW_VERSION_MINOR[[:space:]]+[0-9]+' "$VERFILE" | grep -oE '[0-9]+' | head -1)
if [ -z "$CURMAJ" ] || [ -z "$CURMIN" ]; then
    echo "error: FW_VERSION_MAJOR/MINOR not found in $VERFILE" >&2
    exit 1
fi

NEWMIN=$((CURMIN + 1))
NEWMAJ=$CURMAJ
if [ "$NEWMIN" -ge 1000 ]; then
    NEWMAJ=$((CURMAJ + 1))
    NEWMIN=0
fi

# Собрать FW_VERSION_STR: WU.<major>.<minor> (minor с ведущими нулями до 3 знаков)
MINOR=$(printf '%03d' "$NEWMIN")

# Перезаписать файл аккуратно (sed -i на месте)
sed -e "s/FW_VERSION_MAJOR[[:space:]]\+[0-9]\+/FW_VERSION_MAJOR $NEWMAJ/" \
    -e "s/FW_VERSION_MINOR[[:space:]]\+[0-9]\+/FW_VERSION_MINOR $NEWMIN/" \
    -e "s/FW_VERSION_STR[[:space:]]*\"[^\"]*\"/FW_VERSION_STR  \"WU.$NEWMAJ.$MINOR\"/" \
    "$VERFILE" > "$VERFILE.tmp" && mv "$VERFILE.tmp" "$VERFILE"

echo "version bumped to WU.$NEWMAJ.$MINOR"
exit 0