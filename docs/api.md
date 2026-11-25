# PocketOS local API

Firmware `0.1.0-dev.3` serves the application at `/` and the local API under `/api/v1`. Use the management address printed on UART. The server accepts the AP address as the HTTP Host and requires the exact local Origin on mutations. The pinned SDK limits HTTP headers to 1024 bytes and the URI to 512 bytes. The HTTP server permits four open sockets, uses two-second socket I/O timeouts, and rejects transfer coding and payloads above 32 bytes.

| Method and endpoint | Result |
|---|---|
| GET `/system` | Chip, core count, firmware version, raw reset reason and ELF build ID |
| GET `/telemetry` | Cached readings, uptime, sample sequence, storage status and Guard summary |
| GET `/guard` | Policy version, lease cutoff, memory reserves and last Guard status |
| GET `/apps` | App registry and availability |
| GET `/services` | Management service state, dependencies and admission headroom |
| GET `/settings` | Redacted AP policy and browser polling preference |
| GET `/events` | Most recent redacted service and Guard events |
| POST `/auth/session` | Exchange the 32-character UART code once for a five-minute bearer token |
| DELETE `/auth/session` | End the operator session |
| PATCH `/settings` | Apply a refresh interval of 2, 3 or 5 seconds for the current boot |
| POST `/services/management/stop` | Queue a stop and return HTTP 202 with a job ID and `remainingMs` |
| GET `/jobs/{id}` | Read the stop job state and `remainingMs` while the connection remains available |
| POST `/jobs/{id}/cancel` | Cancel the queued job before its deadline and return the resulting state |

Login and settings bodies use `Content-Type: text/plain` without a newline. Management mutations and job status require the current `Authorization: Bearer …` header. Login attempts are spaced two seconds apart. UART `K` rotates the operator code and revokes the previous session. UART `A` starts an AP session because HTTP is unavailable when it is stopped. UART `S` requests immediate stop. Errors use an HTTP status and an `error.code` JSON field where the request reached the application router.

The 15-second browser cancellation window starts when the SDK reports that the HTTP acceptance response was sent. The supervisor can still act sooner on a Guard limit or an immediate UART stop. A successful cancellation returns `state: "cancelled"` and emits `MANAGEMENT STOP CANCELLED` with the job ID. UART logs `MANAGEMENT STOP SOURCE BROWSER` when the supervisor claims a browser stop and `MANAGEMENT STOP CONFIRMED` after release.

Management traffic uses local HTTP inside the WPA2 AP. The bearer token is held in browser memory and is cleared on logout or expiry. The device does not persist credentials or telemetry. Keep the UART provisioning text private. Server-side authorization protects the control routes. Transport confidentiality beyond the AP's link protection and persistent device configuration require separate product work.
