// The SDK callback -> JS promise bridge.
//
// §6 of docs/design.md: every async EOS call takes a ClientData void pointer
// that comes back in the completion callback. Getting its lifetime wrong gives
// you either a leak on every lobby operation or a use-after-free that crashes
// minutes into a match. So it is written exactly once, here, and every call
// site uses it. Do not hand-roll this per call.
//
// Usage:
//
//   auto* op = AsyncOp<EOS_Lobby_CreateLobbyCallbackInfo>::Start(
//       env, "EOS_Lobby_CreateLobby",
//       [](Napi::Env env, const EOS_Lobby_CreateLobbyCallbackInfo* info) {
//         return Napi::String::New(env, info->LobbyId);
//       });
//   EOS_Lobby_CreateLobby(handle, &options, op->ClientData(), op->Callback());
//   return op->Promise();
//
// Ownership: Start() heap-allocates. The op deletes itself in OnComplete after
// settling the promise. The only path where it leaks is an EOS call that never
// invokes its callback, which the SDK does not do as long as the platform is
// ticking -- see the note on Abandon() below for shutdown.

#pragma once

#include <napi.h>

#include <functional>
#include <string>
#include <utility>

#include "eosjs.h"

namespace eosjs {

template <typename TInfo>
class AsyncOp {
 public:
  // Maps a successful callback payload to the promise's resolution value.
  using Mapper = std::function<Napi::Value(Napi::Env, const TInfo*)>;
  // Full control over settlement, for calls whose "failure" results are
  // actually continuations (EOS_Connect_Login returning EOS_InvalidUser, for
  // instance). A custom settler owns the deferred; if it does not settle it,
  // it must keep something alive that will.
  using Settler =
      std::function<void(Napi::Env, const TInfo*, Napi::Promise::Deferred&)>;

  static AsyncOp* Start(Napi::Env env, const char* operation,
                        Mapper mapper = nullptr) {
    return new AsyncOp(env, operation, std::move(mapper), nullptr);
  }

  static AsyncOp* StartWithSettler(Napi::Env env, const char* operation,
                                   Settler settler) {
    return new AsyncOp(env, operation, nullptr, std::move(settler));
  }

  // Continues an operation that is already holding a promise, for the cases
  // where one JS call fans out into two SDK calls (login -> create user). The
  // adopted deferred is settled by this op instead of a fresh one, so the
  // caller sees a single promise for the whole sequence.
  static AsyncOp* Adopt(Napi::Promise::Deferred deferred, const char* operation,
                        Mapper mapper) {
    return new AsyncOp(std::move(deferred), operation, std::move(mapper));
  }

  void* ClientData() { return static_cast<void*>(this); }

  // The C function pointer to hand to the SDK.
  static void EOS_CALL OnComplete(const TInfo* data) {
    if (data == nullptr || data->ClientData == nullptr) return;

    // Some EOS operations report intermediate results before they finish.
    // Leave the op alive for the terminal callback.
    if (!EOS_EResult_IsOperationComplete(data->ResultCode)) return;

    auto* self = static_cast<AsyncOp<TInfo>*>(
        const_cast<void*>(static_cast<const void*>(data->ClientData)));
    self->Settle(data);
    delete self;
  }

  Napi::Promise Promise() const { return deferred_.Promise(); }

  const char* operation() const { return operation_.c_str(); }
  Napi::Promise::Deferred& deferred() { return deferred_; }

 private:
  AsyncOp(Napi::Env env, const char* operation, Mapper mapper, Settler settler)
      : deferred_(Napi::Promise::Deferred::New(env)),
        operation_(operation),
        mapper_(std::move(mapper)),
        settler_(std::move(settler)) {}

  AsyncOp(Napi::Promise::Deferred deferred, const char* operation, Mapper mapper)
      : deferred_(std::move(deferred)),
        operation_(operation),
        mapper_(std::move(mapper)),
        settler_(nullptr) {}

  void Settle(const TInfo* data) {
    Napi::Env env = deferred_.Env();
    Napi::HandleScope scope(env);

    if (settler_) {
      settler_(env, data, deferred_);
      return;
    }

    if (data->ResultCode == EOS_EResult::EOS_Success) {
      deferred_.Resolve(mapper_ ? mapper_(env, data) : env.Undefined());
    } else {
      deferred_.Reject(
          MakeError(env, operation_.c_str(), data->ResultCode).Value());
    }
  }

  Napi::Promise::Deferred deferred_;
  std::string operation_;
  Mapper mapper_;
  Settler settler_;
};

// Rejects a promise immediately for a synchronous EOS failure, so call sites
// can keep a single promise-shaped return path.
Napi::Value RejectedPromise(const Napi::Env& env, const char* operation,
                            EOS_EResult result);

}  // namespace eosjs
