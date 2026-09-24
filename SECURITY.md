# Security Policy

## Threat model — read this first

**This server has no authentication, no authorisation, and no TLS.** It listens
with `grpc::InsecureServerCredentials()`. Any client that can reach the port can
read and overwrite keys, delete entries, and invoke administrative RPCs.

Deploy the server on a private network with access restricted by firewall
rules or security groups. Use a service mesh sidecar or ingress proxy to
terminate TLS and authenticate callers. Network isolation limits reachability;
it does not provide encryption or caller authentication.

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

Upgrade to **1.8.0 or a later supported patch release** for the numeric-value
corruption/overflow, premature counter/CAS expiry, and pathological `Scan`
pattern fixes described in [CHANGELOG.md](CHANGELOG.md). Verify the running
version and image digest against the
[release assets](https://github.com/celikgo/cachelib-grpc-server/releases/tag/v1.8.0);
do not infer the running version from the moving `:latest` tag alone.

| Version | Supported |
|---|---|
| 1.8.x | Yes; current supported minor release |
| 1.7.x | Never published |
| <= 1.6.x | No backports; upgrade to 1.8.x |

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
