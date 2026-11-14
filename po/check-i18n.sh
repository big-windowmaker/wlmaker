#!/bin/sh
# Checks the translations of wlmaker (domain "wlmaker"). Run by ctest as
# `i18n_check`; also by hand: po/check-i18n.sh SOURCE_DIR [WORK_DIR]
#
# 1. po/POTFILES lists every source file with marked texts, and nothing that
#    is missing.
# 2. po/wlmaker.pot is up to date: Regenerated, it has the same texts.
# 3. A text handed to a function that shows it (menus, dialogs, titles,
#    balloons, cairo_show_text) is marked: A bare "Text" there fails, unless
#    the line says `i18n: not translated` (with the reason).
# 4. Each catalog of po/LINGUAS passes `msgfmt --check`, and is merged with
#    the template (no text of the template missing in it).
#
# Fails loudly: A missing input is an error, never a skipped check.
set -eu

src=${1:?usage: check-i18n.sh SOURCE_DIR [WORK_DIR]}
work=${2:-}
po="$src/po"
fail=0
err() { echo "check-i18n: $*" >&2; fail=1; }

for f in POTFILES LINGUAS wlmaker.pot update-pot.sh extract-plist.awk; do
    if [ ! -f "$po/$f" ]; then
        echo "check-i18n: missing $po/$f" >&2
        exit 1
    fi
done
for t in xgettext msgfmt msgcat msguniq msgcmp awk grep; do
    command -v "$t" > /dev/null || { echo "check-i18n: needs $t" >&2; exit 1; }
done

if [ -z "$work" ]; then
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT INT TERM
fi
mkdir -p "$work/check"
tmp="$work/check"

# Listed files, without comments.
grep -v -e '^#' -e '^[[:space:]]*$' "$po/POTFILES" | sort > "$tmp/listed"
if [ ! -s "$tmp/listed" ]; then
    err "po/POTFILES lists no file"
fi

# 1. Files with marked texts. src/i18n.c (the translating function and its
# test) has none of its own.
(
    cd "$src"
    grep -rlE 'wlmaker_i18n\(|N_\(|dgettext\("wlmaker"|WLMTOOL_I18N\(' \
        --include='*.c' src apps tool 2>/dev/null || true
) | grep -v -x 'src/i18n.c' | sort > "$tmp/marked"
if [ ! -s "$tmp/marked" ]; then
    err "found no source file with marked texts below $src: wrong directory?"
fi
missing=$(comm -23 "$tmp/marked" "$tmp/listed")
if [ -n "$missing" ]; then
    err "files with marked texts, missing in po/POTFILES:"
    printf '%s\n' "$missing" | sed 's/^/  /' >&2
fi
while IFS= read -r f; do
    [ -f "$src/$f" ] || err "po/POTFILES lists $f, which is missing"
done < "$tmp/listed"

# 2. The template, regenerated, against the committed one.
if sh "$po/update-pot.sh" "$src" "$tmp/fresh.pot"; then
    msgcat --no-location --no-wrap --sort-output -o "$tmp/a.pot" "$po/wlmaker.pot"
    msgcat --no-location --no-wrap --sort-output -o "$tmp/b.pot" "$tmp/fresh.pot"
    grep -E '^(msgctxt|msgid|msgid_plural) ' "$tmp/a.pot" > "$tmp/a.ids" || true
    grep -E '^(msgctxt|msgid|msgid_plural) ' "$tmp/b.pot" > "$tmp/b.ids" || true
    if ! cmp -s "$tmp/a.ids" "$tmp/b.ids"; then
        err "po/wlmaker.pot is out of date (run: make wlmaker-pot wlmaker-update-po):"
        diff "$tmp/a.ids" "$tmp/b.ids" | grep '^[<>]' | sed 's/^/  /' >&2 || true
    fi
else
    err "update-pot.sh failed"
fi

