import { useState, type FormEvent } from "react";
import { cloud, type CloudState } from "../cloud/client";
import type { SaveSlot } from "../storage";
export function CloudSave({
  state,
  running,
  onLoad,
}: {
  state: CloudState;
  running: boolean;
  onLoad: (slot: SaveSlot) => void;
}) {
  const [username, setUsername] = useState(""),
    [password, setPassword] = useState("");
  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    const create =
      (event.nativeEvent as SubmitEvent).submitter?.getAttribute("value") ===
      "create";
    await cloud.authenticate(username, password, create);
    setPassword("");
  }
  return (
    <section className="cloud-save" aria-label="Cloud save">
      {state.user ? (
        <>
          <div className="cloud-row">
            <span>Cloud save · {state.user.username}</span>
            <button disabled={state.busy} onClick={() => void cloud.logout()}>
              Sign out
            </button>
          </div>
          {state.save && (
            <div className="cloud-row">
              <span>
                {state.save.name} · {state.save.game} ·{" "}
                {new Date(state.save.updated).toLocaleString()}
              </span>
              <button
                disabled={state.busy || running}
                onClick={() =>
                  void cloud.load().then((slot) => slot && onLoad(slot))
                }
              >
                Load cloud save
              </button>
            </div>
          )}
          <p className="muted">
            One save per account. Choose a local save below to replace it.
          </p>
        </>
      ) : (
        <form onSubmit={(event) => void submit(event)} className="cloud-form">
          <span>Cloud save</span>
          <input
            aria-label="Username"
            autoComplete="username"
            placeholder="Username"
            minLength={3}
            maxLength={24}
            pattern="[A-Za-z0-9_]{3,24}"
            required
            value={username}
            onChange={(e) => setUsername(e.target.value)}
          />
          <input
            aria-label="Password"
            type="password"
            autoComplete="current-password"
            placeholder="Password"
            minLength={8}
            maxLength={128}
            required
            value={password}
            onChange={(e) => setPassword(e.target.value)}
          />
          <button disabled={state.busy} type="submit" value="login">
            Sign in
          </button>
          <button disabled={state.busy} type="submit" value="create">
            Create account
          </button>
        </form>
      )}
      {state.status && (
        <p className="cloud-status" role="status">
          {state.status}
        </p>
      )}
    </section>
  );
}
