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
SITE_BASE_URL="${SITE_BASE_URL:-https://www.scpp-lang.org}"

SITE_BASE_URL="${SITE_BASE_URL%/}"
case "$SITE_BASE_URL" in
  http://*|https://*) ;;
  *)
    echo "error: SITE_BASE_URL must start with http:// or https:// (got: $SITE_BASE_URL)" >&2
    exit 1
    ;;
esac

declare -a GENERATED_PAGE_PATHS=()

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
    zh-TW) printf '繁中（台灣）' ;;
    *) printf '%s' "$1" ;;
  esac
}

theme_prefix_label() {
  case "$1" in
    zh) printf '主题' ;;
    zh-TW) printf '佈景主題' ;;
    *) printf 'Theme' ;;
  esac
}

theme_state_label() {
  local state="$1"
  local lang="$2"
  case "$state:$lang" in
    auto:zh) printf '跟随系统' ;;
    light:zh) printf '浅色' ;;
    dark:zh) printf '深色' ;;
    auto:zh-TW) printf '跟隨系統' ;;
    light:zh-TW) printf '淺色' ;;
    dark:zh-TW) printf '深色' ;;
    auto:*) printf 'Auto' ;;
    light:*) printf 'Light' ;;
    dark:*) printf 'Dark' ;;
    *) printf '%s' "$state" ;;
  esac
}

site_note_text() {
  case "$1" in
    zh) printf '本网站由使用 scpp 编写的 HTTP server 提供服务。' ;;
    zh-TW) printf '本網站由以 scpp 撰寫的 HTTP server 提供服務。' ;;
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
    design:zh) printf '设计文档' ;;
    book:zh-TW) printf '書籍' ;;
    spec:zh-TW) printf '規格' ;;
    design:zh-TW) printf '設計文件' ;;
    book:*) printf 'Book' ;;
    spec:*) printf 'Spec' ;;
    design:*) printf 'Design Docs' ;;
    *) printf '%s' "$section" ;;
  esac
}

design_source_root() {
  printf '%s' "$DOCS_DIR/design"
}

has_section() {
  case "$1" in
    book|spec) return 0 ;;
    design) design_source_root >/dev/null 2>&1 ;;
    *) return 1 ;;
  esac
}

section_has_lang() {
  local section="$1"
  local lang="$2"
  [ -d "$(section_source_dir "$section" "$lang")" ]
}

section_languages() {
  local section="$1"
  local lang
  for lang in en zh zh-TW; do
    if section_has_lang "$section" "$lang"; then
      printf '%s\n' "$lang"
    fi
  done
}

fallback_lang_for_section() {
  local section="$1"
  local requested="$2"
  if section_has_lang "$section" "$requested"; then
    printf '%s' "$requested"
    return
  fi
  if [ "$requested" = 'zh-TW' ] && section_has_lang "$section" zh; then
    printf 'zh'
    return
  fi
  if section_has_lang "$section" en; then
    printf 'en'
    return
  fi
  section_languages "$section" | head -n 1
}

section_source_dir() {
  local section="$1"
  local lang="$2"
  case "$section" in
    design)
      printf '%s/%s' "$(design_source_root)" "$lang"
      ;;
    *)
      printf '%s/%s/%s' "$DOCS_DIR" "$section" "$lang"
      ;;
  esac
}

ensure_design_index() {
  local lang="$1"
  local index="$TMP_DIR/design-$lang-README.md"
  local source_dir
  source_dir="$(section_source_dir design "$lang")"
  {
    if [ "$lang" = 'zh' ]; then
      printf '# %s\n\n' "$(section_label design "$lang")"
      printf '浏览当前已公开的 SCPP 设计文档。\n\n'
    elif [ "$lang" = 'zh-TW' ]; then
      printf '# %s\n\n' "$(section_label design "$lang")"
      printf '瀏覽目前已公開的 SCPP 設計文件。\n\n'
    else
      printf '# %s\n\n' "$(section_label design "$lang")"
      printf 'Browse the currently published SCPP design documents.\n\n'
    fi
    printf '## '
    if [ "$lang" = 'zh' ]; then
      printf '文档列表\n\n'
    elif [ "$lang" = 'zh-TW' ]; then
      printf '文件列表\n\n'
    else
      printf 'Available documents\n\n'
    fi
    while IFS= read -r file; do
      [ -n "$file" ] || continue
      local title
      title="$(page_title_for "$source_dir/$file")"
      printf -- '- [%s](%s)\n' "$title" "$file"
    done < <(find "$source_dir" -maxdepth 1 -name '*.md' -printf '%f\n' | sort)
    printf '\n'
  } > "$index"
  printf '%s' "$index"
}

