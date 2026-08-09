import { startAnnotator } from "./ui/app.mjs";

window.addEventListener("DOMContentLoaded", () => {
  startAnnotator().catch((error) => {
    const root = document.querySelector("#annotator");
    if (root) root.innerHTML = `<div class="fatal">Unable to start annotator: ${String(error.message || error)}</div>`;
  });
});
