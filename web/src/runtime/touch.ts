/** Widescreen keeps the original 256x192 touchscreen centred. Account for
 * both HD pixels and object-fit letterboxing, using the actual last frame. */
export function mapTouch(
  x: number,
  y: number,
  boxWidth: number,
  boxHeight: number,
  frameWidth: number,
  frameHeight: number,
) {
  const fit = Math.min(boxWidth / frameWidth, boxHeight / frameHeight);
  const scale = frameHeight / 192;
  const px = (x - (boxWidth - frameWidth * fit) / 2) / fit;
  const py = (y - (boxHeight - frameHeight * fit) / 2) / fit;
  const dsX = (px - (frameWidth - 256 * scale) / 2) / scale;
  const dsY = py / scale;
  return {
    x: Math.max(0, Math.min(255, Math.floor(dsX))),
    y: Math.max(0, Math.min(191, Math.floor(dsY))),
    inside: dsX >= 0 && dsX < 256 && dsY >= 0 && dsY < 192,
  };
}
