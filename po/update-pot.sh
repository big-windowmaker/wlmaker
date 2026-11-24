#!/bin/sh
# Generates the gettext template of wlmaker (domain "wlmaker").
#
# Usage: po/update-pot.sh SOURCE_DIR OUTPUT_POT
#
# The C files listed in po/POTFILES go through xgettext, with the keywords of
# wlmaker: wlmaker_i18n() translates, N_() marks a text in a table of
# constants, dgettext("wlmaker", ...) translates in the toolkit, and
# WLMTOOL_I18N() in wlmtool (neither links wlmaker's i18n.c). The default
# keywords are off (-k): A gettext() call of another domain must not end up
# here.
#
# The plist files listed (the dock-apps' preferences schema) go through
# extract-plist.awk: Their menu titles are translated by wlmaker at runtime.
#
# Fails loudly when POTFILES, or a file it lists, is missing.
set -eu

src=${1:?usage: update-pot.sh SOURCE_DIR OUTPUT_POT}
out=${2:?usage: update-pot.sh SOURCE_DIR OUTPUT_POT}
potfiles="$src/po/POTFILES"

if [ ! -f "$potfiles" ]; then
    echo "update-pot.sh: missing $potfiles" >&2
    exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

: > "$tmp/c-files"
: > "$tmp/plist.po"
status=0
while IFS= read -r f || [ -n "$f" ]; do
    case "$f" in
        ''|'#'*) continue ;;
    esac
    if [ ! -f "$src/$f" ]; then
        echo "update-pot.sh: $f is listed in po/POTFILES, but missing" >&2
        status=1
        continue
    fi
    case "$f" in
        *.plist)
            awk -v ref="$f" -f "$src/po/extract-plist.awk" "$src/$f" \
                >> "$tmp/plist.po" || status=1 ;;
        *)
            printf '%s\n' "$f" >> "$tmp/c-files" ;;
    esac
done < "$potfiles"
[ "$status" -eq 0 ] || exit "$status"

# The plist texts as a PO file: xgettext reads it as one more input.
{
    printf 'msgid ""\nmsgstr ""\n'
    printf '"Content-Type: text/plain; charset=UTF-8\\n"\n\n'
    cat "$tmp/plist.po"
} > "$tmp/Dockapps.po"
msguniq --use-first -o "$tmp/Dockapps-uniq.po" "$tmp/Dockapps.po"

xgettext \
    --directory="$src" \
    --files-from="$tmp/c-files" \
    --from-code=UTF-8 \
    --language=C \
    -k \
    --keyword=wlmaker_i18n \
    --keyword=N_ \
    --keyword=dgettext:2 \
    --keyword=WLMTOOL_I18N \
    --flag=wlmaker_i18n:1:pass-c-format \
    --flag=bs_strdupf:1:c-format \
    --add-comments=TRANSLATORS: \
    --package-name=wlmaker \
    --msgid-bugs-address=https://github.com/phkaeser/wlmaker/issues \
    --copyright-holder="wlmaker contributors" \
    -o "$tmp/c.pot"

# Merges both; the C file's header wins. Then the header's charset, which
# xgettext leaves as CHARSET for an ASCII-only template.
msgcat --use-first --sort-by-file -o "$tmp/all.pot" "$tmp/c.pot" "$tmp/Dockapps-uniq.po"
sed -e 's/charset=CHARSET/charset=UTF-8/' \
    -e '1s/.*/# Texts of wlmaker, the gettext domain "wlmaker"./' \
    -e '/^"POT-Creation-Date: /d' "$tmp/all.pot" > "$out"
