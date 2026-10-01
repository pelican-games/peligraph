"""Publish only the navigation's documents; point other repository links at GitHub."""
import os
import posixpath
import re
import subprocess
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
ASSETS = {"site/styles.css"}
LINK = re.compile(r'(!?\[(?:[^\[\]]|\[[^\]]*\])*\]\()([^\s)]+)(\))')
PAGES = set()
REVISION = ""


def nav_paths(items):
    for item in items:
        if isinstance(item, str):
            yield item
        elif isinstance(item, dict):
            yield from nav_paths(item.values())
        else:
            yield from nav_paths(item)


def on_config(config):
    global PAGES, REVISION
    PAGES = set(nav_paths(config["nav"]))
    REVISION = os.environ.get("DOCS_SOURCE_REF") or subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()
    if os.environ.get("DOCS_SITE_URL"):
        config["site_url"] = os.environ["DOCS_SITE_URL"].rstrip("/") + "/"
    return config


def on_files(files, config):
    # An allowlist prevents new internal notes or arbitrary files under docs/ from
    # being copied into a public site merely because they were added to the repo.
    for file in list(files):
        from_docs = file.src_dir is not None and Path(file.src_dir).resolve() == Path(config["docs_dir"]).resolve()
        if from_docs and file.src_uri not in PAGES | ASSETS:
            files.remove(file)
    return files


def rewrite_link(match, source, repo_url):
    prefix, destination, suffix = match.groups()
    url = urlsplit(destination)
    if url.scheme or url.netloc or not url.path:
        return match.group(0)
    path = posixpath.normpath(posixpath.join(posixpath.dirname(source), unquote(url.path)))
    if path in PAGES or path in ASSETS:
        return match.group(0)
    absolute = (ROOT / "docs" / path).resolve()
    if not absolute.is_relative_to(ROOT) or not absolute.exists():
        raise ValueError(f"{source}: repository link does not exist: {destination}")
    route = "tree" if absolute.is_dir() else "blob"
    target = quote(absolute.relative_to(ROOT).as_posix(), safe="/")
    destination = f"{repo_url.rstrip('/')}/{route}/{quote(REVISION, safe='')}/{target}"
    if url.query:
        destination += "?" + url.query
    if url.fragment:
        destination += "#" + url.fragment
    return prefix + destination + suffix


def on_page_markdown(markdown, page, config, files):
    # Skip fenced examples: changing paths inside example code changes its meaning.
    result = []
    prose = []
    def flush_prose():
        result.append(LINK.sub(lambda m: rewrite_link(m, page.file.src_uri, config["repo_url"]), "".join(prose)))
        prose.clear()
    fence = None
    for line in markdown.splitlines(keepends=True):
        marker = re.match(r'^\s*(?:>\s*)?(`{3,}|~{3,})', line)
        if marker:
            flush_prose()
            run = marker.group(1)
            if fence is None:
                fence = run
            elif run[0] == fence[0] and len(run) >= len(fence):
                fence = None
            result.append(line)
        elif fence:
            result.append(line)
        else:
            prose.append(line)
    flush_prose()
    return "".join(result)
