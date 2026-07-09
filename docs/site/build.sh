#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT_DIR="$(cd "$DOCS_DIR/.." && pwd)"
OUT_DIR="$SCRIPT_DIR/dist"
TMP_DIR="$SCRIPT_DIR/.build"
FILTER="$SCRIPT_DIR/filters/rewrite-links.lua"
CSS_SRC="$SCRIPT_DIR/assets/site.css"
LOGO_SVG_SRC="$ROOT_DIR/assets/logo/scpp-logo.svg"
LOGO_PNG_SRC="$ROOT_DIR/assets/logo/scpp-logo.png"
SITE_TITLE="SCPP Documentation"
GITHUB_REPO_URL="https://github.com/scpp-lang/scpp"

command -v pandoc >/dev/null 2>&1 || {
  echo "error: pandoc not found on PATH" >&2
  exit 1
}

rm -rf "$OUT_DIR" "$TMP_DIR"
mkdir -p "$OUT_DIR/assets" "$TMP_DIR"
cp "$CSS_SRC" "$OUT_DIR/assets/site.css"
cp "$LOGO_SVG_SRC" "$OUT_DIR/assets/scpp-logo.svg"
cp "$LOGO_PNG_SRC" "$OUT_DIR/assets/scpp-logo.png"

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

site_note_text() {
  case "$1" in
    zh) printf '本网站由使用 scpp 编写的 HTTP server 提供服务。' ;;
    en) printf 'This site is served by an HTTP server written in scpp.' ;;
    *) printf 'This site is served by an HTTP server written in scpp. / 本网站由使用 scpp 编写的 HTTP server 提供服务。' ;;
  esac
}

section_label() {
  local section="$1"
  local lang="${2:-en}"
  case "$section:$lang" in
    book:zh) printf 'Book' ;;
    spec:zh) printf 'Spec' ;;
    book:*) printf 'Book' ;;
    spec:*) printf 'Spec' ;;
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
      find "$DOCS_DIR/spec/$lang" -maxdepth 1 \( -name 'scppm-format.md' -o -name 'scppkg-format.md' \) -printf '%f\n' | sort
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

  sidebar_entry() {
    local file="$1"
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
      title="$file"
    fi
    printf '    <li><a%s href="%s">%s</a></li>\n' "$class_name" "$href" "$(html_escape "$title")"
  }

  {
    printf '<div class="sidebar">\n'
    printf '  <h2>%s · %s</h2>\n' "$(html_escape "$(section_label "$section" "$lang")")" "$(html_escape "$(lang_label "$lang")")"
    if [ "$section" = 'spec' ]; then
      local standard_group_label format_group_label
      if [ "$lang" = 'zh' ]; then
        standard_group_label='语言标准'
        format_group_label='文件格式规范'
      else
        standard_group_label='Language Standard'
        format_group_label='File-Format Specifications'
      fi
      printf '  <ul class="sidebar-root">\n'
      sidebar_entry 'README.md'
      printf '    <li class="sidebar-group">\n'
      printf '      <div class="sidebar-group-title">%s</div>\n' "$(html_escape "$standard_group_label")"
      printf '      <ul class="sidebar-sublist">\n'
      while IFS= read -r file; do
        [ -n "$file" ] || continue
        sidebar_entry "$file"
      done < <(find "$source_dir" -maxdepth 1 -name '[0-9][0-9]-*.md' -printf '%f\n' | sort)
      printf '      </ul>\n'
      printf '    </li>\n'
      printf '    <li class="sidebar-group">\n'
      printf '      <div class="sidebar-group-title">%s</div>\n' "$(html_escape "$format_group_label")"
      printf '      <ul class="sidebar-sublist">\n'
      while IFS= read -r file; do
        [ -n "$file" ] || continue
        sidebar_entry "$file"
      done < <(find "$source_dir" -maxdepth 1 \( -name 'scppm-format.md' -o -name 'scppkg-format.md' \) -printf '%f\n' | sort)
      printf '      </ul>\n'
      printf '    </li>\n'
      printf '  </ul>\n'
    else
      printf '  <ul>\n'
      while IFS= read -r file; do
        [ -n "$file" ] || continue
        sidebar_entry "$file"
      done < <(section_files "$section" "$lang")
      printf '  </ul>\n'
    fi
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
    for section in book spec; do
      href="${prefix}${section}/${current_lang}/index.html"
      class_name=''
      if [ "$section" = "$current_section" ]; then
        class_name=' class="current"'
      fi
      printf '  <a%s href="%s">%s</a>\n' "$class_name" "$href" "$(section_label "$section" "$current_lang")"
    done
    printf '  <a href="%s">GitHub</a>\n' "$GITHUB_REPO_URL"
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
    printf '    <div class="site-note">%s</div>\n' "$(html_escape "$(site_note_text "$lang")")"
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

write_head_includes() {
  local rel_out="$1"
  local out_file="$2"
  local prefix
  prefix="$(root_prefix_for "$rel_out")"
  {
    printf '<link rel="icon" type="image/svg+xml" href="%sassets/scpp-logo.svg">\n' "$prefix"
    printf '<link rel="icon" type="image/png" href="%sassets/scpp-logo.png">\n' "$prefix"
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
  local out_dir before after head css_rel
  out_dir="$(dirname "$out_path")"
  mkdir -p "$out_dir"
  before="$TMP_DIR/before-${section}-${lang}-${source_name}.html"
  after="$TMP_DIR/after-${section}-${lang}-${source_name}.html"
  head="$TMP_DIR/head-${section}-${lang}-${source_name}.html"
  css_rel="$(root_prefix_for "$rel_out")assets/site.css"
  write_before_body "$rel_out" "$section" "$lang" "$source_name" "$title" "$title" "$before"
  write_after_body "$after"
  write_head_includes "$rel_out" "$head"
  pandoc "$source_path" \
    --standalone \
    --lua-filter="$FILTER" \
    --css="$css_rel" \
    --include-in-header="$head" \
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

build_landing() {
  local md="$TMP_DIR/index.md"
  cat > "$md" <<'LANDING'
# SCPP Documentation Site

Browse the language book and the formal specifications, including the language standard and the `.scppm` / `.scppkg` format specs.

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
</div>
LANDING
  local before="$TMP_DIR/before-home.html"
  local after="$TMP_DIR/after-home.html"
  local head="$TMP_DIR/head-home.html"
  cat > "$before" <<'EOF2'
<header class="site-header">
  <div class="site-header-inner">
    <a class="site-title" href="index.html">SCPP Documentation</a>
    <nav class="top-nav">
      <a class="current" href="index.html">Home</a>
      <a href="book/en/index.html">Book</a>
      <a href="spec/en/index.html">Spec</a>
      <a href="https://github.com/scpp-lang/scpp">GitHub</a>
    </nav>
    <div class="lang-switcher">
      <a href="book/en/index.html">EN</a>
      <a href="book/zh/index.html">中文</a>
    </div>
    <div class="site-note">This site is served by an HTTP server written in scpp. / 本网站由使用 scpp 编写的 HTTP server 提供服务。</div>
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
  write_head_includes "index.html" "$head"
  pandoc "$md" \
    --standalone \
    --lua-filter="$FILTER" \
    --css="assets/site.css" \
    --include-in-header="$head" \
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
done

echo "Done. Output in $OUT_DIR/"
