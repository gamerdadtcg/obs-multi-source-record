/**
 * OBS Multi Source Record — Stream Deck plugin (SDK v2, no npm deps).
 * Uses raw WebSockets for Elgato + obs-websocket 5 vendor requests.
 */
"use strict";

const WebSocket = require("ws");
const crypto = require("crypto");

let elgato = null;
const settingsByContext = {};
const actionByContext = {};
const DEFAULTS = { host: "127.0.0.1", port: "4455", password: "" };

const UUID_TOGGLE = "com.travvvman.obs-multi-source-record.toggle";
const UUID_PAUSE = "com.travvvman.obs-multi-source-record.pause";
const UUID_CHAPTER = "com.travvvman.obs-multi-source-record.chapter";
const UUID_SAVE_REPLAY = "com.travvvman.obs-multi-source-record.save-replay";
const UUID_SOURCE_RECORD = "com.travvvman.obs-multi-source-record.source-record";

function sendElgato(msg) {
  if (elgato && elgato.readyState === WebSocket.OPEN) elgato.send(JSON.stringify(msg));
}

function setState(context, state) {
  sendElgato({ event: "setState", context, payload: { state } });
}
function showOk(context) {
  sendElgato({ event: "showOk", context });
}
function showAlert(context) {
  sendElgato({ event: "showAlert", context });
}

function sha256(s) {
  return crypto.createHash("sha256").update(s).digest("base64");
}

function connectObs(settings) {
  const cfg = Object.assign({}, DEFAULTS, settings || {});
  const url = `ws://${cfg.host}:${cfg.port}`;
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, "obswebsocket.json");
    let identified = false;
    const pending = new Map();
    let nextId = 1;

    const api = {
      call(requestType, requestData) {
        const id = String(nextId++);
        return new Promise((res, rej) => {
          const t = setTimeout(() => {
            pending.delete(id);
            rej(new Error("OBS request timeout"));
          }, 5000);
          pending.set(id, {
            resolve: (v) => {
              clearTimeout(t);
              res(v);
            },
            reject: (e) => {
              clearTimeout(t);
              rej(e);
            },
          });
          ws.send(
            JSON.stringify({
              op: 6,
              d: { requestType, requestId: id, requestData: requestData || {} },
            })
          );
        });
      },
      close() {
        try {
          ws.close();
        } catch (_) {}
      },
    };

    ws.on("error", (err) => {
      if (!identified) reject(err);
    });
    ws.on("message", (raw) => {
      let msg;
      try {
        msg = JSON.parse(raw.toString());
      } catch (_) {
        return;
      }
      if (msg.op === 0) {
        const auth = msg.d.authentication;
        const identify = { op: 1, d: { rpcVersion: 1 } };
        if (auth && cfg.password) {
          const secret = sha256(cfg.password + auth.salt);
          identify.d.authentication = sha256(secret + auth.challenge);
        }
        ws.send(JSON.stringify(identify));
      } else if (msg.op === 2) {
        identified = true;
        resolve(api);
      } else if (msg.op === 7) {
        const p = pending.get(msg.d.requestId);
        if (!p) return;
        pending.delete(msg.d.requestId);
        if (msg.d.requestStatus && msg.d.requestStatus.result) p.resolve(msg.d);
        else p.reject(new Error((msg.d.requestStatus && msg.d.requestStatus.comment) || "OBS request failed"));
      }
    });
  });
}

async function vendor(settings, requestType) {
  const obs = await connectObs(settings);
  try {
    const res = await obs.call("CallVendorRequest", {
      vendorName: "obs-multi-source-record",
      requestType,
      requestData: {},
    });
    return (res.responseData && res.responseData.responseData) || res.responseData || {};
  } finally {
    obs.close();
  }
}

async function refresh(context) {
  try {
    const data = await vendor(settingsByContext[context], "status");
    const action = actionByContext[context];
    if (action === UUID_PAUSE) {
      setState(context, data.paused ? 1 : 0);
    } else if (action === UUID_SOURCE_RECORD) {
      setState(context, data.source_recording ? 1 : 0);
    } else {
      setState(context, data.recording ? 1 : 0);
    }
  } catch (_) {
    showAlert(context);
  }
}

async function onKey(context) {
  try {
    const action = actionByContext[context];
    const settings = settingsByContext[context];
    if (action === UUID_PAUSE) {
      const data = await vendor(settings, "toggle_pause");
      setState(context, data.paused ? 1 : 0);
    } else if (action === UUID_CHAPTER) {
      await vendor(settings, "add_chapter");
    } else if (action === UUID_SAVE_REPLAY) {
      await vendor(settings, "save_replay");
    } else if (action === UUID_SOURCE_RECORD) {
      const data = await vendor(settings, "toggle_source_record");
      setState(context, data.source_recording ? 1 : 0);
    } else {
      const data = await vendor(settings, "toggle");
      setState(context, data.recording ? 1 : 0);
    }
    showOk(context);
  } catch (_) {
    showAlert(context);
  }
}

function main() {
  const args = process.argv.slice(2);
  let port, uuid, registerEvent;
  for (let i = 0; i < args.length; i++) {
    if (args[i] === "-port") port = args[++i];
    else if (args[i] === "-pluginUUID") uuid = args[++i];
    else if (args[i] === "-registerEvent") registerEvent = args[++i];
  }
  elgato = new WebSocket("ws://127.0.0.1:" + port);
  elgato.on("open", () => sendElgato({ event: registerEvent, uuid }));
  elgato.on("message", (raw) => {
    let msg;
    try {
      msg = JSON.parse(raw.toString());
    } catch (_) {
      return;
    }
    const { event, context, payload, action } = msg;
    if (event === "willAppear") {
      settingsByContext[context] = (payload && payload.settings) || {};
      actionByContext[context] = action || UUID_TOGGLE;
      refresh(context);
      if (!global.__msrPoll) {
        global.__msrPoll = setInterval(() => {
          for (const ctx of Object.keys(actionByContext)) refresh(ctx);
        }, 2000);
      }
    } else if (event === "didReceiveSettings") {
      settingsByContext[context] = (payload && payload.settings) || {};
    } else if (event === "keyDown") {
      if (action) actionByContext[context] = action;
      onKey(context);
    }
  });
}

main();
