/** Bounded scheduling; the browser resamples the core's 32728 Hz stereo. */
export class GameAudio {
  context = new AudioContext();
  gain = this.context.createGain();
  time = 0;
  constructor() {
    this.gain.connect(this.context.destination);
  }
  async resume() {
    if (this.context.state === "suspended") await this.context.resume();
  }
  volume(value: number) {
    this.gain.gain.value = value;
  }
  play(samples: Float32Array, rate: number, speed = 1) {
    if (!samples.length || !rate || this.context.state !== "running") return;
    const now = this.context.currentTime;
    if (this.time > now + 0.15) return;
    const n = samples.length / 2,
      buffer = this.context.createBuffer(2, n, rate);
    for (let c = 0; c < 2; c++) {
      const dst = buffer.getChannelData(c);
      for (let i = 0; i < n; i++) dst[i] = samples[i * 2 + c];
    }
    const source = this.context.createBufferSource();
    source.buffer = buffer;
    source.playbackRate.value = speed;
    source.connect(this.gain);
    this.time = Math.max(now + 0.025, this.time);
    source.start(this.time);
    this.time += n / rate / speed;
  }
  pause() {
    void this.context.suspend();
    this.time = 0;
  }
  close() {
    void this.context.close();
  }
}
