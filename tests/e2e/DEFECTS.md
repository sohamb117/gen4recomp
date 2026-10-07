# Suspected game/port defects found by the e2e milestones

Each entry: what was seen, a minimal repro, and what is known about the cause. "Suspected" until the cause is
pinned in the port or shown to be the cartridge's own behaviour.

## D/P: the GTS connect screen waits with no time-out (observation, not blocking)

Seen: the GTS receptionist's trade path opens "Connecting to Nintendo Wi-Fi Connection... Please wait a moment..."
and nothing changes for 1800+ frames; B cancels it ("Reconnect to Nintendo WFC?"), NO returns to the field.
The port models an unconfigured console (games/platinum/pc/src/pc_dwc_auth.c). Whether a real unconfigured DS
shows an error code here instead of waiting is [INFERENCE: likely, unverified]. diamond/97-gts-offline uses B + NO.
