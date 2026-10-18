// Apply the saved theme before styles load; storage may be blocked for file: URLs.
(() => {
  let theme;
  try { theme = localStorage.getItem("dy-rhi-theme"); } catch (_) { /* optional */ }
  if (theme !== "dark" && theme !== "light") {
    theme = matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  }
  document.documentElement.dataset.theme = theme;
})();
