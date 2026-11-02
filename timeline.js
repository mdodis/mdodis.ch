// Aligns each <li data-time="id"> in a .timeline to the element with the same
// data-time="id" inside the same .section-row (matching its top and height).
function layoutTimelines() {
    document.querySelectorAll('.section-row').forEach(function (row) {
        var rowTop = row.getBoundingClientRect().top;
        row.querySelectorAll('.timeline li[data-time]').forEach(function (item) {
            var target = row.querySelector('.section [data-time="' + item.dataset.time + '"]');
            if (!target) return;
            var rect = target.getBoundingClientRect();
            item.style.top = (rect.top - rowTop) + 'px';
            item.style.height = rect.height + 'px';
        });
    });
}

window.addEventListener('load', layoutTimelines);
window.addEventListener('resize', layoutTimelines);
if ('ResizeObserver' in window) {
    var observer = new ResizeObserver(layoutTimelines);
    document.querySelectorAll('.section').forEach(function (s) { observer.observe(s); });
}
