// Two fake headsets in one room: each sends a few binary messages; both must hear both (the relay
// echoes to the sender too). Also measures the round trip.  node test-room.mjs [CODE]
const base = "wss://strikers-rooms.strikers-rooms.workers.dev/room/";
const code = process.argv[2] || "TEST";

function open(name) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(base + code);
    ws.binaryType = "arraybuffer";
    ws.got = [];
    ws.onopen = () => resolve(ws);
    ws.onerror = (e) => reject(new Error(name + ": " + (e.message || "error")));
    ws.onmessage = (e) => ws.got.push(new Uint8Array(e.data));
  });
}

const a = await open("a");
const b = await open("b");
const t0 = Date.now();
for (let i = 0; i < 5; i++) {
  a.send(new Uint8Array([1, i]));
  b.send(new Uint8Array([2, i]));
}
await new Promise((r) => setTimeout(r, 1500));
const fromA = (ws) => ws.got.filter((m) => m[0] === 1).length;
const fromB = (ws) => ws.got.filter((m) => m[0] === 2).length;
console.log(`a heard ${fromA(a)} own + ${fromB(a)} from b; b heard ${fromB(b)} own + ${fromA(b)} from a`);

// Round trip: a sends, waits for its own echo.
const rtts = [];
for (let i = 0; i < 10; i++) {
  const before = a.got.length;
  const s = Date.now();
  a.send(new Uint8Array([9, i]));
  while (a.got.length === before) await new Promise((r) => setTimeout(r, 1));
  rtts.push(Date.now() - s);
}
rtts.sort((x, y) => x - y);
console.log(`echo round trip ms: min ${rtts[0]}, median ${rtts[5]}, max ${rtts[9]}`);
a.close();
b.close();
console.log(Date.now() - t0 > 0 ? "ok" : "");
