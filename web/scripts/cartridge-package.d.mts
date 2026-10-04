import type { HostedCartridge } from "../src/cartridges";
export function packCartridge(game: string, data: Uint8Array): { payload: Buffer; entry: HostedCartridge };
