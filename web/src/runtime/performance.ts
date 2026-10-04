export type PerformanceSample = {
  seconds: number;
  drawFps: number;
  gameFps: number;
  refreshHz: number;
  freshFps: number;
  workerMs: number;
  workerP95Ms: number;
  roundTripMs: number;
  drawMs: number;
};

/** Measures whole dual-screen frames. No React updates in the frame loop. */
export class FrameProfiler {
  latest: PerformanceSample | null = null;
  stress: {
    active: boolean;
    frames: number;
    target: number;
    started: number;
    seconds: number;
    fps: number;
    refreshes: number;
    fresh: number;
    workerMs: number;
    drawMs: number;
  } | null = null;
  startStress(now: number, target = 12000) {
    this.stress = {
      active: true,
      frames: 0,
      target,
      started: now,
      seconds: 0,
      fps: 0,
      refreshes: 0,
      fresh: 0,
      workerMs: 0,
      drawMs: 0,
    };
  }
  private started = 0;
  private active = false;
  private draws = 0;
  private ticks = 0;
  private refreshes = 0;
  private fresh = 0;
  private dirty = false;
  private worker: number[] = [];
  private roundTrip = 0;
  private drawTime = 0;

  begin(now: number) {
    this.active = true;
    this.reset(now);
  }
  end() {
    this.active = false;
    if (this.stress) this.stress.active = false;
  }
  private reset(now: number) {
    this.started = now;
    this.draws = this.ticks = this.refreshes = this.fresh = 0;
    this.roundTrip = this.drawTime = 0;
    this.worker = [];
    this.dirty = false;
  }
  rendered(
    ticks: number,
    workerMs: number,
    roundTripMs: number,
    drawMs: number,
  ) {
    if (!this.active) return;
    this.draws++;
    this.ticks += ticks;
    this.dirty = true;
    this.worker.push(workerMs);
    this.roundTrip += roundTripMs;
    this.drawTime += drawMs;
    const stress = this.stress;
    if (stress?.active) {
      stress.frames++;
      stress.seconds = (performance.now() - stress.started) / 1000;
      stress.fps = stress.frames / stress.seconds;
      stress.workerMs += workerMs;
      stress.drawMs += drawMs;
      if (stress.frames >= stress.target) stress.active = false;
    }
  }
  refresh(now: number) {
    if (!this.active) return;
    this.refreshes++;
    if (this.stress?.active) {
      this.stress.refreshes++;
      if (this.dirty) this.stress.fresh++;
    }
    if (this.dirty) this.fresh++;
    this.dirty = false;
    const seconds = (now - this.started) / 1000;
    if (seconds < 1) return;
    const sorted = [...this.worker].sort((a, b) => a - b);
    const count = this.draws || 1;
    this.latest = {
      seconds,
      drawFps: this.draws / seconds,
      gameFps: this.ticks / seconds,
      refreshHz: this.refreshes / seconds,
      freshFps: this.fresh / seconds,
      workerMs: this.worker.reduce((sum, n) => sum + n, 0) / count,
      workerP95Ms:
        sorted[Math.max(0, Math.ceil(sorted.length * 0.95) - 1)] || 0,
      roundTripMs: this.roundTrip / count,
      drawMs: this.drawTime / count,
    };
    this.reset(now);
  }
}
