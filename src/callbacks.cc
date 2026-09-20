#include "callbacks.h"

#include <cstring>

#include "eosjs.h"

namespace eosjs {

State& GetState() {
  static State state;
  return state;
}

bool RequirePlatform(const Napi::Env& env) {
  if (GetState().platform != nullptr) return true;
  Napi::Error err = Napi::Error::New(
      env, "EOS platform is not initialised. Call eos.init(...) first.");
  err.Set("code", Napi::String::New(env, "EOS_NOT_INITIALIZED"));
  err.ThrowAsJavaScriptException();
  return false;
}

std::string ResultToString(EOS_EResult result) {
  const char* text = EOS_EResult_ToString(result);
  return text != nullptr ? std::string(text) : std::string("EOS_UnknownError");
}

Napi::Error MakeError(const Napi::Env& env, const char* operation,
                      EOS_EResult result) {
  const std::string code = ResultToString(result);
  Napi::Error err = Napi::Error::New(
      env, std::string(operation) + " failed: " + code);
  err.Set("code", Napi::String::New(env, code));
  err.Set("operation", Napi::String::New(env, operation));
  err.Set("resultCode", Napi::Number::New(env, static_cast<int>(result)));
  return err;
}

Napi::Value RejectedPromise(const Napi::Env& env, const char* operation,
                            EOS_EResult result) {
  auto deferred = Napi::Promise::Deferred::New(env);
  deferred.Reject(MakeError(env, operation, result).Value());
  return deferred.Promise();
}

std::string PuidToString(EOS_ProductUserId user_id) {
  if (user_id == nullptr || EOS_ProductUserId_IsValid(user_id) == EOS_FALSE) {
    return std::string();
  }
  char buffer[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = {0};
  int32_t length = static_cast<int32_t>(sizeof(buffer));
  if (EOS_ProductUserId_ToString(user_id, buffer, &length) !=
      EOS_EResult::EOS_Success) {
    return std::string();
  }
  return std::string(buffer);
}

EOS_ProductUserId PuidFromString(const std::string& value) {
  if (value.empty()) return nullptr;
  return EOS_ProductUserId_FromString(value.c_str());
}

std::string EpicIdToString(EOS_EpicAccountId account_id) {
  if (account_id == nullptr ||
      EOS_EpicAccountId_IsValid(account_id) == EOS_FALSE) {
    return std::string();
  }
  char buffer[EOS_EPICACCOUNTID_MAX_LENGTH + 1] = {0};
  int32_t length = static_cast<int32_t>(sizeof(buffer));
  if (EOS_EpicAccountId_ToString(account_id, buffer, &length) !=
      EOS_EResult::EOS_Success) {
    return std::string();
  }
  return std::string(buffer);
}

void EmitEvent(const Napi::Env& env, const char* name, Napi::Object payload) {
  State& state = GetState();
  if (state.emitter.IsEmpty()) return;

  Napi::HandleScope scope(env);
  state.emitter.Call({Napi::String::New(env, name), payload});

  // A throwing JS listener must not unwind through the SDK's C stack frame.
  // Surface it as an unhandled exception on the next JS turn instead.
  if (env.IsExceptionPending()) {
    Napi::Error pending = env.GetAndClearPendingException();
    pending.ThrowAsJavaScriptException();
  }
}

void CopyToFixedBuffer(const std::string& source, char* dest,
                       size_t dest_size) {
  if (dest_size == 0) return;
  const size_t count =
      source.size() < dest_size - 1 ? source.size() : dest_size - 1;
  std::memcpy(dest, source.data(), count);
  dest[count] = '\0';
}

}  // namespace eosjs
