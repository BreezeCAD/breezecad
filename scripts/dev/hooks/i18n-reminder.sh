#!/usr/bin/env bash
# Claude Code PostToolUse hook: if an edit to src/ adds interface strings
# (Tr("…"), Trf("…"), N_("…")), remind Claude to refresh the translation template.
# Reads the hook JSON from stdin. Exit 2 = the message on stderr goes to Claude.
input=$(cat)
printf '%s' "$input" | grep -Eq '"file_path" *: *"[^"]*src(/|\\\\)' || exit 0
printf '%s' "$input" | grep -Eq '(Tr|Trf|N_)\(\\"' || exit 0
echo "В правке новые строки интерфейса (Tr/Trf/N_). Перед коммитом обнови шаблон переводов: python3 scripts/i18n/extract.py (на Windows — python) и закоммить translations/template.json." >&2
exit 2
