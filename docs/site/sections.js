// Fold each h2 / h3 section of a page; "すべて開く" / "すべて閉じる" act on every section.
// Sections start closed; a link target's sections open. A collapsed body uses hidden="until-found", so the browser's
// find-in-page still reaches its text and opens it.
(function () {
  "use strict";

  const article = document.querySelector("article.md-content__inner");
  if (!article) return;

  const sections = [];

  function level(element) {
    const match = /^H([1-3])$/.exec(element.tagName);
    return match ? Number(match[1]) : 0;
  }

  function setOpen(section, open) {
    if (open) section.body.removeAttribute("hidden");
    else section.body.setAttribute("hidden", "until-found");
    section.button.setAttribute("aria-expanded", String(open));
    section.button.title = open ? "折りたたむ" : "開く";
    section.heading.classList.toggle("pg-fold-closed", !open);
  }

  function isOpen(section) {
    return !section.body.hasAttribute("hidden");
  }

  // Move everything after a heading of `depth` into a body, up to the next heading
  // of the same or a shallower depth. Text nodes move too: a paragraph right after
  // a fence has no <p> of its own and sits in the container as bare text and links.
  function wrap(container, depth) {
    let body = null;
    for (const child of Array.from(container.childNodes)) {
      const childLevel = child.nodeType === Node.ELEMENT_NODE ? level(child) : 0;
      if (childLevel && childLevel <= depth) {
        body = null;
        if (childLevel === depth && child.id) {
          body = document.createElement("div");
          body.className = "pg-fold-body pg-fold-h" + depth;
          body.id = "pg-fold-" + child.id;
          child.after(body);
          const button = document.createElement("button");
          button.type = "button";
          button.className = "pg-fold-toggle";
          button.setAttribute("aria-controls", body.id);
          child.prepend(button);
          child.classList.add("pg-fold-heading");
          const section = { heading: child, body: body, button: button };
          sections.push(section);
          setOpen(section, false);
          child.addEventListener("click", function (event) {
            if (event.target.closest("a")) return;
            setOpen(section, !isOpen(section));
          });
          body.addEventListener("beforematch", function () {
            setOpen(section, true);
          });
        }
      } else if (body) {
        body.appendChild(child);
      }
    }
  }

  wrap(article, 2);
  for (const container of [article].concat(sections.map(function (s) { return s.body; }))) {
    wrap(container, 3);
  }
  if (sections.length >= 2) {
    const bar = document.createElement("div");
    bar.className = "pg-fold-bar";
    for (const [label, open] of [["すべて開く", true], ["すべて閉じる", false]]) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "md-button";
      button.textContent = label;
      button.addEventListener("click", function () {
        for (const section of sections) setOpen(section, open);
      });
      bar.appendChild(button);
    }
    const title = article.querySelector(":scope > h1");
    if (title) title.after(bar);
    else article.prepend(bar);
  }

  // Open the sections around a link target (table of contents, search result, #anchor).
  function reveal() {
    if (!location.hash) return;
    let target;
    try {
      target = document.getElementById(decodeURIComponent(location.hash.slice(1)));
    } catch (error) {
      return;
    }
    if (!target) return;
    let changed = false;
    for (const section of sections) {
      if (!isOpen(section) && (section.heading === target || section.body.contains(target))) {
        setOpen(section, true);
        changed = true;
      }
    }
    if (changed) target.scrollIntoView();
  }

  window.addEventListener("hashchange", reveal);
  document.addEventListener("click", function (event) {
    const link = event.target.closest('a[href^="#"]');
    if (link && link.getAttribute("href") === location.hash) setTimeout(reveal, 0);
  });
  reveal();
})();
