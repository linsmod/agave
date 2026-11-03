/*
 * bench.js - micro benchmarks that model what a Vue-style renderer does.
 *
 * Runs unchanged in Node (prints a line) and in the engine (sets document.title),
 * so the same code can be compared across VMs.
 */
(function () {
	var results = [];

	function now() {
		return (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now();
	}

	function bench(name, fn, iters) {
		var i;
		var t0, t1;

		fn(1); /* warm up */

		t0 = now();
		for (i = 0; i < iters; i++) {
			fn(i);
		}
		t1 = now();

		results.push([name, Math.round(t1 - t0)]);
	}

	var OBJ_COUNT = 20000;
	var ITERS = 200;

	/* 1. create many objects with many properties (vnode-like) */
	bench('makeObj', function () {
		var i, o, list = [];
		for (i = 0; i < 2000; i++) {
			o = { type: 'div', key: i, props: { id: 'x' + i, class: 'a b' }, children: [], el: null, shapeFlag: 1 };
			list.push(o);
		}
		return list.length;
	}, ITERS);

	/* 2. property read on a wide object */
	var wide = {};
	(function () {
		var i;
		for (i = 0; i < 40; i++) {
			wide['p' + i] = i;
		}
	}());

	bench('wideRead', function () {
		var i, sum = 0;
		for (i = 0; i < 40; i++) {
			sum += wide['p' + i];
		}
		return sum;
	}, OBJ_COUNT);

	/* 3. property write on a wide object */
	bench('wideWrite', function (n) {
		var i;
		for (i = 0; i < 40; i++) {
			wide['p' + i] = n;
		}
	}, 5000);

	/* 4. array of objects, read one field */
	var arr = [];
	(function () {
		var i;
		for (i = 0; i < 2000; i++) {
			arr.push({ id: i, tag: 'div', key: 'k' + i, flag: (i & 1) === 0 });
		}
	}());

	bench('arrField', function () {
		var i, sum = 0;
		for (i = 0; i < arr.length; i++) {
			sum += arr[i].id;
		}
		return sum;
	}, ITERS);

	/* 5. function call overhead through a closure */
	var counter = 0;

	function bump() {
		counter++;
	}

	bench('calls', function () {
		var i;
		for (i = 0; i < 1000; i++) {
			bump();
		}
	}, ITERS);

	/* 6. prototype chain access */
	function Base() {
		this.a = 1;
	}
	Base.prototype.hello = function () {
		return 1;
	};

	function Derived() {
		Base.call(this);
	}
	Derived.prototype = Object.create(Base.prototype);

	bench('proto', function () {
		var i, sum = 0, o;
		for (i = 0; i < 2000; i++) {
			o = new Derived();
			sum += o.a + o.hello();
		}
		return sum;
	}, ITERS);

	/* 7. Object.defineProperty getters (what our style wrapper does) */
	var accessor = { _v: 1 };

	Object.defineProperty(accessor, 'value', {
		get: function () {
			return this._v;
		},
		set: function (v) {
			this._v = v;
		}
	});

	bench('accessor', function () {
		var i, sum = 0;
		for (i = 0; i < 2000; i++) {
			accessor.value = i;
			sum += accessor.value;
		}
		return sum;
	}, ITERS);

	/* 8. string building */
	bench('strings', function () {
		var i, s = '';
		for (i = 0; i < 500; i++) {
			s += 'x' + i;
		}
		return s.length;
	}, ITERS);

	/* 9. JSON-ish deep compare (patch algorithm style) */
	bench('patch', function () {
		var i, n = 0, a, b;
		for (i = 0; i < 2000; i++) {
			a = { type: 'div', key: i, props: { id: 'x' }, children: [], el: null, shapeFlag: 1 };
			b = { type: 'div', key: i, props: { id: 'x' }, children: [], el: null, shapeFlag: 1 };
			if (a.type === b.type && a.key === b.key && a.shapeFlag === b.shapeFlag) {
				n++;
			}
		}
		return n;
	}, ITERS);

	/* 10. Map / Set */
	bench('mapSet', function () {
		var i, m = new Map();
		for (i = 0; i < 500; i++) {
			m.set('k' + i, i);
		}
		return m.get('k499');
	}, ITERS);


	/* 11. Proxy get/set (Vue 3 reactivity goes through this on every access) */
	if (typeof Proxy !== 'undefined') {
		var ptarget = { a: 1, b: 2, c: 3 };
		var proxy = new Proxy(ptarget, {
			get: function (t, k) { return t[k]; },
			set: function (t, k, v) { t[k] = v; return true; }
		});

		bench('proxyRw', function (n) {
			var sum;
			proxy.a = n;
			sum = proxy.a + proxy.b + proxy.c;
			return sum;
		}, 20000);
	}

	/* 12. class accessors (the Feature class is full of getters) */
	function AccessorClass() {
		this._x = 0;
	}

	Object.defineProperty(AccessorClass.prototype, 'x', {
		get: function () { return this._x; },
		set: function (v) { this._x = v; }
	});

	bench('classGetter', function (n) {
		var o = new AccessorClass();
		o.x = n;
		return o.x;
	}, 20000);

	{
		var line = '';
		var i;

		for (i = 0; i < results.length; i++) {
			line += (i ? ' ' : '') + results[i][0] + '=' + results[i][1];
		}

		if (typeof document !== 'undefined' && document.title !== undefined) {
			document.title = '[bench] ' + line;
		}
		if (typeof console !== 'undefined' && console.log) {
			console.log('[bench] ' + line);
		}
	}
}());