source_path_for() {
  local section="$1"
  local lang="$2"
  local file="$3"
  if [ "$section" = 'design' ] && [ "$file" = 'README.md' ]; then
    ensure_design_index "$lang"
    return
  fi
  printf '%s/%s' "$(section_source_dir "$section" "$lang")" "$file"
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
  local source_dir
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
    design)
      printf 'README.md\n'
      source_dir="$(section_source_dir "$section" "$lang")"
      find "$source_dir" -maxdepth 1 -name '*.md' -printf '%f\n' | sort
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
  local source_dir
  source_dir="$(section_source_dir "$section" "$lang")"

  sidebar_entry() {
    local file="$1"
    local out_name title href class_name source_path
    out_name="$(output_name_for "$file")"
    href="$prefix$section/$lang/$out_name"
    class_name=''
    if [ "$out_name" = "$current_out" ]; then
      class_name=' class="current"'
    fi
    source_path="$(source_path_for "$section" "$lang" "$file")"
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
      elif [ "$lang" = 'zh-TW' ]; then
        standard_group_label='語言標準'
        format_group_label='檔案格式規格'
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
    elif [ "$section" = 'design' ]; then
      local group_label
      if [ "$lang" = 'zh' ]; then
        group_label='设计文档'
      elif [ "$lang" = 'zh-TW' ]; then
        group_label='設計文件'
      else
        group_label='Design Documents'
      fi
      printf '  <ul class="sidebar-root">\n'
      sidebar_entry 'README.md'
      printf '    <li class="sidebar-group">\n'
      printf '      <div class="sidebar-group-title">%s</div>\n' "$(html_escape "$group_label")"
      printf '      <ul class="sidebar-sublist">\n'
      while IFS= read -r file; do
        [ -n "$file" ] || continue
        sidebar_entry "$file"
      done < <(find "$source_dir" -maxdepth 1 -name '*.md' -printf '%f\n' | sort)
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
    for section in book spec design; do
      if ! has_section "$section"; then
        continue
      fi
      local nav_lang
      nav_lang="$(fallback_lang_for_section "$section" "$current_lang")"
      href="${prefix}${section}/${nav_lang}/index.html"
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
  {
    printf '<div class="lang-switcher">\n'
    local switch_lang source target label
    while IFS= read -r switch_lang; do
      [ -n "$switch_lang" ] || continue
      case "$switch_lang" in
        en) label='EN' ;;
        zh) label='中文' ;;
        zh-TW) label='繁中' ;;
        *) label="$switch_lang" ;;
      esac
      if [ "$switch_lang" = "$lang" ]; then
        printf '  <span class="current">%s</span>\n' "$label"
      else
        source="$(source_path_for "$section" "$switch_lang" "$current_source_name")"
        target="$prefix$section/$switch_lang/$(output_name_for "$current_source_name")"
        if [ ! -f "$source" ]; then
          target="$prefix$section/$switch_lang/index.html"
        fi
        printf '  <a href="%s">%s</a>\n' "$target" "$label"
      fi
    done < <(section_languages "$section")
    printf '</div>\n'
  } > "$file"
}

