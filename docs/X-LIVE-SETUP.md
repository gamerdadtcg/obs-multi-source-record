# X Live setup (bring your own Client ID)

Multi Source Record’s **X Live** dock lets each streamer use **their own** X developer app Client ID. Travis’s (or anyone else’s) Client ID is never baked into the plugin. API spend is billed to **whoever owns the Client ID** you paste.

> Safety: MSR never starts an X broadcast until you click **Go Live on X**. Syncing a source only fills the RTMPS destination (stream key is stored masked and never logged).

## 1. Create an X developer app

1. Open [X Developer Console](https://console.x.com/) and create a Project + App (or reuse one you own).
2. Under **User authentication settings**:
   - App type: **Native App** (public client — **no client secret** in OBS).
   - Enable **OAuth 2.0**.
3. Callback / redirect URI (exact):

   ```text
   http://127.0.0.1:8765/callback
   ```

4. Scopes (OAuth 2.0):
   - `broadcast.read`
   - `broadcast.write`
   - `offline.access` (refresh token so you stay signed in)
   - `users.read`

5. Copy the app’s **Client ID** (OAuth 2.0). You will paste this into the MSR dock. It is **not** a secret for Native App / PKCE, but treat it as yours — anyone who uses it can burn **your** API credits.

## 2. Request Livestream API access (whitelist)

Livestream REST endpoints (`/2/users/:id/sources`, broadcasts, chat) require **whitelist** approval. Until approved, MSR shows a clear error with this form:

**[Livestream API Access Form](https://docs.x.com/forms/livestream-api-access.md)**

Submit the form for the same app / Client ID you will use in MSR. Without whitelist, Sign-in may work but **Sync source** / **Go Live** will fail with HTTP 403.

## 3. Configure MSR

1. Install a build that includes X Live (this branch / release notes).
2. Open the **Multi Source Record** dock → expand **X Live (bring your own Client ID)**.
3. Paste **your** Client ID → **Save** (or Sign in, which also persists it).
4. Click **Sign in to X** — your system browser opens; approve; the loopback page says you can return to OBS.
5. Click **Sync source → destination**. MSR will GET your existing source or POST create one, then fill an **X** / **X Live** multistream destination’s server URL. The stream key is written into the password-style field and **never shown in status text or logs**.
6. Start your encoder / MSR multistream to that destination when you are ready (separate from X “Go Live”).
7. Enter a **Broadcast title**, then click **Go Live on X** only when you intend to publish.
8. Click **End X broadcast** when finished. Status polling (every **60 seconds**) runs **only while live** to save API cost.
9. Chat v1: **Send chat** while live; **Load chat history** only after End (no live chat polling).

## 4. Spending-limit tip

X API is **pay-per-use**, tracked on the **app (Client ID)** — not on each OAuth end-user. Set a spending limit / budget alert on [console.x.com](https://console.x.com/) for your app. Prefer:

- One Client ID **per streamer / org** (BYOK).
- Sync source once; reuse the cached source.
- Go Live / End only on explicit clicks.
- 60s status poll only while live (MSR default).
- No live chat polling in v1.

If you share one Client ID with many streamers, **you** pay for all of their Livestream + related API traffic.

## 5. Troubleshooting

| Symptom | Likely cause |
| -------- | ------------- |
| “Set your X OAuth Client ID first” | Empty Client ID field |
| Cannot bind `127.0.0.1:8765` | Another app is using that port; close it and retry Sign in |
| HTTP 403 + whitelist message | App not approved — use the [Livestream API form](https://docs.x.com/forms/livestream-api-access.md) |
| Sync works, Go Live fails | Encoder not pushing yet, or title/state rejected — check status text |
| Tokens lost after reinstall | Tokens live in the OBS module config file `x-live-tokens.json` (never logged); Sign in again |

## References

- [X Livestream API — typical workflow](https://docs.x.com/livestream-api/typical-workflow)
- [X API pricing](https://docs.x.com/x-api/getting-started/pricing)
- Internal research notes: `docs/X-LIVE-INTEGRATION.md`
