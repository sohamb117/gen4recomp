import { test } from "node:test";
import assert from "node:assert/strict";
import { GameAudio } from "../src/runtime/audio";

test("fast-forward audio uses the complete batch at 2x without growing the queue", () => {
  const sources: { playbackRate: { value: number }; startTime: number }[] = [];
  const original = Object.getOwnPropertyDescriptor(globalThis, "AudioContext");
  class Context {
    currentTime = 1;
    state = "running";
    destination = {};
    createGain() {
      return { gain: { value: 1 }, connect() {} };
    }
    createBuffer(_channels: number, length: number) {
      const channels = [new Float32Array(length), new Float32Array(length)];
      return { getChannelData: (index: number) => channels[index] };
    }
    createBufferSource() {
      const source = {
        playbackRate: { value: 1 },
        startTime: 0,
        connect() {},
        start(time: number) {
          this.startTime = time;
        },
      };
      sources.push(source);
      return source;
    }
  }
  Object.defineProperty(globalThis, "AudioContext", {
    configurable: true,
    value: Context,
  });
  try {
    const sound = new GameAudio();
    const twoFrames = new Float32Array(3200);
    sound.play(twoFrames, 48000, 2);
    sound.play(twoFrames, 48000, 2);
    assert.equal(sources.length, 2);
    assert.equal(sources[0].playbackRate.value, 2);
    assert.ok(
      Math.abs(sources[1].startTime - sources[0].startTime - 1 / 60) < 1e-9,
    );
    sound.volume(0);
    assert.equal(sound.gain.gain.value, 0);
    sound.volume(0.65);
    assert.equal(sound.gain.gain.value, 0.65);
  } finally {
    if (original) Object.defineProperty(globalThis, "AudioContext", original);
    else Reflect.deleteProperty(globalThis, "AudioContext");
  }
});