build_theme_toggle() {
  local lang="$1"
  local file="$2"
  {
    printf '<button class="theme-toggle" type="button" data-theme-toggle data-theme-prefix="%s" data-theme-auto="%s" data-theme-light="%s" data-theme-dark="%s" aria-label="%s">\n' \
      "$(html_escape "$(theme_prefix_label "$lang")")" \
      "$(html_escape "$(theme_state_label auto "$lang")")" \
      "$(html_escape "$(theme_state_label light "$lang")")" \
      "$(html_escape "$(theme_state_label dark "$lang")")" \
      "$(html_escape "$(theme_prefix_label "$lang")")"
    printf '  <span class="theme-toggle-label">◐ <span data-theme-toggle-text>%s: %s</span></span>\n' \
      "$(html_escape "$(theme_prefix_label "$lang")")" \
      "$(html_escape "$(theme_state_label auto "$lang")")"
    printf '</button>\n'
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
  local prefix top_nav sidebar switcher theme_toggle
  prefix="$(root_prefix_for "$rel_out")"
  top_nav="$TMP_DIR/top-nav-${section}-${lang}-${source_name}.html"
  sidebar="$TMP_DIR/sidebar-${section}-${lang}-${source_name}.html"
  switcher="$TMP_DIR/lang-${section}-${lang}-${source_name}.html"
  theme_toggle="$TMP_DIR/theme-toggle-${section}-${lang}-${source_name}.html"
  build_top_nav "$section" "$lang" "$prefix" "$top_nav"
  build_sidebar "$section" "$lang" "$(output_name_for "$source_name")" "$prefix" "$sidebar"
  build_lang_switcher "$section" "$lang" "$source_name" "$prefix" "$switcher"
  build_theme_toggle "$lang" "$theme_toggle"
  {
    printf '<header class="site-header">\n'
    printf '  <div class="site-header-inner">\n'
    printf '    <a class="site-title" href="%sindex.html">%s</a>\n' "$prefix" "$SITE_TITLE"
    cat "$top_nav"
    cat "$switcher"
    cat "$theme_toggle"
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
    cat <<'EOF2'
<script>
(() => {
  const key = 'scpp-docs-theme';
  const root = document.documentElement;

  function apply(theme) {
    if (theme === 'light' || theme === 'dark') {
      root.dataset.theme = theme;
    } else {
      root.removeAttribute('data-theme');
    }
  }

  function readTheme() {
    try {
      const stored = localStorage.getItem(key);
      return stored === 'light' || stored === 'dark' ? stored : 'auto';
    } catch {
      return 'auto';
    }
  }

  function persist(theme) {
    try {
      if (theme === 'auto') {
        localStorage.removeItem(key);
      } else {
        localStorage.setItem(key, theme);
      }
    } catch {
    }
  }

  function iconFor(theme) {
    if (theme === 'light') return '☀';
    if (theme === 'dark') return '🌙';
    return '◐';
  }

  function labelFor(button, theme) {
    const prefix = button.dataset.themePrefix || 'Theme';
    const state = button.dataset['theme' + theme.charAt(0).toUpperCase() + theme.slice(1)] || theme;
    return `${prefix}: ${state}`;
  }

  function renderButton(button, theme) {
    const textNode = button.querySelector('[data-theme-toggle-text]');
    if (textNode) {
      textNode.textContent = labelFor(button, theme);
      const icon = iconFor(theme);
      const wrapper = button.querySelector('.theme-toggle-label');
      if (wrapper) {
        wrapper.firstChild.textContent = `${icon} `;
      }
    }
    button.setAttribute('aria-pressed', theme === 'dark' ? 'true' : 'false');
  }

  function cycle(theme) {
    if (theme === 'auto') return 'dark';
    if (theme === 'dark') return 'light';
    return 'auto';
  }

  const initialTheme = readTheme();
  apply(initialTheme);

  document.addEventListener('DOMContentLoaded', () => {
    const buttons = document.querySelectorAll('[data-theme-toggle]');
    buttons.forEach((button) => {
      renderButton(button, readTheme());
      button.addEventListener('click', () => {
        const next = cycle(readTheme());
        persist(next);
        apply(next);
        buttons.forEach((b) => renderButton(b, next));
      });
    });
  });
})();
</script>
EOF2
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
  GENERATED_PAGE_PATHS+=("$rel_out")
}

site_url_for() {
  local rel_out="$1"
  if [ "$rel_out" = 'index.html' ]; then
    printf '%s/' "$SITE_BASE_URL"
  else
    printf '%s/%s' "$SITE_BASE_URL" "$rel_out"
  fi
}

generate_sitemap() {
  local out_file="$OUT_DIR/sitemap.xml"
  {
    printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>'
    printf '%s\n' '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">'
    local rel_out
    for rel_out in "${GENERATED_PAGE_PATHS[@]}"; do
      printf '  <url><loc>%s</loc></url>\n' "$(html_escape "$(site_url_for "$rel_out")")"
    done
    printf '%s\n' '</urlset>'
  } > "$out_file"
}

generate_robots() {
  cat > "$OUT_DIR/robots.txt" <<EOF2
User-agent: *
Allow: /
Sitemap: ${SITE_BASE_URL}/sitemap.xml
EOF2
}

build_section() {
  local section="$1"
  local lang="$2"
  while IFS= read -r file; do
    [ -n "$file" ] || continue
    local source
    source="$(source_path_for "$section" "$lang" "$file")"
    local out_name title rel_out
    out_name="$(output_name_for "$file")"
    rel_out="$section/$lang/$out_name"
    title="$(page_title_for "$source")"
    render_markdown_page "$source" "$rel_out" "$section" "$lang" "$file" "$title"
  done < <(section_files "$section" "$lang")
}

build_landing() {
  local md="$TMP_DIR/index.md"
  {
    cat <<'LANDING'
# SCPP Documentation Site

Browse the language book, the formal specifications, and the published design documents under <code>docs/design/</code>.

<div class="landing-grid">
  <section class="card">
    <h2>Book</h2>
    <ul>
LANDING
    while IFS= read -r lang; do
      [ -n "$lang" ] || continue
      printf '      <li><a href="book/%s/index.html">%s</a></li>\n' "$lang" "$(lang_label "$lang")"
    done < <(section_languages book)
    cat <<'LANDING'
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
    <h2>Design Docs</h2>
    <ul>
      <li><a href="design/en/index.html">English</a></li>
      <li><a href="design/zh/index.html">中文</a></li>
    </ul>
  </section>
</div>
LANDING
  } > "$md"
  local before="$TMP_DIR/before-home.html"
  local after="$TMP_DIR/after-home.html"
  local head="$TMP_DIR/head-home.html"
  local theme_toggle="$TMP_DIR/theme-toggle-home.html"
  build_theme_toggle en "$theme_toggle"
  cat > "$before" <<'EOF2'
<header class="site-header">
  <div class="site-header-inner">
    <a class="site-title" href="index.html">SCPP Documentation</a>
    <nav class="top-nav">
      <a class="current" href="index.html">Home</a>
      <a href="book/en/index.html">Book</a>
      <a href="spec/en/index.html">Spec</a>
      <a href="design/en/index.html">Design Docs</a>
      <a href="https://github.com/scpp-lang/scpp">GitHub</a>
    </nav>
    <div class="lang-switcher">
EOF2
  while IFS= read -r lang; do
    [ -n "$lang" ] || continue
    local label
    case "$lang" in
      en) label='EN' ;;
      zh) label='中文' ;;
      zh-TW) label='繁中' ;;
      *) label="$lang" ;;
    esac
    printf '      <a href="book/%s/index.html">%s</a>\n' "$lang" "$label" >> "$before"
  done < <(section_languages book)
  cat >> "$before" <<'EOF2'
    </div>
EOF2
  cat "$theme_toggle" >> "$before"
  cat >> "$before" <<'EOF2'
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
  GENERATED_PAGE_PATHS+=("index.html")
}

build_landing
for section in book spec design; do
  if ! has_section "$section"; then
    continue
  fi
  while IFS= read -r lang; do
    [ -n "$lang" ] || continue
    build_section "$section" "$lang"
  done < <(section_languages "$section")
done

generate_sitemap
generate_robots

echo "Done. Output in $OUT_DIR/"
