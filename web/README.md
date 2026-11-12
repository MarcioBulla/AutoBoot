# AutoBootWeb

AutoBootWeb is an Astro and Vue sign-in page with a small Node.js authentication service.

## First-time setup

Build and start the web server:

```sh
bun install --frozen-lockfile
bun run build
AUTH_DATA_DIR="$HOME/.local/share/autobootweb" PORT=18082 bun run start
```

On first start, the service creates a one-time setup code at `AUTH_DATA_DIR/setup-token` with owner-only permissions. Read the code on the server and enter it in the first-time setup form; do not share it. Then choose a username and a password with at least 12 characters. The initial account can only be created once; there are no default credentials. The setup code is deleted after account creation.

For the local command above, read the setup code with:

```sh
cat "$HOME/.local/share/autobootweb/setup-token"
```

Open `http://localhost:18082` locally, or open the production site over HTTPS, to complete setup.

The service stores the password as a scrypt hash in `admin.json` under `AUTH_DATA_DIR`. It creates the data directory with owner-only access and the account file with mode `0600`. TOTP seeds are encrypted with AES-256-GCM; back up both `admin.json` and `totp-encryption.key` securely. Authentication sessions are held in memory and end when the service restarts.

Broker client keys, CSR state, certificates, CA certificates, and connection details are stored under `AUTH_DATA_DIR` with owner-only file permissions. The generated private key is never returned with the CSR or exported by the application. Include the broker files in protected backups.

After signing in, the Control workspace monitors a device's MQTT availability and PC state and sends `get`, `start`, and `restart` commands with UUIDv4 command IDs. The configurable Device ID defaults to `AutoBoot`; set it and the secure WebSocket connection in Settings → Broker connection. Once all broker credentials are saved, the backend connects automatically at service startup and subscribes to `<Device_ID>/availability`, `<Device_ID>/state`, and `<Device_ID>/result` at QoS 1. It publishes commands to `<Device_ID>/command/{get|start|restart}` at QoS 1 without retain, and matches results by `command_id`. Settings also contains Account security. The administrator can change their password, add or remove passkeys, and enable or disable an authenticator app. Every security change requires the current password. If a passkey or TOTP is configured, the administrator must also verify with one configured factor; when both are available, either one can be used. Passkeys use WebAuthn and need HTTPS or localhost. TOTP codes are required for password login after the authenticator is enabled.

## Running behind a reverse proxy

Keep the existing static site on port `18082`. The ColdLab Nginx runs in a rootless container, so the API uses the same private network namespace and listens on `127.0.0.1:18083` inside that namespace. The Quadlet manifest is `deploy/autobootweb-auth.container`; it runs a digest-pinned Node 24 LTS image and mounts the API source and persistent data directory separately.

Install the API source at `/opt/autobootweb/app/server.mjs`, create `/var/lib/autobootweb` owned by the `autobootweb` service user with mode `0700`, and install the Quadlet as `/etc/containers/systemd/autobootweb-auth.container`. Reload the system service manager and start `autobootweb-auth.service`.

In the HTTPS virtual host for `autoboot.example.com`, route API paths before the static-site catch-all:

```nginx
location ^~ /api/auth/ {
    proxy_pass http://127.0.0.1:18083;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-Proto https;
    proxy_set_header X-Forwarded-For $remote_addr;
}

location / {
    proxy_pass http://127.0.0.1:18082;
    proxy_set_header Host $host;
}
```

Preserve the original `Host` header. Pangolin terminates TLS before this Nginx, so forward the public HTTPS scheme explicitly. The Quadlet sets these production environment values:

```text
NODE_ENV=production
COOKIE_SECURE=true
TRUST_PROXY=true
AUTH_DATA_DIR=/path/to/private/persistent/data
API_ONLY=true
PORT=18083
```

Only set `TRUST_PROXY=true` when the service is reachable through a trusted reverse proxy. The service uses `HttpOnly`, `Secure` (in production), and `SameSite=Strict` session cookies, same-origin checks, password hashing, and per-address rate limiting. Keep port `18083` private to localhost and run the API as a supervised service so it restarts after host reboots.

## Deployment note

The current Forgejo workflow publishes static files only. Build a Node-compatible, single-file API bundle with `bun run bundle:auth`; deploy `build/auth/server.mjs` to `/opt/autobootweb/app/server.mjs` and restart the authentication Quadlet separately from the static release. The bundle contains its runtime dependencies and runs on the pinned Node 24 image. The site's origin must route `/api/auth/*` to that service. A static-only deployment displays a service-unavailable message instead of accepting credentials.

## Build

```sh
bun run build
```

The generated Astro assets are written to `dist/`.
