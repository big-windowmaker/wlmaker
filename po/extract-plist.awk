# Extracts the texts of a dock-app menu schema (share/wlmaker/Dockapps.plist)
# for the gettext template: Each `Title = "..."`, and the title of each choice
# `(Value, "Title")`. Writes PO entries, one per text, with all its places.
#
# Usage: awk -v ref=share/wlmaker/Dockapps.plist -f extract-plist.awk FILE
#
# Titles must be quoted strings: An unquoted `Title = Word;` is reported on
# stderr and makes awk exit with status 1, so the extraction fails loudly
# instead of leaving the text out of the template.

function add(text, line) {
    if (text == "") return
    if (!(text in refs)) {
        order[++n] = text
        refs[text] = ref ":" line
    } else {
        refs[text] = refs[text] " " ref ":" line
    }
}

function po_escape(s) {
    gsub(/\\/, "\\\\", s)
    gsub(/"/, "\\\"", s)
    return s
}

{
    line = $0
    # Comments: The whole line, or after "//" outside of a string (the
    # schema has no "//" inside its strings).
    sub(/^[ \t]*\/\/.*/, "", line)
    sub(/[ \t]\/\/.*$/, "", line)

    rest = line
    while (match(rest, /Title[ \t]*=[ \t]*"[^"]*"/)) {
        m = substr(rest, RSTART, RLENGTH)
        sub(/^Title[ \t]*=[ \t]*"/, "", m)
        sub(/"$/, "", m)
        add(m, NR)
        rest = substr(rest, RSTART + RLENGTH)
    }
    if (match(line, /Title[ \t]*=[ \t]*[^" \t]/)) {
        printf("%s:%d: unquoted Title, not extracted\n", ref, NR) > "/dev/stderr"
        failed = 1
    }

    # Choices: (Value, "Title"), the value quoted or not.
    rest = line
    while (match(rest, /\([ \t]*("[^"]*"|[A-Za-z0-9_.-]+)[ \t]*,[ \t]*"[^"]*"[ \t]*\)/)) {
        m = substr(rest, RSTART, RLENGTH)
        sub(/^.*,[ \t]*"/, "", m)
        sub(/"[ \t]*\)$/, "", m)
        add(m, NR)
        rest = substr(rest, RSTART + RLENGTH)
    }
}

END {
    for (i = 1; i <= n; ++i) {
        t = order[i]
        print "#. Dock-app menu, from the dock-apps' preferences schema."
        print "#: " refs[t]
        print "msgid \"" po_escape(t) "\""
        print "msgstr \"\""
        print ""
    }
    exit failed
}
