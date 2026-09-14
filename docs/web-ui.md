# Web UI (miraview)

mirakc doesn't provide its own Web UI.  Instead, it can serve any static Web UI
via [`server.mounts`](./config.md#servermounts).

This repository bundles a pre-built [miraview], a web frontend for mirakc.
miraview accesses the mirakc REST API on the same origin, and mirakc doesn't
send CORS headers, so serving it from the mirakc server itself is required.

The bundled files live in [webui/miraview](../webui/miraview).  See
[webui/miraview/README.md](../webui/miraview/README.md) for their provenance and
licensing.

## Docker

The Docker images built from this repository contain the bundled UI at
`/usr/share/miraview`.  Add the following to `config.yml`:

```yaml
server:
  mounts:
    /miraview:
      path: /usr/share/miraview
      index: index.html
```

[docker/config.yml](../docker/config.yml) already contains this setting.

Then open the following URL in a browser:

* http://localhost:40772/miraview

The `index` option redirects `/miraview` to `/miraview/index.html` (HTTP 303).
Don't append a trailing slash: `/miraview/` returns 404 because requests for
directories are handled by the mount fallback only at the exact mount point
(`/miraview`) and below.  Use `/miraview` as the canonical URL.

## Run from source

Specify the directory of the bundled UI in `config.yml`:

```yaml
server:
  mounts:
    /miraview:
      path: /path/to/mirakc/webui/miraview
      index: index.html
```

Then launch mirakc and open http://localhost:40772/miraview (see the note above
about the trailing slash).

For TOML configuration files:

```toml
[server.mounts."/miraview"]
path = "/path/to/mirakc/webui/miraview"
index = "index.html"
```

## Notes

* `path` must be an absolute path to an existing directory.
* The directory is served under `/miraview`; the assets referenced in
  `index.html` use relative paths, so the UI works under any sub-path.
* The UI uses the mirakc REST API on the same origin.  Don't serve the UI from
  a different origin, because mirakc doesn't support CORS.
* Playback of BS4K (MMT/TLV) channels in a browser is not supported.  Use an
  external player such as VLC.

## License

miraview is distributed under the [MIT License](../webui/miraview/LICENSE),
Copyright (c) 2022 maeda577.  The bundled JavaScript also contains MIT-licensed
libraries (React, React DOM, scheduler and MUI).  See
[THIRD_PARTY_LICENSES.txt](../webui/miraview/THIRD_PARTY_LICENSES.txt).

[miraview]: https://github.com/maeda577/miraview
