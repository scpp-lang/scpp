#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT_DIR="$(cd "$DOCS_DIR/.." && pwd)"
OUT_DIR="$SCRIPT_DIR/dist"
TMP_DIR="$SCRIPT_DIR/.build"
FILTER="$SCRIPT_DIR/filters/rewrite-links.lua"
CSS_SRC="$SCRIPT_DIR/assets/site.css"
SITE_TITLE="SCPP Documentation"

command -v pandoc >/dev/null 2>&1 || {
  echo "error: pandoc not found on PATH" >&2
  exit 1
}

rm -rf "$OUT_DIR" "$TMP_DIR"
mkdir -p "$OUT_DIR/assets" "$TMP_DIR"
cp "$CSS_SRC" "$OUT_DIR/assets/site.css"

cleanup() {
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

html_escape() {
  local s="$1"
  s=${s//&/&amp;}
  s=${s//</&lt;}
  s=${s//>/&gt;}
  printf '%s' "$s"
}

first_heading() {
  local file="$1"
  sed -n 's/^# //p' "$file" | head -n 1
}

root_prefix_for() {
  local rel="$1"
  local dir
  dir="$(dirname "$rel")"
  if [ "$dir" = "." ]; then
    printf ''
    return
  fi
  local prefix=""
  IFS='/' read -r -a parts <<< "$dir"
  for _ in "${parts[@]}"; do
    prefix+="../"
  done
  printf '%s' "$prefix"
}

lang_label() {
  case "$1" in
    en) printf 'English' ;;
    zh) printf '中文' ;;
    *) printf '%s' "$1" ;;
  esac
}

section_label() {
  local section="$1"
  local lang="${2:-en}"
  case "$section:$lang" in
    book:zh) printf 'Book' ;;
    spec:zh) printf 'Spec' ;;
    standards:zh) printf 'Standards' ;;
    book:*) printf 'Book' ;;
    spec:*) printf 'Spec' ;;
    standards:*) printf 'Standards' ;;
    *) printf '%s' "$section" ;;
  esac
}

page_title_for() {
  local source="$1"
  if [ -f "$source" ]; then
    first_heading "$source"
  else
    printf ''
  fi
}

section_files() {
  local section="$1"
  local lang="$2"
  case "$section" in
    book)
      printf 'README.md\n'
      find "$DOCS_DIR/book/$lang" -maxdepth 1 -name 'ch*.md' -printf '%f\n' | sort
      ;;
    spec)
      printf 'README.md\n'
      find "$DOCS_DIR/spec/$lang" -maxdepth 1 -name '[0-9][0-9]-*.md' -printf '%f\n' | sort
      ;;
    standards)
      find "$DOCS_DIR/standards/$lang" -maxdepth 1 -name '*.md' -printf '%f\n' | sort
      ;;
  esac
}

output_name_for() {
  local filename="$1"
  if [ "$filename" = 'README.md' ]; then
    printf 'index.html'
  else
    printf '%s' "${filename%.md}.html"
  fi
}

build_sidebar() {
  local section="$1"
  local lang="$2"
  local current_out="$3"
  local prefix="$4"
  local sidebar_file="$5"
  local source_dir="$DOCS_DIR/$section/$lang"

  {
    printf '<div class="sidebar">\n'
    printf '  <h2>%s · %s</h2>\n' "$(html_escape "$(section_label "$section" "$lang")")" "$(html_escape "$(lang_label "$lang")")"
    printf '  <ul>\n'
    while IFS= read -r file; do
      [ -n "$file" ] || continue
      local out_name title href class_name source_path
      out_name="$(output_name_for "$file")"
      href="$prefix$section/$lang/$out_name"
      class_name=''
      if [ "$out_name" = "$current_out" ]; then
        class_name=' class="current"'
      fi
      source_path="$source_dir/$file"
      if [ -f "$source_path" ]; then
        title="$(page_title_for "$source_path")"
      else
        if [ "$section" = 'standards' ]; then
          if [ "$lang" = 'zh' ]; then
            title='格式标准总览'
          else
            title='Format Standards'
          fi
        else
          title="$file"
        fi
      fi
      printf '    <li><a%s href="%s">%s</a></li>\n' "$class_name" "$href" "$(html_escape "$title")"
    done < <(if [ "$section" = 'standards' ]; then printf 'README.md\n'; fi; section_files "$section" "$lang")
    printf '  </ul>\n'
    printf '</div>\n'
  } > "$sidebar_file"
}

