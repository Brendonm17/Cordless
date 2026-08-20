// Cordless - grab your own Discord token from a logged-in browser tab.
// Finds your token in the page and copies it to your clipboard.
//
// Put the one-line version from docs/login.md in a bookmark's URL, open
// https://discord.com/app (logged in), and click the bookmark.
(() => {
  const TOKEN_RE = /^[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{5,}\.[A-Za-z0-9_-]{20,}$/;
  let delivered = false, timer;
  const origXHR   = XMLHttpRequest.prototype.setRequestHeader;
  const origFetch = window.fetch;

  const cleanup = () => {
    XMLHttpRequest.prototype.setRequestHeader = origXHR;
    window.fetch = origFetch;
    if (timer) clearTimeout(timer);
  };

  const deliver = (token) => {
    if (delivered || typeof token !== 'string' || !TOKEN_RE.test(token)) return;
    delivered = true;
    cleanup();
    const manual = () => prompt('Cordless: copy your token (Ctrl+C, then Enter):', token);
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(token).then(
        () => alert('Cordless: token copied to clipboard.\n\nPaste it into set-token.ps1.\nKeep it private -- it is full access to your account.'),
        manual
      );
    } else {
      manual();
    }
  };

  // 1. Find the webpack getToken() that returns a token-shaped value.
  try {
    window.webpackChunkdiscord_app.push([[Symbol('cordless')], {}, (req) => {
      for (const i in req.c) {
        try {
          const e = req.c[i] && req.c[i].exports;
          if (!e) continue;
          for (const c of [e.default, e.Z, e.ZP, e]) {
            if (c && typeof c.getToken === 'function') {
              let v; try { v = c.getToken(); } catch (_) {}
              if (typeof v === 'string' && TOKEN_RE.test(v)) { deliver(v); return; }
            }
          }
        } catch (_) {}
      }
    }]);
  } catch (_) {}
  if (delivered) return;

  // 2. Capture the Authorization header off the next request (XHR and fetch).
  XMLHttpRequest.prototype.setRequestHeader = function (key, val) {
    try { if (typeof key === 'string' && key.toLowerCase() === 'authorization') deliver(val); } catch (_) {}
    return origXHR.apply(this, arguments);
  };
  window.fetch = function (...args) {
    try {
      const h = args[1] && args[1].headers;
      const auth = h && (h.Authorization || h.authorization || (h.get && h.get('authorization')));
      if (auth) deliver(auth);
    } catch (_) {}
    return origFetch.apply(this, args);
  };
  timer = setTimeout(() => {
    if (!delivered) {
      cleanup();
      alert('Cordless: no token found yet.\n\nMake sure you are logged in, click a server or channel, and click the bookmark again. If it still misses, use the Copy-as-cURL method in docs/login.md.');
    }
  }, 20000);
})();
