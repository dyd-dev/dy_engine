"use strict";

(() => {
  const $ = (query, root = document) => root.querySelector(query);
  const $$ = (query, root = document) => [...root.querySelectorAll(query)];
  const toast = $(".toast");
  let toastTimer;
  function notify(message) {
    toast.textContent = message;
    toast.classList.add("visible");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => toast.classList.remove("visible"), 2300);
  }

  const themeButton = $(".theme-toggle");
  function themeLabel() {
    const dark = document.documentElement.dataset.theme === "dark";
    themeButton.setAttribute("aria-label", dark ? "밝은 테마로 전환" : "어두운 테마로 전환");
    themeButton.title = themeButton.getAttribute("aria-label");
  }
  themeLabel();
  themeButton.addEventListener("click", () => {
    const theme = document.documentElement.dataset.theme === "dark" ? "light" : "dark";
    document.documentElement.dataset.theme = theme;
    try { localStorage.setItem("dy-rhi-theme", theme); } catch (_) { /* optional */ }
    themeLabel();
  });
  $(".print-button").addEventListener("click", () => window.print());

  // Build highlighting with text nodes. Header text never becomes executable HTML.
  const keywords = new Set("class struct enum namespace public protected private virtual static const constexpr inline explicit using return if else for while try catch throw true false nullptr default delete int void bool float auto sizeof unsigned".split(" "));
  const tokenPattern = /\/\/[^\n]*|\/\*[\s\S]*?\*\/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\b(?:0x[\da-fA-F]+|\d+(?:\.\d+)?)(?:[uUlLfF]*)\b|\b[A-Za-z_]\w*\b/g;
  $$("code.language-cpp").forEach(block => {
    const source = block.textContent;
    const fragment = document.createDocumentFragment();
    let end = 0;
    for (const match of source.matchAll(tokenPattern)) {
      fragment.append(source.slice(end, match.index));
      const value = match[0];
      const kind = value.startsWith("//") || value.startsWith("/*") ? "comment"
        : /^["']/.test(value) ? "string"
        : /^\d/.test(value) ? "number"
        : keywords.has(value) ? "keyword"
        : /^(?:u?int\d+_t|size_t|[A-Z]\w*)$/.test(value) ? "type" : "";
      if (kind) {
        const span = document.createElement("span");
        span.className = `token-${kind}`;
        span.textContent = value;
        fragment.append(span);
      } else fragment.append(value);
      end = match.index + value.length;
    }
    fragment.append(source.slice(end));
    block.replaceChildren(fragment);
  });

  $$(".copy-button").forEach(button => button.addEventListener("click", async () => {
    const code = $("pre code", button.closest(".code-block"));
    let copied = false;
    try {
      if (navigator.clipboard && window.isSecureContext) {
        await navigator.clipboard.writeText(code.textContent);
        copied = true;
      }
    } catch (_) { /* The file: fallback below also works when clipboard access is denied. */ }
    if (!copied) {
      const field = document.createElement("textarea");
      field.value = code.textContent;
      field.style.cssText = "position:fixed;top:0;left:-9999px";
      document.body.append(field);
      field.select();
      try { copied = document.execCommand("copy"); } catch (_) { /* show selection */ }
      field.remove();
      button.focus();
    }
    if (copied) {
      button.textContent = "복사됨";
      setTimeout(() => { button.textContent = "복사"; }, 2000);
      notify("코드를 복사했습니다.");
    } else {
      const range = document.createRange();
      range.selectNodeContents(code);
      const selection = window.getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
      notify("선택된 코드를 Ctrl+C 또는 ⌘C로 복사하세요.");
    }
  }));

  const menuButton = $(".menu-toggle");
  const backdrop = $(".nav-backdrop");
  const sidebar = $("#sidebar");
  function setMenu(open, returnFocus = false) {
    document.body.classList.toggle("menu-open", open);
    menuButton.setAttribute("aria-expanded", String(open));
    backdrop.hidden = !open;
    if (open) $("button, a", sidebar)?.focus();
    else if (returnFocus) menuButton.focus();
  }
  menuButton.addEventListener("click", () => setMenu(!document.body.classList.contains("menu-open")));
  backdrop.addEventListener("click", () => setMenu(false, true));
  $$("a", sidebar).forEach(a => a.addEventListener("click", () => setMenu(false)));
  matchMedia("(min-width: 821px)").addEventListener("change", event => { if (event.matches) setMenu(false); });

  const dialog = $("#search-dialog");
  const input = $("#search-input");
  const results = $("#search-results");
  const status = $("#search-status");
  const index = (window.RHI_SEARCH || []).map(entry => ({
    ...entry,
    titleLower: entry.title.toLocaleLowerCase(),
    haystack: `${entry.title} ${entry.text} ${entry.header} ${entry.kind}`.toLocaleLowerCase(),
  }));
  let selection = -1;
  let resultLinks = [];
  let searchOpener = null;
  function selectResult(next, focus = false) {
    if (!resultLinks.length) return;
    selection = (next + resultLinks.length) % resultLinks.length;
    resultLinks.forEach((a, i) => a.classList.toggle("selected", i === selection));
    resultLinks[selection].scrollIntoView({ block: "nearest" });
    if (focus) resultLinks[selection].focus();
  }
  function renderSearch() {
    const query = input.value.trim().toLocaleLowerCase();
    const terms = query.split(/\s+/).filter(Boolean);
    let matches;
    if (query) {
      matches = index.filter(entry => terms.every(term => entry.haystack.includes(term)));
      const score = entry => entry.titleLower === query ? 0 : entry.titleLower.startsWith(query) ? 1 : entry.titleLower.includes(query) ? 2 : 3;
      matches.sort((a, b) => score(a) - score(b) || a.title.length - b.title.length || a.title.localeCompare(b.title));
    } else {
      const names = ["IDevice", "ICommandList", "GraphicsPipelineDesc", "ResourceScope", "RenderGraph", "TextureDesc"];
      matches = names.map(name => index.find(entry => entry.title === name)).filter(Boolean);
    }
    status.textContent = query ? `${matches.length}개 결과${matches.length > 60 ? " · 상위 60개 표시" : ""}` : "자주 찾는 API";
    results.replaceChildren();
    selection = -1;
    resultLinks = [];
    if (!matches.length) {
      const empty = document.createElement("p");
      empty.className = "search-empty";
      empty.textContent = "일치하는 API가 없습니다. 이름의 일부나 다른 설명으로 검색해 보세요.";
      results.append(empty);
    }
    for (const entry of matches.slice(0, 60)) {
      const link = document.createElement("a");
      link.className = "search-result";
      link.href = entry.url;
      const title = document.createElement("div");
      title.className = "result-title";
      const name = document.createElement("code");
      name.textContent = entry.title;
      const kind = document.createElement("span");
      kind.className = "result-kind";
      kind.textContent = `${entry.kind} · ${entry.header}`;
      title.append(name, kind);
      const description = document.createElement("div");
      description.className = "result-description";
      description.textContent = entry.text.replaceAll("`", "");
      link.append(title, description);
      link.addEventListener("click", () => dialog.close());
      link.addEventListener("focus", () => {
        selection = resultLinks.indexOf(link);
        resultLinks.forEach(a => a.classList.toggle("selected", a === link));
      });
      results.append(link);
      resultLinks.push(link);
    }
    results.scrollTop = 0;
  }
  function openSearch() {
    searchOpener = document.activeElement;
    setMenu(false);
    dialog.showModal();
    renderSearch();
    input.focus();
    input.select();
  }
  $$(".search-trigger").forEach(button => button.addEventListener("click", openSearch));
  $(".close-search").addEventListener("click", () => dialog.close());
  dialog.addEventListener("close", () => {
    if (searchOpener?.getClientRects().length) searchOpener.focus();
    else menuButton.focus();
  });
  dialog.addEventListener("click", event => {
    const bounds = dialog.getBoundingClientRect();
    if (event.target === dialog && (event.clientX < bounds.left || event.clientX > bounds.right || event.clientY < bounds.top || event.clientY > bounds.bottom)) dialog.close();
  });
  input.addEventListener("input", renderSearch);
  dialog.addEventListener("keydown", event => {
    if (event.key === "Escape") {
      // A search input otherwise consumes the first Escape just to clear its value.
      event.preventDefault();
      event.stopPropagation();
      dialog.close();
    } else if (event.key === "ArrowDown" || event.key === "ArrowUp") {
      event.preventDefault();
      selectResult(event.key === "ArrowDown" ? selection + 1 : (selection < 0 ? resultLinks.length - 1 : selection - 1), event.target !== input);
    } else if (event.key === "Enter" && event.target === input && resultLinks.length) {
      event.preventDefault();
      resultLinks[Math.max(selection, 0)].click();
    }
  });
  document.addEventListener("keydown", event => {
    if (event.key === "Tab" && !dialog.open && document.body.classList.contains("menu-open")) {
      const focusable = [menuButton, ...$$("button, a, summary", sidebar)].filter(el => el.getClientRects().length);
      const position = focusable.indexOf(document.activeElement);
      if (event.shiftKey && position <= 0) {
        event.preventDefault();
        focusable.at(-1)?.focus();
      } else if (!event.shiftKey && (position === focusable.length - 1 || position < 0)) {
        event.preventDefault();
        focusable[0]?.focus();
      }
    }
    const editing = event.target.matches("input, textarea, select, [contenteditable='true']");
    if (!dialog.open && !editing && (event.key === "/" || (event.key.toLowerCase() === "k" && (event.ctrlKey || event.metaKey)))) {
      event.preventDefault();
      openSearch();
    } else if (event.key === "Escape" && !dialog.open && document.body.classList.contains("menu-open")) {
      setMenu(false, true);
    }
  });

  const apiFilter = $("#api-filter");
  if (apiFilter) {
    const sections = $$(".index-section");
    const rows = sections.flatMap(section => $$("tbody tr", section));
    const descriptions = rows.map(row => row.textContent.toLocaleLowerCase());
    function filterIndex() {
      const terms = apiFilter.value.trim().toLocaleLowerCase().split(/\s+/).filter(Boolean);
      let count = 0;
      rows.forEach((row, i) => {
        row.hidden = !terms.every(term => descriptions[i].includes(term));
        if (!row.hidden) count++;
      });
      sections.forEach(section => { section.hidden = !$$("tbody tr", section).some(row => !row.hidden); });
      $("#filter-status").textContent = `${rows.length}개 중 ${count}개 표시`;
    }
    apiFilter.addEventListener("input", filterIndex);
    filterIndex();
  }

  const tocLinks = $$(".page-toc nav a");
  const anchors = tocLinks.map(link => document.getElementById(decodeURIComponent(link.hash.slice(1)))).filter(Boolean);
  let scrollPending = false;
  function updateToc() {
    let active = anchors[0];
    for (const anchor of anchors) if (anchor.getBoundingClientRect().top <= 160) active = anchor;
    tocLinks.forEach(link => {
      const current = !!active && link.hash === `#${active.id}`;
      link.classList.toggle("active", current);
      if (current) link.setAttribute("aria-current", "location");
      else link.removeAttribute("aria-current");
    });
    scrollPending = false;
  }
  document.addEventListener("scroll", () => {
    if (!scrollPending) { scrollPending = true; requestAnimationFrame(updateToc); }
  }, { passive: true });
  updateToc();

  const currentNav = $(".nav-symbol.current");
  if (currentNav && matchMedia("(min-width: 821px)").matches) {
    sidebar.scrollTop = Math.max(0, currentNav.offsetTop - sidebar.clientHeight / 2);
  }
})();