build_top_nav() {
  local current_section="$1"
  local current_lang="$2"
  local prefix="$3"
  local file="$4"
  {
    printf '<nav class="top-nav">\n'
    local href class_name section
    href="${prefix}index.html"
    printf '  <a href="%s">Home</a>\n' "$href"
    for section in book spec standards; do
      href="${prefix}${section}/${current_lang}/index.html"
      class_name=''
      if [ "$section" = "$current_section" ]; then
        class_name=' class="current"'
      fi
      printf '  <a%s href="%s">%s</a>\n' "$class_name" "$href" "$(section_label "$section" "$current_lang")"
    done
    printf '</nav>\n'
  } > "$file"
}

build_lang_switcher() {
  local section="$1"
  local lang="$2"
  local current_source_name="$3"
  local prefix="$4"
  local file="$5"
  local other_lang='en'
  [ "$lang" = 'en' ] && other_lang='zh'
  local other_source="$DOCS_DIR/$section/$other_lang/$current_source_name"
  local other_target="$prefix$section/$other_lang/$(output_name_for "$current_source_name")"
  if [ ! -f "$other_source" ]; then
    other_target="$prefix$section/$other_lang/index.html"
  fi
  {
    printf '<div class="lang-switcher">\n'
    if [ "$lang" = 'en' ]; then
      printf '  <span class="current">EN</span>\n'
      printf '  <a href="%s">中文</a>\n' "$other_target"
    else
      printf '  <a href="%s">EN</a>\n' "$other_target"
      printf '  <span class="current">中文</span>\n'
    fi
    printf '</div>\n'
  } > "$file"
}

write_before_body() {
  local rel_out="$1"
  local section="$2"
  local lang="$3"
  local source_name="$4"
  local source_title="$5"
  local page_title="$6"
  local out_file="$7"
  local prefix top_nav sidebar switcher
  prefix="$(root_prefix_for "$rel_out")"
  top_nav="$TMP_DIR/top-nav-${section}-${lang}-${source_name}.html"
  sidebar="$TMP_DIR/sidebar-${section}-${lang}-${source_name}.html"
  switcher="$TMP_DIR/lang-${section}-${lang}-${source_name}.html"
  build_top_nav "$section" "$lang" "$prefix" "$top_nav"
  build_sidebar "$section" "$lang" "$(output_name_for "$source_name")" "$prefix" "$sidebar"
  build_lang_switcher "$section" "$lang" "$source_name" "$prefix" "$switcher"
  {
    printf '<header class="site-header">\n'
    printf '  <div class="site-header-inner">\n'
    printf '    <a class="site-title" href="%sindex.html">%s</a>\n' "$prefix" "$SITE_TITLE"
    cat "$top_nav"
    cat "$switcher"
    printf '  </div>\n'
    printf '</header>\n'
    printf '<div class="page-shell with-sidebar">\n'
    cat "$sidebar"
    printf '  <main class="content">\n'
    printf '    <div class="content-header">\n'
    printf '      <div class="breadcrumbs"><a href="%sindex.html">Home</a> / <a href="%s%s/%s/index.html">%s</a> / %s</div>\n' \
      "$prefix" "$prefix" "$section" "$lang" "$(section_label "$section" "$lang")" "$(html_escape "$(lang_label "$lang")")"
    printf '      <div class="meta-row">\n'
    printf '        <div><strong>%s</strong></div>\n' "$(html_escape "$page_title")"
    printf '        <div>%s</div>\n' "$(html_escape "$(lang_label "$lang")")"
    printf '      </div>\n'
    printf '    </div>\n'
  } > "$out_file"
}

write_after_body() {
  local out_file="$1"
  {
    printf '    <div class="footer">Generated from <code>docs/</code> by <code>docs/site/build.sh</code>.</div>\n'
    printf '  </main>\n'
    printf '</div>\n'
  } > "$out_file"
}

render_markdown_page() {
  local source_path="$1"
  local rel_out="$2"
  local section="$3"
  local lang="$4"
  local source_name="$5"
  local title="$6"
  local out_path="$OUT_DIR/$rel_out"
  local out_dir before after css_rel
  out_dir="$(dirname "$out_path")"
  mkdir -p "$out_dir"
  before="$TMP_DIR/before-${section}-${lang}-${source_name}.html"
  after="$TMP_DIR/after-${section}-${lang}-${source_name}.html"
  css_rel="$(root_prefix_for "$rel_out")assets/site.css"
  write_before_body "$rel_out" "$section" "$lang" "$source_name" "$title" "$title" "$before"
  write_after_body "$after"
  pandoc "$source_path" \
    --standalone \
    --lua-filter="$FILTER" \
    --css="$css_rel" \
    --include-before-body="$before" \
    --include-after-body="$after" \
    --metadata pagetitle="$title" \
    --metadata lang="$lang" \
    -o "$out_path"
}

