/* CSSOM capability check for tools/pages/cap_cssom.html (run by --eval-file).
 *
 * Reads document.styleSheets, walks cssRules, and asserts the properties a
 * feature-detecting page uses: sheet href/type/media/ownerNode, rule type/
 * selectorText/cssText/style, nested @media rules, @font-face, and
 * <style>.sheet. Also guards document.URL/title: the document object has a
 * single catch-all getter, so a new CSSOM property must not replace it.
 */
(function () {
    var out = [];
    var ss = document.styleSheets;

    out.push('sheets=' + ss.length);
    out.push('list.item=' + (typeof ss.item));

    var s0 = ss[0];
    out.push('sheet0.id=' + (s0.ownerNode ? s0.ownerNode.id : '-'));
    out.push('sheet0.type=' + s0.type);
    out.push('sheet0.rules=' + s0.cssRules.length);
    out.push('sheet0.rulesAlias=' + (s0.rules ? s0.rules.length : -1));

    var r0 = s0.cssRules[0];
    out.push('rule0.type=' + r0.type);
    out.push('rule0.sel=' + r0.selectorText);
    out.push('rule0.color=' + r0.style.getPropertyValue('color'));
    out.push('rule0.width=' + r0.style.getPropertyValue('width'));
    out.push('rule0.cssLen=' + (r0.cssText ? r0.cssText.length : -1));
    out.push('rule0.styleItem=' + r0.style.item(0));

    var r1 = s0.cssRules[1];
    out.push('rule1.ml=' + r1.style.getPropertyValue('margin-left'));
    out.push('rule1.prio=' + r1.style.getPropertyPriority('margin-left'));

    var r2 = s0.cssRules[2];
    out.push('rule2.type=' + r2.type);
    out.push('rule2.media=' + r2.media.mediaText);
    out.push('rule2.inner=' + r2.cssRules.length);
    out.push('rule2.innerSel=' + r2.cssRules[0].selectorText);

    var s1 = ss[1];
    out.push('sheet1.rules=' + s1.cssRules.length);
    out.push('sheet1.unset=' + (s1.cssRules[0].style.getPropertyValue('width').length > 0 ? 'set' : 'empty'));
    out.push('sheet1.fontface=' + s1.cssRules[1].type);
    out.push('sheet1.fontfaceStyle=' + (s1.cssRules[1].style ? 'yes' : 'no'));

    var st = document.getElementById('s1');
    out.push('stylesheet.sheet=' + (st.sheet ? st.sheet.cssRules.length : -1));
    out.push('stylesheet.owner=' + (st.sheet && st.sheet.ownerNode === st));
    out.push('identity.list=' + (document.styleSheets === document.styleSheets));
    out.push('identity.sheet=' + (st.sheet === document.styleSheets[0]));

    /* Mutation lives on the same wrapper: insertRule must show up in cssRules
     * and in the page's cascade, deleteRule must remove it. */
    var sheet = st.sheet;
    var before = sheet.cssRules.length;
    var idx = sheet.insertRule('.zz { color: rgb(9, 8, 7); }', before);
    out.push('insert.index=' + idx);
    out.push('insert.length=' + sheet.cssRules.length);
    out.push('insert.color=' + sheet.cssRules[idx].style.getPropertyValue('color'));
    sheet.deleteRule(idx);
    out.push('delete.length=' + sheet.cssRules.length);

    out.push('doc.url=' + (typeof document.URL));
    out.push('doc.title=' + document.title);
    out.push('doc.body=' + (document.body ? document.body.tagName : 'null'));

    /* A <style> added after load must show up on the next read: the list is
     * live (Document::styleSheets() calls updateStyleIfNeeded). */
    var dyn = document.createElement('style');
    dyn.id = 'dyn';
    dyn.textContent = '.e { color: rgb(4, 5, 6); }';
    document.head.appendChild(dyn);

    var ds = document.styleSheets;
    var dynRules = -1;
    for (var i = 0; i < ds.length; ++i) {
        if (ds[i].ownerNode === dyn)
            dynRules = ds[i].cssRules.length;
    }
    out.push('dynamic.sheets=' + ds.length);
    out.push('dynamic.rules=' + dynRules);

    probeExit(out.join('\n'));
})();
