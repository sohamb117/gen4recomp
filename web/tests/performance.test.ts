import { test } from "node:test";
import assert from "node:assert/strict";
import { FrameProfiler } from "../src/runtime/performance";

test("12,000 drawn frames at 300 FPS remain distinct from 30 Hz refresh", (t) => {
  let now = 0;
  t.mock.method(performance, "now", () => now);
  const profiler = new FrameProfiler();
  profiler.begin(0);
  profiler.startStress(0);
  for (let frame = 1; frame <= 12000; frame++) {
    now = (frame * 1000) / 300;
    profiler.rendered(1, 2, 2.2, 0.1);
    if (frame % 10 === 0) profiler.refresh(now);
  }
  assert.equal(profiler.stress?.frames, 12000);
  assert.equal(profiler.stress?.active, false);
  assert.equal(profiler.stress?.fps, 300);
  assert.equal(profiler.stress?.seconds, 40);
  assert.ok(Math.abs(profiler.latest!.drawFps - 300) < 0.01);
  assert.ok(Math.abs(profiler.latest!.refreshHz - 30) < 0.01);
  assert.ok(Math.abs(profiler.latest!.freshFps - 30) < 0.01);
  assert.equal(profiler.latest!.workerP95Ms, 2);
});

test("batched game ticks do not inflate draw FPS, and paused time is excluded", () => {
  const profiler = new FrameProfiler();
  profiler.begin(0);
  for (let frame = 1; frame <= 60; frame++) {
    profiler.rendered(2, 4, 4.2, 0.1);
    profiler.refresh((frame * 1000) / 60);
  }
  assert.equal(profiler.latest!.drawFps, 60);
  assert.equal(profiler.latest!.gameFps, 120);
  const sample = profiler.latest;
  profiler.end();
  profiler.rendered(1, 1000, 1000, 1000);
  profiler.refresh(20000);
  assert.equal(profiler.latest, sample);
  profiler.begin(20000);
  for (let frame = 1; frame <= 60; frame++) {
    profiler.rendered(1, 4, 4.2, 0.1);
    profiler.refresh(20000 + (frame * 1000) / 60);
  }
  assert.equal(profiler.latest!.gameFps, 60);
});
