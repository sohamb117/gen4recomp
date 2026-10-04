export function Circuit() {
  return (
    <div className="circuit" aria-hidden="true">
      <svg viewBox="0 0 1440 1000" preserveAspectRatio="none">
        {Array.from({ length: 18 }, (_, i) => (
          <g key={i}>
            <path
              d={`M ${i * 86} 0 V ${80 + i * 18} C ${i * 63} ${500 - i * 9}, ${1300 - i * 50} ${100 + i * 33}, ${1440 - i * 30} 1000`}
            />
            <circle cx={i * 86} cy={80 + i * 18} r="3" />
            <path
              d={`M 0 ${i * 68} H ${250 + i * 24} L ${600 + i * 24} ${280 + i * 39} H 1440`}
            />
          </g>
        ))}
      </svg>
    </div>
  );
}
