# Superseded HTTP benchmark configuration

This directory preserves the September 23, 2026 candidate campaign. It is
historical evidence, separate from the [1.8.0 release campaign](../RELEASE-1.8.0.md).
Use each run manifest's image identity and configuration hashes to distinguish
the campaigns; an unchanged server version string does not make the artifacts identical.

`http_service.before_reuse.py` is the exact `/work/http_service_strong.py`
extracted from client image
`sha256:4ed7ea272e276065c46112b2b9da1bfe47d025741941a959d638b3c99181d4a4`.
`nginx.before_keepalive.conf` is the NGINX bind-mounted configuration used
by `pilot-http-current`, `rc-http`, and `rc-http-secondary`.

The first 30-second `rc-http-secondary` one-pass adapter repetitions opened
a new origin TCP connection on every miss, exhausted ephemeral ports, and
reported 888 and 215 errors. Those raw runs and the generated
`http-secondary-before-reuse` summary are deliberately retained as failed
benchmark attempts. The release-candidate CacheLib executable was unchanged.

The current `http_service.py` reuses a per-handler origin connection, and
the current `nginx.conf` enables NGINX upstream keepalive. Later
`run_http.py` manifests include both configuration hashes and a snapshot
of the exact NGINX file mounted into each run. A fresh one-pass series
qualifies this corrected benchmark path. Do not pool the two adapter and
NGINX configurations.
