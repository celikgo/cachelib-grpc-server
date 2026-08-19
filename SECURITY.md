# Security Policy

## Threat model — read this first

**This server has no authentication, no authorisation, and no TLS.** It listens
with `grpc::InsecureServerCredentials()`. Any client that can reach the port can
read, overwrite, and `Flush` the entire cache.

That is a deliberate design point, not an oversight: the server is meant to sit
on a trusted network behind something that terminates TLS and identity — a
service mesh sidecar, an ingress proxy, or a private subnet with security
groups. It is the same posture as a stock Redis or memcached deployment.

**Do not expose port 50051 to the public internet.** Concretely:

- Bind to a private interface, or publish the port only to an internal network.
- Terminate TLS and authenticate callers at a mesh sidecar or proxy.
- Treat `Flush` and `Scan` as privileged: `Flush` destroys the entire cache and
  `Scan` walks every key.
- The metrics endpoint on `:9090` exposes cache statistics, key counts, and hit
  rates. Keep it on an internal network too.

The cache holds whatever callers put in it. If that is personal or otherwise
sensitive data, remember that DRAM contents can reach disk via the NVM tier
(`--enable_nvm`) and via host swap; the flash tier is not encrypted at rest.

## Supported versions

Fixes land on the latest minor release. Older tags are not backported.

| Version | Supported |
|---|---|
| 1.6.x | Yes |
| < 1.6 | No |

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
