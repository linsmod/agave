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

    /* A <style> is a sheet wherever it lands. The 2008 walk stopped at
     * <body>, so a style written in the body (the parser parks it in head),
     * appended into <body> by script, or appended to <html> after <body> was
     * invisible: <style>.sheet was null and its rules did nothing. css3test
     * appends its probe <style> to documentElement. */
    var s3 = document.getElementById('s3');
    out.push('bodystyle.sheet=' + (s3.sheet ? s3.sheet.cssRules.length : -1));
    out.push('bodystyle.color=' +
             getComputedStyle(document.getElementById('body-style')).color);

    var target = document.createElement('div');
    target.id = 'appended';
    target.textContent = 'w';
    document.body.appendChild(target);

    var inBody = document.createElement('style');
    inBody.textContent = '#appended { color: rgb(21, 22, 23); }';
    document.body.appendChild(inBody);
    out.push('append-body.sheet=' + (inBody.sheet ? inBody.sheet.cssRules.length : -1));
    out.push('append-body.color=' + getComputedStyle(target).color);

    var afterBody = document.createElement('style');
    afterBody.textContent = '#appended { color: rgb(31, 32, 33); }';
    document.documentElement.appendChild(afterBody);
    out.push('append-root.sheet=' + (afterBody.sheet ? afterBody.sheet.cssRules.length : -1));
    out.push('append-root.color=' + getComputedStyle(target).color);

    var afterBodyDynamic = document.createElement('style');
    document.documentElement.appendChild(afterBodyDynamic);
    afterBodyDynamic.textContent = '#appended { color: rgb(41, 42, 43); }';
    out.push('append-root-late=' +
             (afterBodyDynamic.sheet ? afterBodyDynamic.sheet.cssRules.length : -1));
    out.push('append-root-late.color=' + getComputedStyle(target).color);

    /* CSS.supports('selector(...)'): must consult the selector parser, not
     * treat the argument as a declaration. */
    out.push('supports.sel-type=' + CSS.supports('selector(h1)'));
    out.push('supports.sel-pseudo=' + CSS.supports('selector(::first-letter)'));
    out.push('supports.sel-attr=' + CSS.supports('selector([title="x"])'));
    out.push('supports.sel-bad=' + CSS.supports('selector(!!!'));

    /* @page is a CSSOM page rule with its declaration block, and it does not
     * leak into the cascade. */
    var pageSheet = document.createElement('style');
    pageSheet.textContent = '@page :left { margin: 2cm; } .pg { color: rgb(31, 32, 33); }';
    document.head.appendChild(pageSheet);
    var pageRules = pageSheet.sheet ? pageSheet.sheet.cssRules : null;
    out.push('page.count=' + (pageRules ? pageRules.length : -1));
    out.push('page.type=' + (pageRules && pageRules.length ? pageRules[0].type : -1));
    out.push('page.selector=' +
             (pageRules && pageRules.length ? pageRules[0].selectorText : '-'));
    out.push('page.styleLen=' + (pageRules && pageRules.length ? pageRules[0].style.length : -1));
    out.push('page.margin=' +
             (pageRules && pageRules.length
                 ? pageRules[0].style.getPropertyValue('margin-left') : '-'));

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
