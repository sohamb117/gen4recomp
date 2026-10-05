/* Only ROM-derived assets are cached here. Save slots remain owned by pokeweb. */
let pending;
function database() {
  return (pending ??= new Promise((resolve, reject) => {
    const request = indexedDB.open("pokeweb-gen123-cache", 1);
    request.onupgradeneeded = () =>
      request.result.createObjectStore("extractions");
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  }));
}
async function cacheRead(key) {
  try {
    const db = await database();
    return await new Promise((resolve, reject) => {
      const r = db
        .transaction("extractions")
        .objectStore("extractions")
        .get(key);
      r.onsuccess = () => resolve(r.result);
      r.onerror = () => reject(r.error);
    });
  } catch {
    return undefined;
  }
}
async function cacheWrite(key, files) {
  try {
    const db = await database();
    await new Promise((resolve, reject) => {
      const tx = db.transaction("extractions", "readwrite");
      tx.objectStore("extractions").put(
        files.map(([path, bytes]) => [path, new Blob([bytes])]),
        key,
      );
      tx.oncomplete = resolve;
      tx.onerror = () => reject(tx.error);
      tx.onabort = () => reject(tx.error);
    });
  } catch (error) {
    console.warn(
      "Extraction cache unavailable; next launch will import again.",
      error,
    );
  }
}
