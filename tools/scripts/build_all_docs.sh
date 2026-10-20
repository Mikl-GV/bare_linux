#!/usr/bin/env bash
# Сборка docs/ALL-DOCS.md из всех документов проекта (единая папка docs/).
cd "$(dirname "$0")/../.." || exit 1

docs=(
  README.md
  SOFTWARE-ARCHITECTURE.md
  H3-BAREMETAL-GUIDE.md
  HARDWARE-REF.md
  HARDWARE-WIFI.md
  KNOWLEDGE.md
  REGISTER-MAP.md
  RESEARCH-MATCHAST.md
  LAKKA-ETALON-ZAMERY.md
  rtl8189fs-live-snapshot.md
  rtl8189fs-live-snapshot-big.md
  rtl8189fs-lakka-live-dump.md
  rtl8189fs-lakka-live-rx-dump-w7.md
  lakka-live/LAKKA-LIVE-SNAPSHOT-W537.md
  lakka-live/LAKKA-CAM-SECURITY.md
  lakka-live/LAKKA-MACREG-FULL.txt
  W233-SDIO-TRACE.md
  WIFI-FW-DOWNLOAD.md
  I2S-REFERENCE.md
  NEXT-CHAT-PROMPT.md
)

{
  echo "# ALL-DOCS: сводный документ проекта Bare_linux"
  echo
  echo "> Собрано автоматически из всех .md проекта (папка docs/)."
  echo "> Дата сборки: 2026-10-17."
  echo
  echo "## Оглавление"
  echo
  i=0
  for f in "${docs[@]}"; do
    i=$((i+1))
    echo "  $i. [$f](#$f)"
  done
  echo
  echo "---"
  echo
  for f in "${docs[@]}"; do
    echo
    echo "======================================================================"
    echo "# ДОКУМЕНТ: $f"
    echo "======================================================================"
    echo
    cat "docs/$f"
    echo
  done
} > docs/ALL-DOCS.md

echo "OK: docs/ALL-DOCS.md собрано ($(wc -l < docs/ALL-DOCS.md) строк, $(wc -c < docs/ALL-DOCS.md) байт)"