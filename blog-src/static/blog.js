// Blog page behaviour: search over search-index.json, table of contents with scroll highlighting, copy buttons.
(function () {
    "use strict";

    /* ---- Search -------------------------------------------------------------------------------------------------- */
    const MAX_RESULTS = 10;
    const SNIPPET_RADIUS = 60;

    const searchBox = document.getElementById("searchbox");
    const resultsList = document.getElementById("search-results");
    let entries = null;

    function escapeHtml(s) {
        return s.replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
    }

    function escapeRegExp(s) {
        return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    }

    function countOccurrences(haystack, needle) {
        let count = 0;
        for (let i = haystack.indexOf(needle); i !== -1; i = haystack.indexOf(needle, i + needle.length)) {
            count++;
        }
        return count;
    }

    function loadIndex() {
        if (entries) return Promise.resolve(entries);
        return fetch("./search-index.json")
            .then(response => response.json())
            .then(data => {
                entries = data.map(item => {
                    // Contents still carry inline html (<b>, <code>) from the source text
                    const text = item.contents.replace(/<[^>]*>/g, "").replace(/\s+/g, " ").trim();
                    return {
                        item: item,
                        text: text,
                        lowerTitle: item.title.toLowerCase(),
                        lowerSubtitle: item.subtitle.toLowerCase(),
                        lowerText: text.toLowerCase(),
                    };
                });
                return entries;
            });
    }

    function search(query) {
        const terms = query.toLowerCase().split(/\s+/).filter(t => t.length >= 2);
        if (terms.length === 0) return [];

        const results = [];
        for (const entry of entries) {
            let score = 0;
            let matchedAll = true;
            for (const term of terms) {
                const inSubtitle = countOccurrences(entry.lowerSubtitle, term);
                const inTitle = countOccurrences(entry.lowerTitle, term);
                const inText = countOccurrences(entry.lowerText, term);
                if (inSubtitle + inTitle + inText === 0) {
                    matchedAll = false;
                    break;
                }
                score += inSubtitle * 10 + inTitle * 5 + inText;
            }
            if (matchedAll) results.push({ entry: entry, score: score, terms: terms });
        }

        results.sort((a, b) => b.score - a.score);
        return results.slice(0, MAX_RESULTS);
    }

    function highlight(text, terms) {
        const pattern = new RegExp("(" + terms.map(escapeRegExp).join("|") + ")", "gi");
        return escapeHtml(text).replace(pattern, "<mark>$1</mark>");
    }

    function snippet(entry, terms) {
        let at = -1;
        for (const term of terms) {
            const i = entry.lowerText.indexOf(term);
            if (i !== -1 && (at === -1 || i < at)) at = i;
        }
        if (at === -1) return "";

        const start = Math.max(0, at - SNIPPET_RADIUS);
        const end = Math.min(entry.text.length, at + SNIPPET_RADIUS * 2);
        // escapeHtml runs before highlighting, so terms can't match inside entities
        let s = highlight(entry.text.slice(start, end), terms);
        if (start > 0) s = "&hellip;" + s;
        if (end < entry.text.length) s += "&hellip;";
        return s;
    }

    function hideResults() {
        resultsList.hidden = true;
        resultsList.innerHTML = "";
    }

    function renderResults(results, query) {
        resultsList.innerHTML = "";
        resultsList.scrollTop = 0;

        if (results.length === 0) {
            const li = document.createElement("li");
            li.className = "search-empty";
            li.textContent = "No results for “" + query + "”";
            resultsList.appendChild(li);
            resultsList.hidden = false;
            return;
        }

        for (const result of results) {
            const item = result.entry.item;
            const li = document.createElement("li");
            const a = document.createElement("a");
            a.className = "search-result";
            a.href = item.page + "#" + item.section;
            a.innerHTML =
                '<span class="search-result-title">' + highlight(item.subtitle, result.terms) + "</span>" +
                '<span class="search-result-page">' + escapeHtml(item.title) + "</span>" +
                '<span class="search-result-snippet">' + snippet(result.entry, result.terms) + "</span>";
            li.appendChild(a);
            resultsList.appendChild(li);
        }
        resultsList.hidden = false;
    }

    function debounce(fn, delay) {
        let timeout;
        return function (...args) {
            clearTimeout(timeout);
            timeout = setTimeout(() => fn.apply(this, args), delay);
        };
    }

    function resultLinks() {
        return Array.from(resultsList.querySelectorAll("a.search-result"));
    }

    if (searchBox && resultsList) {
        searchBox.addEventListener("focus", loadIndex, { once: true });

        searchBox.addEventListener("input", debounce(() => {
            const query = searchBox.value.trim();
            if (!query) {
                hideResults();
                return;
            }
            loadIndex().then(() => renderResults(search(query), query));
        }, 150));

        searchBox.addEventListener("keydown", e => {
            const links = resultLinks();
            if (e.key === "ArrowDown" && links.length > 0) {
                e.preventDefault();
                links[0].focus();
            } else if (e.key === "Enter" && links.length > 0) {
                e.preventDefault();
                links[0].click();
            } else if (e.key === "Escape") {
                searchBox.value = "";
                hideResults();
                searchBox.blur();
            }
        });

        resultsList.addEventListener("keydown", e => {
            const links = resultLinks();
            const i = links.indexOf(document.activeElement);
            if (i === -1) return;

            if (e.key === "ArrowDown" && i + 1 < links.length) {
                e.preventDefault();
                links[i + 1].focus();
            } else if (e.key === "ArrowUp") {
                e.preventDefault();
                (i > 0 ? links[i - 1] : searchBox).focus();
            } else if (e.key === "Escape") {
                hideResults();
                searchBox.focus();
            }
        });

        // Following a link to another section of the same page doesn't reload, so close the list
        resultsList.addEventListener("click", e => {
            if (e.target.closest("a")) hideResults();
        });

        document.addEventListener("click", e => {
            if (!searchBox.contains(e.target) && !resultsList.contains(e.target)) hideResults();
        });

        // "/" focuses the search box from anywhere on the page
        document.addEventListener("keydown", e => {
            const typing = /^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement.tagName);
            if (e.key === "/" && !typing) {
                e.preventDefault();
                searchBox.focus();
            }
        });
    }

    /* ---- Table of contents --------------------------------------------------------------------------------------- */
    // docubuild2 nests <section class="Ln section" id="..."><h4>Title</h4>...</section> for every @section.
    const tocNav = document.getElementById("toc");
    const content = document.querySelector("main");
    const tocLinks = [];

    function childSections(parent) {
        const owner = parent.matches("section.section") ? parent : null;
        return Array.from(parent.querySelectorAll("section.section"))
            .filter(s => s.parentElement.closest("section.section") === owner);
    }

    function buildTocList(parent) {
        const sections = childSections(parent);
        if (sections.length === 0) return null;

        const ul = document.createElement("ul");
        for (const section of sections) {
            const heading = section.querySelector(":scope > h4");
            if (!heading || !section.id) continue;

            const li = document.createElement("li");
            const a = document.createElement("a");
            a.href = "#" + section.id;
            a.textContent = heading.textContent.trim();
            li.appendChild(a);
            tocLinks.push({ link: a, section: section });

            const nested = buildTocList(section);
            if (nested) li.appendChild(nested);
            ul.appendChild(li);
        }
        return ul;
    }

    function updateActiveTocLink() {
        // The last section whose top has scrolled past the sticky header is the current one
        const offset = 90;
        let current = tocLinks.length > 0 ? tocLinks[0] : null;
        for (const entry of tocLinks) {
            if (entry.section.getBoundingClientRect().top - offset <= 0) current = entry;
        }
        // At the very bottom, short last sections never reach the top
        if ((window.innerHeight + window.scrollY) >= document.documentElement.scrollHeight - 2 && tocLinks.length > 0) {
            current = tocLinks[tocLinks.length - 1];
        }
        for (const entry of tocLinks) {
            entry.link.classList.toggle("active", entry === current);
        }
    }

    if (tocNav && content) {
        const list = buildTocList(content);
        if (list) {
            tocNav.appendChild(list);

            let scheduled = false;
            window.addEventListener("scroll", () => {
                if (scheduled) return;
                scheduled = true;
                requestAnimationFrame(() => {
                    scheduled = false;
                    updateActiveTocLink();
                });
            }, { passive: true });
            updateActiveTocLink();
        } else {
            tocNav.closest("aside").remove();
        }
    }

    /* ---- Copy buttons -------------------------------------------------------------------------------------------- */
    document.querySelectorAll("pre.copy").forEach(pre => {
        const wrapper = document.createElement("div");
        wrapper.className = "code-wrapper";
        pre.parentNode.insertBefore(wrapper, pre);
        wrapper.appendChild(pre);

        const button = document.createElement("button");
        button.type = "button";
        button.className = "copy-btn";
        button.textContent = "Copy";
        wrapper.appendChild(button);

        button.addEventListener("click", () => {
            navigator.clipboard.writeText(pre.innerText.trim()).then(() => {
                button.textContent = "Copied!";
                setTimeout(() => (button.textContent = "Copy"), 2000);
            }).catch(err => console.error("Failed to copy: ", err));
        });
    });
})();