# 3. Bare texts handed to functions that show them. Statements are joined up
# to their ';'; unit tests (after "== Unit tests") and whole-line comments are left out.
sinks='wlmaker_context_menu_create|wlmaker_context_menu_add_item|wlmaker_context_menu_add_submenu|wlmaker_context_menu_add_option|_wlmaker_dock_menu_add|_wlmaker_appicons_menu_add|_wlmdock_menu_add|wlmtk_menu_item_set_text|wlmtk_window_set_title|wlmaker_dialog_confirm|wlmaker_dialog_confirm_with_icon|_wlmaker_force_close_notice|wlmaker_dock_fx_balloon_enter|_wlmaker_root_menu_create_disabled_item|cairo_show_text|wlmtool_menu_create|wlmtool_menu_get_or_create_submenu'
(
    cd "$src"
    find src apps tool -name '*.c' | sort
) > "$tmp/all-c"
while IFS= read -r f; do
    awk -v file="$f" -v sinks="$sinks" '
        /^\/\* == Unit [Tt]ests/ { exit }
        # Whole-line comments: Left out of the statement.
        /^[ \t]*\/\// { if (open && $0 ~ /i18n: not translated/) ok = 1; next }
        {
            if (!open && match($0, "(^|[^A-Za-z0-9_])(" sinks ")[ \t]*\\(")) {
                open = 1; start = FNR; stmt = ""
            }
            if (open) {
                stmt = stmt " " $0
                if ($0 ~ /;[ \t]*(\/\/.*|\/\*.*)?$/ || $0 ~ /\{[ \t]*$/) {
                    open = 0
                    if (ok || stmt ~ /i18n: not translated/) { ok = 0; next }
                    s = stmt
                    # Comments, then the marked texts.
                    gsub(/\/\/[^"]*$/, "", s)
                    gsub(/(wlmaker_i18n|N_|WLMTOOL_I18N)[ \t]*\([ \t]*("([^"\\]|\\.)*"[ \t]*)+\)/, "", s)
                    gsub(/dgettext[ \t]*\([^,]*,[ \t]*("([^"\\]|\\.)*"[ \t]*)+\)/, "", s)
                    # Keys looked up in a dict: Not shown.
                    gsub(/(wlmaker_dock_launcher_flag|wlmaker_dock_launcher_value|bspl_dict_get_[a-z_]+)[ \t]*\([^()]*\)/, "", s)
                    if (match(s, /"[^"]*[A-Za-z]{2,}[^"]*"/)) {
                        printf("%s:%d: bare text %s\n", file, start,
                               substr(s, RSTART, RLENGTH))
                    }
                }
            }
        }' "$src/$f"
done < "$tmp/all-c" > "$tmp/bare"
if [ -s "$tmp/bare" ]; then
    err "texts shown to the user without wlmaker_i18n()/N_() (or an 'i18n: not translated' comment):"
    sed 's/^/  /' "$tmp/bare" >&2
fi

# 4. The catalogs.
grep -v -e '^#' -e '^[[:space:]]*$' "$po/LINGUAS" > "$tmp/linguas" || true
[ -s "$tmp/linguas" ] || err "po/LINGUAS lists no language"
while IFS= read -r lang; do
    p="$po/$lang.po"
    if [ ! -f "$p" ]; then
        err "po/LINGUAS lists $lang, but $p is missing"
        continue
    fi
    msgfmt --check -o /dev/null "$p" || err "msgfmt --check failed for $p"
    # Every text of the template is in the catalog (translated or not).
    msgcmp --use-untranslated "$p" "$po/wlmaker.pot" 2> "$tmp/msgcmp" ||
        { err "$p is not merged with po/wlmaker.pot (run: make wlmaker-update-po)"
          head -20 "$tmp/msgcmp" >&2; }
done < "$tmp/linguas"

if [ "$fail" -ne 0 ]; then
    echo "check-i18n: FAILED" >&2
    exit 1
fi
echo "check-i18n: OK ($(wc -l < "$tmp/listed") files, $(grep -c '^msgid ' "$po/wlmaker.pot") entries in the template)"
