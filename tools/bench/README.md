# tools/bench - micro benchmarks for the Ant VM

`bench.js` models the operations a Vue-style renderer performs (object literal
creation, accessor get/set, prototype calls, Proxy traps, Map/Set, strings).

The same file runs unchanged in Node and in the engine:

    node tools/bench/bench.js                       # V8 reference numbers
    agave_view_host http://127.0.0.1:8899/bench.html  # engine numbers (title)

Serve the folder with any static server, e.g.

    cd tools/bench && python -m http.server 8899

Measured 2026-11 (ms, lower is better):

    test         V8     engine   ratio
    makeObj      14     151      10.8x
    wideRead     56     128       2.3x
    wideWrite    16      34       2.1x
    arrField      1       2       2.0x
    calls         1       2       2.0x
    proto         1      63      63x
    accessor      1     105     105x
    strings       2       8       4.0x
    patch         2     164      82x
    mapSet        7      22       3.1x
    proxyRw       4      40      10x
    classGetter   1      14      14x

Accessor and prototype dispatch are the outliers, and those are exactly what
Vue's reactivity and the site's Feature getters exercise on every render.
