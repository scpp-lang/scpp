#!/usr/bin/env bash
set -euo pipefail

OUT_DIR="${1:-$(pwd)/site-dist}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

command -v pandoc >/dev/null 2>&1 || {
    echo "error: pandoc not found on PATH" >&2
    exit 1
}

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}/assets"

cat > "${OUT_DIR}/assets/site.css" <<'CSS'
:root {
    color-scheme: light;
    font-family: Inter, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    line-height: 1.5;
}
body {
    margin: 0 auto;
    max-width: 960px;
    padding: 2rem 1.25rem 4rem;
    color: #1f2937;
}
nav {
    display: flex;
    flex-wrap: wrap;
    gap: 0.75rem;
    margin-bottom: 1.5rem;
}
nav a, a {
    color: #1d4ed8;
    text-decoration: none;
}
nav a:hover, a:hover {
    text-decoration: underline;
}
.card-grid {
    display: grid;
    gap: 1rem;
    grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
}
.card {
    border: 1px solid #d1d5db;
    border-radius: 0.75rem;
    padding: 1rem;
    background: #f9fafb;
}
ul.doc-list {
    padding-left: 1.25rem;
}
code {
    background: #f3f4f6;
    padding: 0.1rem 0.3rem;
    border-radius: 0.25rem;
}
CSS

first_heading() {
    local file="$1"
    local heading
    heading="$(sed -n 's/^# \+//p' "$file" | head -n 1)"
    if [[ -n "${heading}" ]]; then
        printf '%s' "${heading}"
    else
        basename "$file" .md
    fi
}

render_lang_tree() {
    local section="$1"
    local lang="$2"
    local src_dir="${REPO_ROOT}/docs/${section}/${lang}"
    local out_dir="${OUT_DIR}/${section}/${lang}"

    mkdir -p "${out_dir}"

    local items=()
    while IFS= read -r -d '' file; do
        items+=("$file")
    done < <(find "${src_dir}" -maxdepth 1 -type f -name '*.md' ! -name 'README.md' -print0 | sort -z)

    for file in "${items[@]}"; do
        local stem title
        stem="$(basename "$file" .md)"
        title="$(first_heading "$file")"
        pandoc "$file" \
            --standalone \
            --toc \
            --metadata title="$title" \
            --metadata lang="$lang" \
            --css=/assets/site.css \
            -o "${out_dir}/${stem}.html"
    done

    {
        printf '<!doctype html>\n<html lang="%s">\n<head>\n' "$lang"
        printf '  <meta charset="utf-8">\n'
        printf '  <title>SCPP %s (%s)</title>\n' "$section" "$lang"
        printf '  <link rel="stylesheet" href="/assets/site.css">\n'
        printf '</head>\n<body>\n'
        printf '  <nav><a href="/">Home</a><a href="/%s/">%s</a></nav>\n' "$section" "$section"
        printf '  <h1>%s / %s</h1>\n' "$section" "$lang"
        printf '  <p>Generated static site view for the SCPP documentation tree.</p>\n'
        printf '  <ul class="doc-list">\n'
        for file in "${items[@]}"; do
            local stem title
            stem="$(basename "$file" .md)"
            title="$(first_heading "$file")"
            printf '    <li><a href="/%s/%s/%s.html">%s</a></li>\n' "$section" "$lang" "$stem" "$title"
        done
        printf '  </ul>\n'
        printf '</body>\n</html>\n'
    } > "${out_dir}/index.html"
}

render_section_index() {
    local section="$1"
    mkdir -p "${OUT_DIR}/${section}"
    cat > "${OUT_DIR}/${section}/index.html" <<EOF2
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>SCPP ${section}</title>
  <link rel="stylesheet" href="/assets/site.css">
</head>
<body>
  <nav><a href="/">Home</a></nav>
  <h1>SCPP ${section}</h1>
  <div class="card-grid">
    <div class="card"><h2><a href="/${section}/en/">English</a></h2><p>Browse the English ${section} pages.</p></div>
    <div class="card"><h2><a href="/${section}/zh/">中文</a></h2><p>浏览中文 ${section} 页面。</p></div>
  </div>
</body>
</html>
EOF2
}

for section in book spec standards; do
    render_section_index "$section"
    render_lang_tree "$section" en
    render_lang_tree "$section" zh
done

cat > "${OUT_DIR}/index.html" <<'EOF2'
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>SCPP project site</title>
  <link rel="stylesheet" href="/assets/site.css">
</head>
<body>
  <h1>SCPP project site</h1>
  <p>Static documentation site packaged for the scpp httpserver.</p>
  <div class="card-grid">
    <div class="card"><h2><a href="/book/">Book</a></h2><p>Language narrative and guides in English and Chinese.</p></div>
    <div class="card"><h2><a href="/spec/">Specification</a></h2><p>Formal language specification pages in English and Chinese.</p></div>
    <div class="card"><h2><a href="/standards/">Standards</a></h2><p>Packaging and module-format standards in English and Chinese.</p></div>
  </div>
</body>
</html>
EOF2

echo "Built site into ${OUT_DIR}"
