/* Multi-Set-Cookie regression: both cookies from one response must be in
 * document.cookie. The page already performed a synchronous XHR to /cookie. */
(function () {
  var c = document.cookie;
  probeExit('cookieA=' + (/(?:^|; )A=1(?:;|$)/.test(c) ? 1 : 0) +
            ' cookieB=' + (/(?:^|; )B=2(?:;|$)/.test(c) ? 1 : 0) +
            ' raw=' + c);
})();
