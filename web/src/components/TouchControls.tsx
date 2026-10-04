type Props = {
  onKey: (code: string, down: boolean) => void;
  disabled: boolean;
};

export function TouchControls({ onKey, disabled }: Props) {
  function control(
    label: string,
    code: string,
    position: string,
    name = label,
  ) {
    return (
      <button
        className={position}
        aria-label={name}
        disabled={disabled}
        onPointerDown={(event) => {
          event.preventDefault();
          event.currentTarget.setPointerCapture(event.pointerId);
          onKey(code, true);
        }}
        onPointerUp={() => onKey(code, false)}
        onPointerCancel={() => onKey(code, false)}
        onLostPointerCapture={() => onKey(code, false)}
        onKeyDown={(event) => {
          if (event.key === " " || event.key === "Enter") {
            event.preventDefault();
            onKey(code, true);
          }
        }}
        onKeyUp={(event) => {
          if (event.key === " " || event.key === "Enter") {
            event.preventDefault();
            onKey(code, false);
          }
        }}
        onBlur={() => onKey(code, false)}
      >
        {label}
      </button>
    );
  }

  return (
    <div className="touch-controls" role="group" aria-label="Game controls">
      <div className="shoulder-controls">
        {control("L", "KeyQ", "shoulder", "Left shoulder")}
        {control("R", "KeyE", "shoulder", "Right shoulder")}
      </div>
      <div
        className="control-pad d-pad"
        role="group"
        aria-label="Directional pad"
      >
        {control("↑", "ArrowUp", "north", "Up")}
        {control("←", "ArrowLeft", "west", "Left")}
        {control("→", "ArrowRight", "east", "Right")}
        {control("↓", "ArrowDown", "south", "Down")}
      </div>
      <div
        className="control-pad face-buttons"
        role="group"
        aria-label="Face buttons"
      >
        {control("X", "KeyC", "north")}
        {control("Y", "KeyV", "west")}
        {control("A", "KeyZ", "east")}
        {control("B", "KeyX", "south")}
      </div>
      <div className="system-controls">
        {control("SELECT", "Tab", "system-button")}
        {control("START", "Escape", "system-button")}
      </div>
    </div>
  );
}
