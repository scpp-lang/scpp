#!/usr/bin/env bash
# Builds the SCPP26 Language Standard (docs/spec/) into HTML and PDF.
#
# Requires `pandoc` on PATH, and a PDF engine pandoc can drive -- this
# script defaults to `typst` (a lightweight, single-binary alternative
# to a full LaTeX install; https://typst.app). Override with
# PDF_ENGINE=xelatex (or another engine of your choice) if you have one
# installed instead.
#
# Usage:
#   ./build.sh          # builds both en and zh
#   ./build.sh en       # builds only English
#   ./build.sh zh       # builds only Chinese

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/dist"
PDF_ENGINE="${PDF_ENGINE:-typst}"

command -v pandoc >/dev/null 2>&1 || {
    echo "error: pandoc not found on PATH" >&2
    exit 1
}

build_lang() {
    local lang="$1"
    local src_dir="$SCRIPT_DIR/$lang"
    local title

    if [ "$lang" = "zh" ]; then
        title="SCPP26 语言标准"
    else
        title="SCPP26 Language Standard"
    fi

    # Numbered clause files only, in filename order; README.md (a table
    # of contents, not clause content) is intentionally excluded.
    local files=()
    while IFS= read -r -d '' f; do
        files+=("$f")
    done < <(find "$src_dir" -maxdepth 1 -name '[0-9][0-9]-*.md' -print0 | sort -z)

    if [ "${#files[@]}" -eq 0 ]; then
        echo "warning: no clause files found in $src_dir, skipping" >&2
        return
    fi

    mkdir -p "$OUT_DIR"

    echo "Building $lang HTML -> $OUT_DIR/scpp26-$lang.html"
    pandoc "${files[@]}" \
        --standalone --toc --toc-depth=3 \
        --metadata title="$title" \
        --metadata lang="$lang" \
        -o "$OUT_DIR/scpp26-$lang.html"

    echo "Building $lang PDF -> $OUT_DIR/scpp26-$lang.pdf (engine: $PDF_ENGINE)"
    pandoc "${files[@]}" \
        --toc --toc-depth=3 \
        --metadata title="$title" \
        --pdf-engine="$PDF_ENGINE" \
        -o "$OUT_DIR/scpp26-$lang.pdf"
}

if [ "$#" -eq 0 ]; then
    build_lang en
    build_lang zh
else
    for lang in "$@"; do
        build_lang "$lang"
    done
fi

echo "Done. Output in $OUT_DIR/"
