// Super Mario Strikers (Quest) room server: a relay, nothing more. Each room code is one Durable
// Object; every headset in the room holds a WebSocket to it, and each binary message one headset
// sends goes to every headset in the room, the sender included (the game hears its own messages and
// skips them, and uses them to know the connection is alive). The game itself decides everything
// else (who hosts, the match setup, the lockstep): see src/platform/xr_net.cpp in the port.
//
//   wss://<worker>/room/<CODE>     CODE = 3-8 letters/digits, upper case
//
// Free plan: SQLite-backed Durable Objects with the WebSocket hibernation API, so an idle room
// costs nothing.

const MAX_PER_ROOM = 4; // two players, plus room for a reconnect overlapping a stale socket

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const m = url.pathname.match(/^\/room\/([A-Z0-9]{3,8})$/);
    if (!m) {
      return new Response("Super Mario Strikers Quest room server\n", {
        headers: { "content-type": "text/plain" },
      });
    }
    if (request.headers.get("Upgrade") !== "websocket") {
      return new Response("expected a WebSocket upgrade\n", { status: 426 });
    }
    const room = env.ROOMS.get(env.ROOMS.idFromName(m[1]));
    return room.fetch(request);
  },
};

export class Room {
  constructor(state, env) {
    this.state = state;
  }

  async fetch(request) {
    const open = this.state.getWebSockets();
    if (open.length >= MAX_PER_ROOM) {
      // Oldest first: a headset that reconnected leaves its dead socket behind for a while.
      try {
        open[0].close(4000, "room full, oldest dropped");
      } catch (e) {}
    }
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.state.acceptWebSocket(server);
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, message) {
    if (typeof message === "string" || message.byteLength > 4096) return; // game messages are small binary
    for (const peer of this.state.getWebSockets()) {
      try {
        peer.send(message);
      } catch (e) {}
    }
  }

  webSocketClose(ws, code, reason, wasClean) {
    try {
      ws.close(code, reason);
    } catch (e) {}
  }

  webSocketError(ws, error) {
    try {
      ws.close(1011, "error");
    } catch (e) {}
  }
}
