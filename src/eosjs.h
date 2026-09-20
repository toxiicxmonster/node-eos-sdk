// Shared declarations for the node-eos-sdk addon.
//
// NOTE ON SDK SIGNATURES: the EOS option structs, ApiVersion constants and
// callback shapes used throughout src/ are written from Epic's published API
// documentation and from the community bindings, not from a pinned copy of the
// headers (the SDK is not redistributable, so this repository cannot contain
// one). Epic revise the C API between versions. If a field name or an
// EOS_*_API_LATEST constant has moved, the compiler will say so precisely.
// See docs/sdk-setup.md for the SDK version this was written against.

#pragma once

#include <napi.h>

#include <string>

extern "C" {
#include "eos_sdk.h"
#include "eos_common.h"
#include "eos_logging.h"
#include "eos_connect.h"
#include "eos_lobby.h"
#include "eos_p2p.h"
}

namespace eosjs {

// Process-wide addon state.
//
// EOS_Initialize may be called at most once per process and cannot be called
// again after EOS_Shutdown, so a single process-global is the honest model
// here rather than per-Environment instance data: two Node workers loading this
// addon would be contending for one SDK anyway. Platform::Init guards both
// transitions and throws rather than letting the SDK fail opaquely.
struct State {
  EOS_HPlatform platform = nullptr;
  bool initialized = false;   // EOS_Initialize has been called
  bool shut_down = false;     // EOS_Shutdown has been called; no way back
  bool debug_logging = false;
  Napi::FunctionReference emitter;  // JS side event sink; see EmitEvent
};

State& GetState();

// True if the platform exists; throws a JS error and returns false otherwise.
bool RequirePlatform(const Napi::Env& env);

// EOS_EResult -> "EOS_Success" etc.
std::string ResultToString(EOS_EResult result);

// Builds a JS Error carrying `.code` (the EOS result string) and `.operation`,
// so callers can branch on the result rather than parsing a message.
Napi::Error MakeError(const Napi::Env& env, const char* operation,
                      EOS_EResult result);

// EOS_ProductUserId <-> string. An invalid or null id yields an empty string.
std::string PuidToString(EOS_ProductUserId user_id);
EOS_ProductUserId PuidFromString(const std::string& value);

// EOS_EpicAccountId -> string (used only for Auth-sourced ids).
std::string EpicIdToString(EOS_EpicAccountId account_id);

// Dispatches ('name', payload) to the JS event sink registered by
// _setEventSink. Safe to call when no sink is registered (it is a no-op).
//
// Every caller is inside EOS_Platform_Tick(), which this addon only ever calls
// from the JS main thread, so this reaches JS directly with no ThreadSafeFunction
// hop. That is the whole reason for the Option A tick model in docs/design.md.
void EmitEvent(const Napi::Env& env, const char* name, Napi::Object payload);

// Copies a JS string into a fixed-size char buffer, truncating safely.
void CopyToFixedBuffer(const std::string& source, char* dest, size_t dest_size);

namespace platform {
void Init(Napi::Env env, Napi::Object exports);
}  // namespace platform

namespace connect {
void Init(Napi::Env env, Napi::Object exports);
}

namespace lobby {
void Init(Napi::Env env, Napi::Object exports);
void ClearNotifications();
}

namespace p2p {
void Init(Napi::Env env, Napi::Object exports);
void ClearNotifications();
// Drains the receive queue; called once per tick from platform.cc.
void DrainPackets(const Napi::Env& env);
}  // namespace p2p

}  // namespace eosjs
