// N-API entry point. Exports four namespaces -- platform, connect, lobby, p2p
// -- plus the event sink that index.js registers to receive SDK notifications.
//
// Nothing here is the public API: index.js wraps all of it. Native stays as
// close to the C SDK as it can, and every convenience (the tick interval,
// promises over events, the Lobby object) lives in JS where it is cheap to
// change.

#include <napi.h>

#include "callbacks.h"
#include "eosjs.h"

namespace {

// _setEventSink(fn) -- fn(name, payload) receives every SDK notification.
// index.js registers one sink and fans out to an EventEmitter.
Napi::Value SetEventSink(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  eosjs::State& state = eosjs::GetState();

  if (info.Length() < 1 || info[0].IsNull() || info[0].IsUndefined()) {
    if (!state.emitter.IsEmpty()) state.emitter.Reset();
    return env.Undefined();
  }
  if (!info[0].IsFunction()) {
    Napi::TypeError::New(env, "_setEventSink(fn): fn must be a function")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  state.emitter = Napi::Persistent(info[0].As<Napi::Function>());
  return env.Undefined();
}

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
  exports.Set("_setEventSink", Napi::Function::New(env, SetEventSink));
  eosjs::platform::Init(env, exports);
  eosjs::connect::Init(env, exports);
  eosjs::lobby::Init(env, exports);
  eosjs::p2p::Init(env, exports);
  return exports;
}

}  // namespace

NODE_API_MODULE(eos, InitAll)
