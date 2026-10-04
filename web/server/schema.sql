CREATE TABLE IF NOT EXISTS accounts (
  id uuid PRIMARY KEY,
  username text UNIQUE NOT NULL CHECK (username ~ '^[a-z0-9_]{3,24}$'),
  password_hash text NOT NULL,
  created_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE IF NOT EXISTS sessions (
  token_hash text PRIMARY KEY,
  account_id uuid NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
  expires_at timestamptz NOT NULL
);
CREATE INDEX IF NOT EXISTS sessions_expiry ON sessions(expires_at);
CREATE TABLE IF NOT EXISTS cloud_saves (
  account_id uuid PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
  game text NOT NULL CHECK (game IN ('diamond', 'pearl', 'platinum')),
  name text NOT NULL CHECK (length(name) BETWEEN 1 AND 32),
  data bytea NOT NULL CHECK (octet_length(data) = 524288),
  sha256 text NOT NULL CHECK (sha256 ~ '^[0-9a-f]{64}$'),
  revision integer NOT NULL CHECK (revision > 0),
  updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE IF NOT EXISTS auth_limits (
  key text PRIMARY KEY,
  count integer NOT NULL,
  expires_at timestamptz NOT NULL
);
