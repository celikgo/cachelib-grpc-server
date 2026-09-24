# Security Policy

## Threat model — read this first

**This server has no authentication, no authorisation, and no TLS.** It listens
with `grpc::InsecureServerCredentials()`. Any client that can reach the port can
read and overwrite keys, delete entries, and invoke administrative RPCs.

That is a deliberate design point, not an oversight: the server is meant to sit
on a trusted network behind something that terminates TLS and identity — a
service mesh sidecar, an ingress proxy, or a private subnet with security
groups.

**Do not expose port 50051 to the public internet.** Concretely:

- Bind to a private interface, or publish the port only to an internal network.
- Terminate TLS and authenticate callers at a mesh sidecar or proxy.
- Treat `Flush` and `Scan` as privileged: they mutate or enumerate DRAM-resident
  entries. With the flash tier enabled, `Flush` leaves flash-only keys behind;
  it is not a secure erase operation.
- The metrics endpoint on `:9090` exposes cache statistics, key counts, and hit
  rates. Keep it on an internal network too.

The cache holds whatever callers put in it. If that is personal or otherwise
sensitive data, remember that DRAM contents can reach disk via the NVM tier
(`--enable_nvm`) and via host swap; the flash tier is not encrypted at rest.
Startup truncates the Navy file and cached values are not restored, but
truncation is not a guarantee of physical-media sanitization.

## Supported versions

Fixes land on the current development line and the latest supported minor
release. Older tags are not backported.

<!-- RELEASE_STATUS: finalize support policy for 1.8.0 publication before tagging. -->
The 1.8.0 release is being qualified. Its source fixes the older numeric-value
corruption/overflow and pathological `Scan` pattern defects described in
[CHANGELOG.md](CHANGELOG.md). Do not infer that `:latest` contains these fixes
until its published version and digest have been verified.

| Version | Supported |
|---|---|
| 1.8.x | Publication pending; fixes land on this development line |
| 1.7.x | Never published |
| <= 1.6.x | No backports; upgrade after 1.8.0 publication |

## Reporting a vulnerability

Report privately through GitHub's
[private vulnerability reporting](https://github.com/celikgo/cachelib-grpc-server/security/advisories/new)
on this repository. Please do not open a public issue for anything exploitable.

Include a description, affected version or image digest, and reproduction steps.
Expect an acknowledgement within 7 days and an assessment within 30. This is a
personally maintained project, not a vendor product with an on-call rotation —
timelines are best-effort.

Vulnerabilities in **CacheLib itself** belong to
[Meta's disclosure process](https://github.com/facebook/CacheLib/security),
not here. Issues in this server's gRPC layer, container image, or cache
lifecycle handling belong here.
