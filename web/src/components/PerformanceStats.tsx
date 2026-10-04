import { useEffect, useState } from "react";
import type { FrameProfiler } from "../runtime/performance";

export function PerformanceStats({
  profiler,
  state,
  onPause,
  onBenchmark,
}: {
  profiler: FrameProfiler;
  state: string;
  onPause: () => void;
  onBenchmark: () => void;
}) {
  const [sample, setSample] = useState(profiler.latest);
  const [stress, setStress] = useState(profiler.stress);
  useEffect(() => {
    const timer = setInterval(() => {
      setSample(profiler.latest);
      setStress(profiler.stress ? { ...profiler.stress } : null);
    }, 1000);
    return () => clearInterval(timer);
  }, [profiler]);
  return (
    <div className="performance-stats">
      <p>
        {state === "running"
          ? "Live · one-second samples"
          : "Last active sample · " + state}
      </p>
      {sample ? (
        <dl>
          {[
            ["Canvas FPS", sample.drawFps.toFixed(1)],
            ["Game ticks / second", sample.gameFps.toFixed(1)],
            ["Browser refresh / second", sample.refreshHz.toFixed(1)],
            ["Refreshes with a new frame", sample.freshFps.toFixed(1)],
            ["Worker time / frame", sample.workerMs.toFixed(2) + " ms"],
            [
              "Worker time · 95th percentile",
              sample.workerP95Ms.toFixed(2) + " ms",
            ],
            ["Worker round trip", sample.roundTripMs.toFixed(2) + " ms"],
            ["Canvas upload / frame", sample.drawMs.toFixed(2) + " ms"],
          ].map(([label, value]) => (
            <div key={label}>
              <dt>{label}</dt>
              <dd>{value}</dd>
            </div>
          ))}
        </dl>
      ) : (
        <p>Run the game for a few seconds to collect a sample.</p>
      )}
      <p>
        Canvas FPS counts both screens as one frame. Browser refresh uses
        requestAnimationFrame; it estimates repaint opportunities, not physical
        display scanout.
      </p>
      {stress && (
        <div className="benchmark-result">
          <p>
            {stress.active
              ? "Benchmark running"
              : stress.frames === stress.target
                ? "Benchmark complete"
                : "Benchmark stopped"}
            : {stress.frames.toLocaleString()} /{" "}
            {stress.target.toLocaleString()} frames
          </p>
          <p>
            {stress.fps.toFixed(1)} canvas FPS average ·{" "}
            {stress.seconds.toFixed(1)} s ·{" "}
            {stress.seconds ? (stress.fresh / stress.seconds).toFixed(1) : "0"}{" "}
            fresh refreshes/s
          </p>
        </div>
      )}
      {(state === "paused" || state === "running") && (
        <button disabled={stress?.active} onClick={onBenchmark}>
          Benchmark 12,000 frames · 300 FPS target
        </button>
      )}
      <p>
        The benchmark accelerates the game, then restores normal pacing. Keep
        this tab visible. Pause stops the test.
      </p>
      {(state === "paused" || state === "running") && (
        <button onClick={onPause}>
          {state === "paused" ? "Resume measurement" : "Pause measurement"}
        </button>
      )}
    </div>
  );
}