build_book_or_spec() {
  local section="$1"
  local lang="$2"
  local source_dir="$DOCS_DIR/$section/$lang"
  while IFS= read -r file; do
    [ -n "$file" ] || continue
    local source="$source_dir/$file"
    local out_name title rel_out
    out_name="$(output_name_for "$file")"
    rel_out="$section/$lang/$out_name"
    title="$(page_title_for "$source")"
    render_markdown_page "$source" "$rel_out" "$section" "$lang" "$file" "$title"
  done < <(section_files "$section" "$lang")
}

standards_index_markdown() {
  local lang="$1"
  local out_file="$2"
  if [ "$lang" = 'zh' ]; then
    printf '# 格式标准\n\n' > "$out_file"
    printf 'SCPP 的二进制/打包格式规范。\n\n' >> "$out_file"
  else
    printf '# Format Standards\n\n' > "$out_file"
    printf 'Binary and packaging format specifications for SCPP.\n\n' >> "$out_file"
  fi
  printf '## Documents\n\n' >> "$out_file"
  while IFS= read -r file; do
    [ -n "$file" ] || continue
    local source="$DOCS_DIR/standards/$lang/$file"
    local title out_name
    title="$(page_title_for "$source")"
    out_name="$(output_name_for "$file")"
    printf -- '- [%s](%s)\n' "$title" "$out_name" >> "$out_file"
  done < <(section_files standards "$lang")
}

build_standards() {
  local lang="$1"
  local section='standards'
  local source_dir="$DOCS_DIR/$section/$lang"
  local index_md="$TMP_DIR/standards-$lang-index.md"
  standards_index_markdown "$lang" "$index_md"
  render_markdown_page "$index_md" "$section/$lang/index.html" "$section" "$lang" 'README.md' "$(page_title_for "$index_md")"
  while IFS= read -r file; do
    [ -n "$file" ] || continue
    local source="$source_dir/$file"
    local title rel_out
    title="$(page_title_for "$source")"
    rel_out="$section/$lang/$(output_name_for "$file")"
    render_markdown_page "$source" "$rel_out" "$section" "$lang" "$file" "$title"
  done < <(section_files standards "$lang")
}

build_landing() {
  local md="$TMP_DIR/index.md"
  cat > "$md" <<'LANDING'
# SCPP Documentation Site

Browse the language book, formal spec, and wire/package format standards.

<div class="landing-grid">
  <section class="card">
    <h2>Book</h2>
    <ul>
      <li><a href="book/en/index.html">English</a></li>
      <li><a href="book/zh/index.html">中文</a></li>
    </ul>
  </section>
  <section class="card">
    <h2>Spec</h2>
    <ul>
      <li><a href="spec/en/index.html">English</a></li>
      <li><a href="spec/zh/index.html">中文</a></li>
    </ul>
  </section>
  <section class="card">
    <h2>Standards</h2>
    <ul>
      <li><a href="standards/en/index.html">English</a></li>
      <li><a href="standards/zh/index.html">中文</a></li>
    </ul>
  </section>
</div>
LANDING
  local before="$TMP_DIR/before-home.html"
  local after="$TMP_DIR/after-home.html"
  cat > "$before" <<'EOF2'
<header class="site-header">
  <div class="site-header-inner">
    <a class="site-title" href="index.html">SCPP Documentation</a>
    <nav class="top-nav">
      <a class="current" href="index.html">Home</a>
      <a href="book/en/index.html">Book</a>
      <a href="spec/en/index.html">Spec</a>
      <a href="standards/en/index.html">Standards</a>
    </nav>
    <div class="lang-switcher">
      <a href="book/en/index.html">EN</a>
      <a href="book/zh/index.html">中文</a>
    </div>
  </div>
</header>
<div class="page-shell">
  <main class="content">
EOF2
  cat > "$after" <<'EOF2'
    <div class="footer">Generated from <code>docs/</code> by <code>docs/site/build.sh</code>.</div>
  </main>
</div>
EOF2
  pandoc "$md" \
    --standalone \
    --lua-filter="$FILTER" \
    --css="assets/site.css" \
    --include-before-body="$before" \
    --include-after-body="$after" \
    --metadata pagetitle="SCPP Documentation Site" \
    --metadata lang="en" \
    -o "$OUT_DIR/index.html"
}

build_landing
for lang in en zh; do
  build_book_or_spec book "$lang"
  build_book_or_spec spec "$lang"
  build_standards "$lang"
done

echo "Done. Output in $OUT_DIR/"
