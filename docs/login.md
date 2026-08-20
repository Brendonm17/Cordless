# Logging in to Cordless

Cordless signs in with your Discord user token. It reads that token once from
`ux0:data/cordless/token.txt`, then saves the session to your card, so you
normally only do this the first time. The only reason you'd redo it is if you
change your password or log out of all devices.

There are two steps. First you copy your token out of a browser, then you get it
onto the card.

## Step 1: Get your token

Log in to Discord in a desktop browser and grab your token one of these two ways.

### Bookmarklet (one click)

You set this up once. Make a browser bookmark, name it "Cordless token", and paste
the line below as its URL (it starts with `javascript:`):

```
javascript:(()=>{const R=/^[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{5,}\.[A-Za-z0-9_-]{20,}$/;let D=false,T;const oX=XMLHttpRequest.prototype.setRequestHeader,oF=window.fetch;const cl=()=>{XMLHttpRequest.prototype.setRequestHeader=oX;window.fetch=oF;if(T)clearTimeout(T)};const dv=t=>{if(D||typeof t!=='string'||!R.test(t))return;D=true;cl();const p=()=>prompt('Cordless: copy your token (Ctrl+C, then Enter):',t);if(navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(t).then(()=>alert('Cordless: token copied to clipboard. Paste it into set-token.ps1. Keep it private.'),p)}else{p()}};try{window.webpackChunkdiscord_app.push([[Symbol('c')],{},r=>{for(const i in r.c){try{const e=r.c[i]&&r.c[i].exports;if(!e)continue;for(const c of [e.default,e.Z,e.ZP,e]){if(c&&typeof c.getToken==='function'){let v;try{v=c.getToken()}catch(_){}if(typeof v==='string'&&R.test(v)){dv(v);return}}}}catch(_){}}}])}catch(_){}if(D)return;XMLHttpRequest.prototype.setRequestHeader=function(k,v){try{if(typeof k==='string'&&k.toLowerCase()==='authorization')dv(v)}catch(_){}return oX.apply(this,arguments)};window.fetch=function(...a){try{const h=a[1]&&a[1].headers;const A=h&&(h.Authorization||h.authorization||(h.get&&h.get('authorization')));if(A)dv(A)}catch(_){}return oF.apply(this,a)};T=setTimeout(()=>{if(!D){cl();alert('Cordless: no token yet. Log in, click a channel, and click the bookmark again. Else use Copy-as-cURL (docs/login.md).')}},20000)})()
```

Whenever you're logged in, click that bookmark and it copies your token straight
to the clipboard. If it says "no token yet", click into any channel first, then
click the bookmark again.

### By hand, in the Network tab

If you'd rather not run anything, you can just read your own request:

1. Open <https://discord.com/app> while logged in, press `F12`, and switch to the
   **Network** tab.
2. Type `api` into the filter box, then click any server or channel.
3. Pick one of the requests to `discord.com/api/...` and look under **Request
   Headers** for `authorization`.
4. That header's value is your token. Copy the whole thing.

## Step 2: Put it on the card

### With the script (Windows)

From the repo root, run:

```powershell
.\scripts\set-token.ps1
```

It grabs the token from your clipboard (or asks you to paste it), checks it
against Discord, finds your card, and writes `token.txt` in exactly the format the
Vita expects. Eject the card and launch Cordless.

### By hand (any OS)

You can also write the file yourself:

1. On the card, make a folder called `data/cordless`, so the full path is
   `ux0:data/cordless`.
2. Inside it, create `token.txt` and put your token in it, on one line with
   nothing else around it.
3. Save it as plain ASCII with no BOM and no trailing newline. The next section
   explains why that matters.

Then eject and launch.

## The token.txt trap

The file has to be nothing but the raw token: one line, plain ASCII, no
byte-order-mark, no trailing newline. The usual culprit is a UTF-8 BOM, which
Notepad's default "UTF-8" quietly adds. Those three invisible bytes sit in front
of your token and break it.

Safe ways to write it by hand:

- In Notepad, use Save As, set the encoding to **ANSI**, and quote the filename
  (`"token.txt"`) so Windows doesn't save it as `token.txt.txt`.
- In VS Code, pick **UTF-8** in the status bar instead of "UTF-8 with BOM", and
  don't let it add a final newline.
- Or skip all of that and let `set-token.ps1` handle it.

## Safety

- Your token is full access to your whole account, so guard it like a password.
  Keep it on your PC and your Vita card and nowhere else, and never share it.
- Never paste code into Discord's DevTools console. That "don't paste anything
  here" banner is there for a reason; the console is how a lot of accounts get
  stolen. Both the bookmarklet and the Network-tab method stay well clear of it.
- Don't hit Log Out in that browser once you've grabbed the token. Logging out
  kills it. Closing the tab is fine.
- Running a user token through a third-party client is against Discord's ToS, and
  that's a risk you're choosing to take. Cordless does its best to look like the
  real client and avoids hammering the API, but it can't make the risk zero. Log
  in once and let the saved session carry you.
