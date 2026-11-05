# vendored runtime assets

- `vconsole.min.js` — [vConsole](https://github.com/Tencent/vConsole) 3.15.1, MIT.
  Downloaded from `https://unpkg.com/vconsole@3.15.1/dist/vconsole.min.js`.
  The Win32 shell injects it from the right-click menu / `=` menu
  (`注入 vConsole`); CMake copies it next to the binaries.

It is a debugging payload for the shell, **not** part of the page runtime:
never load it from `prelude.js` / `polyfills.js`.
