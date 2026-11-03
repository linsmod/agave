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
    makeObj      14     154      11x
    wideRead     55     129       2.3x
    wideWrite    15      34       2.3x
    arrField      1       2       2x
    calls         1       2       2x
    proto         1      63      63x
    accessor      1     112     112x
    strings       2       9       4.5x
    patch         2     176      88x
    mapSet        7      24       3.4x
    proxyRw       3      43      14x
    classGetter   0      13      --
    smallStr      1       4       4x
    numIdx        1       3       3x
    sameKey       2       7       3.5x

Reading: plain property access (smallStr / numIdx / sameKey) is only 3-4x
slower and array indexing is close to V8. The outliers all involve *accessor
properties*:

    plain read             ~7 ns
    accessor read/write   ~140 ns   (20x a plain read)
    object literal        ~380 ns

ccessor (800k get/set pairs) takes 112ms while smallStr (600k plain
reads) takes 4ms, so the cost is in js_call_getter -> call_proto_accessor ->
sv_vm_call, not in the property lookup itself. Vue's reactivity and the site's
Feature class getters hit that path on every render.
