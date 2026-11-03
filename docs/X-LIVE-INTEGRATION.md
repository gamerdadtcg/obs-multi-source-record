# X Live from MSR dock — research & plan (Step 1)

**Status:** research complete; scaffolding on `feat/x-live-dock` @ `cce15bb`; draft PR #1 CI green (Windows/macOS/Ubuntu + format).  
**Do not** install on Travis’s PC or Go Live from this work.

Sources: [X Livestream API docs](https://github.com/xdevplatform/docs/tree/main/livestream-api) (2026), OAuth 2.0 PKCE guide, and Grok Bot `user-X` MCP livestream tools / `x-api-mcp-guide`.

---

## What Travis wants

From the MSR dock alone:

1. Sign in to X once (OAuth)
2. Auto-fetch RTMP server + stream key (create/reuse source)
3. Create / start / end broadcast
4. Set title
5. Show live status
6. Show **X live chat** in the dock

---

## What’s possible vs not

| Capability | Public Livestream API | `user-X` MCP today | Desktop OBS plugin |
|---|---|---|---|
| OAuth 2.0 PKCE user login | Yes (`broadcast.read` / `broadcast.write` / `offline.access`) | Bot connector only (not usable inside OBS) | **Yes** — Native App = public client + PKCE + loopback redirect |
| List/create/reuse RTMP **sources** (`rtmps_url`, `rtmp_stream_key`) | `GET/POST /2/users/:id/sources` | `list_sources`, `create_source`, … | **Yes** (after whitelist) |
| Immediate create broadcast | `POST /2/users/:id/broadcasts` | **Missing** | **Yes** via REST |
| Publish / End (go live now) | `PUT …/broadcasts/:id/state` `PUBLISH` / `END` | **Not supported** (“Going live right now is not supported”; only `go_live_scheduled_broadcast` for `manual_publish` schedules) | **Yes** via REST |
| Title on go-live | `title` on PUBLISH | Schedule `title` only | **Yes** |
| Live status / viewers | `GET` broadcast; source `is_stream_active` | `get_broadcast`, `get_source` | **Yes** |
| **Live** chat while RUNNING | **X Activity API** webhook `broadcast.chat` (needs hosted webhook + CRC) | No live stream of chat | **Hard** — plugin cannot reliably host XAA webhook alone |
| Chat **history** after END | `GET /2/broadcasts/:id/chat` | `get_broadcast_chat` | **Yes** |
| Send chat while live | `POST /2/broadcasts/:id/chat` | **Missing** | **Yes** via REST |
| Moderators / mute | Yes | Yes | Optional later |

### Hard blockers / gates

1. **Livestream API whitelist** — app must be approved via X’s Livestream API Access Form (docs list it under Enterprise / review). Without it, OAuth succeeds but source/broadcast calls 403.
2. **Account eligibility** — Premium (or follower threshold); private accounts cannot broadcast. MCP: `list_broadcasts` can 403 with that message.
3. **Live chat in-dock** — real-time path is **webhook-only** (X Activity). A pure desktop plugin needs either:
   - a small **relay** (Travis-owned HTTPS endpoint → WebSocket/SSE to the plugin), or
   - accept **post-stream history** + optional “Open chat on x.com” until a relay exists.
4. **Grok Bot X MCP cannot drive OBS** — different OAuth client; scopes on Travis’s bot connection currently **lack `broadcast.read`** (403 on `list_sources` during this research). Plugin must use its **own** X app credentials.

### Desktop OAuth PKCE — yes

X documents OAuth 2.0 Authorization Code + PKCE; Native App is a **public** client (no client secret in the plugin). Standard pattern for OBS:

1. Generate `code_verifier` / S256 `code_challenge`
2. Open system browser to `https://x.com/i/oauth2/authorize?...`
3. Local loopback listener `http://127.0.0.1:<port>/callback` (registered redirect URI)
4. Exchange code at `https://api.x.com/2/oauth2/token`
5. Store refresh token with OS secret store (Windows DPAPI / macOS Keychain); request `offline.access`

Never ship a client secret. Never log stream keys or tokens.

---

## Endpoint map (plugin will call)

```
GET  /2/region
GET  /2/users/:user_id/sources
POST /2/users/:user_id/sources          { name, region? }
GET  /2/users/:user_id/sources/:id
POST /2/users/:user_id/broadcasts       { source_id }
PUT  /2/users/:user_id/broadcasts/:id/state   PUBLISH | END
GET  /2/users/:user_id/broadcasts | GET /2/broadcasts/:id
GET  /2/broadcasts/:id/chat             (after END only)
POST /2/broadcasts/:id/chat             { text } while RUNNING
POST /2/activity/subscriptions          event_type=broadcast.chat  (relay)
```

Scopes: `broadcast.read`, `broadcast.write`, `offline.access` (+ optional `users.read`, `tweet.read`).

---

## Implementation plan

### Phase 0 — Access (Travis / operator)
- Create X Developer app (OAuth 2.0 Native, PKCE).
- Submit Livestream API access form; wait for whitelist.
- Register loopback redirect(s).
- Put **Client ID only** in plugin config (user-paste or build-time public id).

### Phase 1 — Auth + sources → MSR destination (this branch starts here)
- `MsrXLiveClient`: PKCE login, refresh, `users/me`, list/create source.
- On success, write `rtmps_url` + key into the existing MSR X destination (never display key in UI; show `key_len` / “configured”).
- Dock: Sign in / Sign out / “Sync source to destination”.

### Phase 2 — Go Live controls
- Create broadcast → optional wait for `is_stream_active` (MSR already streaming) → PUBLISH with title + `chat_option` → poll status (viewers, state) → END.
- Wire “Start MSR streams” optionally before PUBLISH so encoder is hot.
- **No auto Go Live** without explicit dock button.

### Phase 3a — Chat (history)
- After END, pull `GET …/chat` into a dock list (same as MCP `get_broadcast_chat`).

### Phase 3b — Live chat (relay)
- Tiny companion (Cloudflare Worker / Vercel): register XAA webhook, CRC, forward `broadcast.chat` to plugin over authenticated WebSocket.
- Dock QListWidget appends messages; optional send via `POST …/chat`.

### Phase 4 — Polish
- Manual-publish scheduled broadcasts as fallback.
- Moderator/mute controls.
- Match X encoder guidance (already partly in MSR x264 path).

---

## Out of scope / non-goals
- Using Grok Bot MCP from inside OBS.
- Embedding client secrets.
- Auto Go Live without a click.
- Installing builds on Travis’s PC from agent sessions unless he OK’s it.

---

## Scaffolding in this branch
- `docs/X-LIVE-INTEGRATION.md` (this file)
- `src/msr-x-live.hpp` / `src/msr-x-live.cpp` — client stub + status enum
- Dock placeholder group “X Live” (disabled until Client ID + whitelist)

---

## Build status (Step 1)

- Branch: `feat/x-live-dock` @ `cce15bb`
- Draft PR: https://github.com/gamerdadtcg/obs-multi-source-record/pull/1 (private)
- CI run 41712108995: **success** — clang-format, gersemi, Windows, macOS, Ubuntu 24.04
- Scaffold only: Sign in / Sync / Go Live stubs return not-implemented; no tokens/keys logged
- Not installed on Travis’s PC; OBS not touched; no Go Live

