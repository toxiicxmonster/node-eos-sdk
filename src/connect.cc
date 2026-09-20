// M2 -- Connect login. The cross-store piece.
//
// EOS has two identity systems. Auth is Epic Games accounts proper; Connect is
// a Product User ID (PUID), a per-product identity that any external account
// can map onto. Lobby and P2P address PUIDs, so Connect is what makes a Steam
// buyer and an Epic buyer addressable by the same call. This file wraps Connect
// only; Auth is out of scope (see docs/design.md section 2).
//
// A first login for an account returns EOS_InvalidUser plus a continuance
// token, which must be exchanged via EOS_Connect_CreateUser. That two-step is
// hidden here: callers see one promise that resolves to a PUID either way.

#include <map>
#include <string>

#include "callbacks.h"
#include "eosjs.h"

namespace eosjs {
namespace connect {
namespace {

EOS_NotificationId g_auth_expiration_notification = EOS_INVALID_NOTIFICATIONID;

EOS_HConnect Handle() {
  return EOS_Platform_GetConnectInterface(GetState().platform);
}

// Accepted `type` values, mapped to EOS external credential types.
//
// STEAM_APP_TICKET is deliberately absent: Epic deprecated it in favour of
// STEAM_SESSION_TICKET, and new integrations should not reach for it.
bool ParseCredentialType(const std::string& name,
                         EOS_EExternalCredentialType* out) {
  static const std::map<std::string, EOS_EExternalCredentialType> kTypes = {
      {"EPIC", EOS_EExternalCredentialType::EOS_ECT_EPIC},
      {"STEAM_SESSION_TICKET",
       EOS_EExternalCredentialType::EOS_ECT_STEAM_SESSION_TICKET},
      {"DEVICE_ID_ACCESS_TOKEN",
       EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN},
      {"OPENID_ACCESS_TOKEN",
       EOS_EExternalCredentialType::EOS_ECT_OPENID_ACCESS_TOKEN},
      {"DISCORD_ACCESS_TOKEN",
       EOS_EExternalCredentialType::EOS_ECT_DISCORD_ACCESS_TOKEN},
      {"GOG_SESSION_TICKET",
       EOS_EExternalCredentialType::EOS_ECT_GOG_SESSION_TICKET},
      {"APPLE_ID_TOKEN", EOS_EExternalCredentialType::EOS_ECT_APPLE_ID_TOKEN},
      {"GOOGLE_ID_TOKEN", EOS_EExternalCredentialType::EOS_ECT_GOOGLE_ID_TOKEN},
      {"ITCHIO_JWT", EOS_EExternalCredentialType::EOS_ECT_ITCHIO_JWT},
      {"ITCHIO_KEY", EOS_EExternalCredentialType::EOS_ECT_ITCHIO_KEY},
      {"AMAZON_ACCESS_TOKEN",
       EOS_EExternalCredentialType::EOS_ECT_AMAZON_ACCESS_TOKEN},
      {"XBL_XSTS_TOKEN", EOS_EExternalCredentialType::EOS_ECT_XBL_XSTS_TOKEN},
      {"PSN_ID_TOKEN", EOS_EExternalCredentialType::EOS_ECT_PSN_ID_TOKEN},
      {"NINTENDO_NSA_ID_TOKEN",
       EOS_EExternalCredentialType::EOS_ECT_NINTENDO_NSA_ID_TOKEN},
  };
  auto it = kTypes.find(name);
  if (it == kTypes.end()) return false;
  *out = it->second;
  return true;
}

Napi::Value PuidResult(Napi::Env env, EOS_ProductUserId user_id) {
  return Napi::String::New(env, PuidToString(user_id));
}

// login({ type, token, displayName? }) -> Promise<string productUserId>
Napi::Value Login(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();

  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "connect.login(options): options must be an object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object options = info[0].As<Napi::Object>();

  if (!options.Has("type") || !options.Get("type").IsString()) {
    Napi::TypeError::New(env, "connect.login: options.type must be a string")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string type_name =
      options.Get("type").As<Napi::String>().Utf8Value();
  EOS_EExternalCredentialType credential_type;
  if (!ParseCredentialType(type_name, &credential_type)) {
    Napi::TypeError::New(
        env, "connect.login: unsupported credential type '" + type_name + "'")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  // A device-id login carries no token; every other type requires one.
  const bool is_device_id =
      credential_type ==
      EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;
  std::string token;
  if (options.Has("token") && options.Get("token").IsString()) {
    token = options.Get("token").As<Napi::String>().Utf8Value();
  } else if (!is_device_id) {
    Napi::TypeError::New(env, "connect.login: options.token must be a string")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::string display_name;
  if (options.Has("displayName") && options.Get("displayName").IsString()) {
    display_name = options.Get("displayName").As<Napi::String>().Utf8Value();
  }

  EOS_Connect_Credentials credentials = {};
  credentials.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
  credentials.Token = token.empty() ? nullptr : token.c_str();
  credentials.Type = credential_type;

  EOS_Connect_UserLoginInfo user_login_info = {};
  user_login_info.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
  user_login_info.DisplayName =
      display_name.empty() ? nullptr : display_name.c_str();

  EOS_Connect_LoginOptions login_options = {};
  login_options.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
  login_options.Credentials = &credentials;
  // Required for device-id and Nintendo logins, harmless otherwise.
  login_options.UserLoginInfo =
      display_name.empty() ? nullptr : &user_login_info;

  using LoginOp = AsyncOp<EOS_Connect_LoginCallbackInfo>;
  auto* op = LoginOp::StartWithSettler(
      env, "EOS_Connect_Login",
      [](Napi::Env env, const EOS_Connect_LoginCallbackInfo* data,
         Napi::Promise::Deferred& deferred) {
        if (data->ResultCode == EOS_EResult::EOS_Success) {
          deferred.Resolve(PuidResult(env, data->LocalUserId));
          return;
        }

        // First login for this external account: exchange the continuance
        // token for a new PUID and settle the same promise with the result.
        if (data->ResultCode == EOS_EResult::EOS_InvalidUser &&
            data->ContinuanceToken != nullptr) {
          EOS_Connect_CreateUserOptions create_options = {};
          create_options.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
          create_options.ContinuanceToken = data->ContinuanceToken;

          using CreateOp = AsyncOp<EOS_Connect_CreateUserCallbackInfo>;
          auto* create = CreateOp::Adopt(
              deferred, "EOS_Connect_CreateUser",
              [](Napi::Env env, const EOS_Connect_CreateUserCallbackInfo* d) {
                return PuidResult(env, d->LocalUserId);
              });
          EOS_Connect_CreateUser(Handle(), &create_options,
                                 create->ClientData(), &CreateOp::OnComplete);
          return;
        }

        deferred.Reject(
            MakeError(env, "EOS_Connect_Login", data->ResultCode).Value());
      });

  EOS_Connect_Login(Handle(), &login_options, op->ClientData(),
                    &LoginOp::OnComplete);
  return op->Promise();
}

// createDeviceId(deviceModel) -> Promise<void>
//
// Device-id accounts are the cheapest way to get two distinct PUIDs on one
// machine, which is exactly what the loopback test needs. EOS_DuplicateNotAllowed
// means one already exists, which is success as far as callers are concerned.
Napi::Value CreateDeviceId(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();

  const std::string device_model =
      info.Length() > 0 && info[0].IsString()
          ? info[0].As<Napi::String>().Utf8Value()
          : std::string("node-eos-sdk");

  EOS_Connect_CreateDeviceIdOptions options = {};
  options.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
  options.DeviceModel = device_model.c_str();

  using Op = AsyncOp<EOS_Connect_CreateDeviceIdCallbackInfo>;
  auto* op = Op::StartWithSettler(
      env, "EOS_Connect_CreateDeviceId",
      [](Napi::Env env, const EOS_Connect_CreateDeviceIdCallbackInfo* data,
         Napi::Promise::Deferred& deferred) {
        if (data->ResultCode == EOS_EResult::EOS_Success ||
            data->ResultCode == EOS_EResult::EOS_DuplicateNotAllowed) {
          deferred.Resolve(env.Undefined());
        } else {
          deferred.Reject(
              MakeError(env, "EOS_Connect_CreateDeviceId", data->ResultCode)
                  .Value());
        }
      });

  EOS_Connect_CreateDeviceId(Handle(), &options, op->ClientData(),
                             &Op::OnComplete);
  return op->Promise();
}

// getLoggedInUsers() -> string[]
Napi::Value GetLoggedInUsers(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();

  EOS_HConnect handle = Handle();
  const int32_t count = EOS_Connect_GetLoggedInUsersCount(handle);
  Napi::Array users = Napi::Array::New(env, static_cast<size_t>(count));
  for (int32_t i = 0; i < count; ++i) {
    users.Set(static_cast<uint32_t>(i),
              Napi::String::New(
                  env, PuidToString(EOS_Connect_GetLoggedInUserByIndex(handle, i))));
  }
  return users;
}

// Steam session tickets expire after roughly 8 hours of continuous play, after
// which Connect logins start failing. The SDK warns before that happens; the
// event lets the host re-acquire a ticket instead of dropping a player
// mid-match. See the support article linked in docs/design.md section 11.
void EOS_CALL OnAuthExpiration(
    const EOS_Connect_AuthExpirationCallbackInfo* data) {
  if (data == nullptr) return;
  State& state = GetState();
  if (state.emitter.IsEmpty()) return;

  Napi::Env env = state.emitter.Env();
  Napi::HandleScope scope(env);
  Napi::Object payload = Napi::Object::New(env);
  payload.Set("productUserId",
              Napi::String::New(env, PuidToString(data->LocalUserId)));
  EmitEvent(env, "connect:auth-expiring", payload);
}

Napi::Value WatchAuthExpiration(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();
  if (g_auth_expiration_notification != EOS_INVALID_NOTIFICATIONID) {
    return env.Undefined();
  }

  EOS_Connect_AddNotifyAuthExpirationOptions options = {};
  options.ApiVersion = EOS_CONNECT_ADDNOTIFYAUTHEXPIRATION_API_LATEST;
  g_auth_expiration_notification = EOS_Connect_AddNotifyAuthExpiration(
      Handle(), &options, nullptr, &OnAuthExpiration);
  return env.Undefined();
}

}  // namespace

void Init(Napi::Env env, Napi::Object exports) {
  Napi::Object ns = Napi::Object::New(env);
  ns.Set("login", Napi::Function::New(env, Login));
  ns.Set("createDeviceId", Napi::Function::New(env, CreateDeviceId));
  ns.Set("getLoggedInUsers", Napi::Function::New(env, GetLoggedInUsers));
  ns.Set("watchAuthExpiration", Napi::Function::New(env, WatchAuthExpiration));
  exports.Set("connect", ns);
}

}  // namespace connect
}  // namespace eosjs
