"""Check generated HTML/search links and UTF-8 without BOM, without writing files."""
from html.parser import HTMLParser
import json
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parent.parent


class Page(HTMLParser):
    def __init__(self, text):
        super().__init__(convert_charrefs=True)
        self.ids, self.links = set(), []
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs:
            if attrs["id"] in self.ids:
                raise ValueError(f'Duplicate anchor: {attrs["id"]}')
            self.ids.add(attrs["id"])
        for attr in ("href", "src"):
            if attr in attrs:
                self.links.append(attrs[attr])


def main():
    manifest = json.loads((ROOT / "component/manifest.json").read_text(encoding="utf-8"))
    pages = {name: Page((ROOT / name).read_text(encoding="utf-8")) for name in manifest["pages"]}
    checked = 0

    def check_link(page, link):
        nonlocal checked
        parsed = urlsplit(link)
        if parsed.scheme or parsed.netloc:
            return
        target = ((ROOT / page).parent / unquote(parsed.path)).resolve() if parsed.path else ROOT / page
        if not target.is_relative_to(ROOT) or not target.is_file():
            raise ValueError(f"Missing or nonportable link: {page}: {link}")
        relative = target.relative_to(ROOT).as_posix()
        if parsed.fragment and relative in pages and unquote(parsed.fragment) not in pages[relative].ids:
            raise ValueError(f"Missing anchor: {page}: {link}")
        checked += 1

    for name, page in pages.items():
        for link in page.links:
            check_link(name, link)
    source = (ROOT / "component/assets/search-index.js").read_text(encoding="utf-8")
    entries = json.loads(source.split("window.RHI_SEARCH = ", 1)[1].strip().removesuffix(";"))
    for entry in entries:
        check_link("index.html", entry["url"])
    texts = 0
    for path in ROOT.rglob("*"):
        if path.is_file() and path.suffix in (".html", ".json", ".js", ".css", ".py", ".md", ".cpp", ".txt"):
            data = path.read_bytes()
            if data.startswith(b"\xef\xbb\xbf"):
                raise ValueError(f"UTF-8 BOM: {path}")
            data.decode("utf-8", errors="strict")
            texts += 1
    if manifest["diagnostics"]:
        raise ValueError(f'Unclassified declarations: {manifest["diagnostics"]}')
    print(f"Verified {len(pages)} HTML pages, {checked} local links, {len(entries)} search entries, {texts} UTF-8 files.")


if __name__ == "__main__":
    main()
