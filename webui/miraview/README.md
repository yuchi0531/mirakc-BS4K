# miraview (bundled Web UI)

A static build of [miraview], a web frontend for mirakc, bundled with this
repository so that it can be served by mirakc itself via `server.mounts`.

* Upstream: https://github.com/maeda577/miraview
* Version: v0.1.2 (released 2022-09-17)
* License: MIT License, Copyright (c) 2022 maeda577
* Artifact: `build.tar.gz`
  * URL: https://github.com/maeda577/miraview/releases/download/v0.1.2/build.tar.gz
  * Size: 592,075 bytes
  * SHA256: `ed720ee41ee9e3752c2ba5c2a2b145aab44f885b445811c725c441566ddc2b83`

The contents of `build.tar.gz` are extracted into this directory as-is.  The
archive does not contain the upstream `LICENSE`, so it is added manually.

## Contents and provenance

| File | Description | Source |
| --- | --- | --- |
| `index.html` | Entry point of the SPA | upstream artifact |
| `static/` | Compiled JS/CSS bundles and source maps | upstream artifact |
| `asset-manifest.json`, `manifest.json` | CRA/PWA metadata | upstream artifact |
| `favicon.ico`, `logo192.png`, `logo512.png` | Icons | upstream artifact |
| `robots.txt` | Robots policy | upstream artifact |
| `LICENSE` | Upstream MIT license | https://raw.githubusercontent.com/maeda577/miraview/v0.1.2/LICENSE |
| `THIRD_PARTY_LICENSES.txt` | License notices for JavaScript libraries bundled into `static/js/main.*.js` (React, React DOM, scheduler, MUI, all MIT) | extracted from `static/js/main.5e0e84a9.js.LICENSE.txt` |

## Usage

See [docs/web-ui.md](../../docs/web-ui.md) for instructions on serving this
directory with `server.mounts`.

Notes when serving it under a sub-path such as `/miraview`:

* The asset references in `index.html` are relative (`./static/js/...`), so the
  UI works when mounted under any sub-path.
* The UI accesses the mirakc REST API on the same origin (`/api/*`).  mirakc
  does not provide CORS headers, so serving the UI from the same mirakc origin
  is required.
* Playback of BS4K (MMT/TLV) channels in a browser is not supported.  Use an
  external player such as VLC.

[miraview]: https://github.com/maeda577/miraview
