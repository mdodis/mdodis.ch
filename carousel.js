// Prev/next buttons for every .carousel; wraps around at the ends.
document.querySelectorAll('.carousel').forEach(function (carousel) {
    var track = carousel.querySelector('.carousel-track');

    function step(dir) {
        var width = track.clientWidth;
        var max = track.scrollWidth - width;
        var next = track.scrollLeft + dir * width;
        if (next > max + 1) next = 0;
        if (next < -1) next = max;
        track.scrollTo({ left: next, behavior: 'smooth' });
    }

    carousel.querySelector('.carousel-prev').addEventListener('click', function () { step(-1); });
    carousel.querySelector('.carousel-next').addEventListener('click', function () { step(1); });
});
