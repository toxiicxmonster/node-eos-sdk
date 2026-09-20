// M1 -- Platform lifecycle: EOS_Initialize, EOS_Platform_Create,
// EOS_Platform_Tick, EOS_Platform_Release, EOS_Shutdown.
//
// The tick is driven from JS (see index.js) on a 50 ms interval in the Electron
// main process. Every EOS callback in this addon therefore fires synchronously
// inside platform.tick(), on the JS main thread, which is what lets the rest of
// the addon call into JS directly. See docs/design.md section 6.

#include <string>

#include "callbacks.h"
#include "eosjs.h"

namespace eosjs {
namespace platform {
namespace {

std::string RequiredString(const Napi::Object& options, const char* key,
                           bool* ok) {
  Napi::Env env = options.Env();
  if (!options.Has(key) || !options.Get(key).IsString()) {
    Napi::TypeError::New(
        env, std::string("eos.init: options.") + key + " must be a string")
        .ThrowAsJavaScriptException();
    *ok = false;
    return std::string();
  }
  return options.Get(key).As<Napi::String>().Utf8Value();
}

std::string OptionalString(const Napi::Object& options, const char* key,
                           const char* fallback) {
  if (options.Has(key) && options.Get(key).IsString()) {
    return options.Get(key).As<Napi::String>().Utf8Value();
  }
  return std::string(fallback);
}

bool OptionalBool(const Napi::Object& options, const char* key, bool fallback) {
  if (options.Has(key) && options.Get(key).IsBoolean()) {
    return options.Get(key).As<Napi::Boolean>().Value();
  }
  return fallback;
}

void EOS_CALL LogCallback(const EOS_LogMessage* message) {
  if (message == nullptr) return;
  State& state = GetState();
  if (state.emitter.IsEmpty()) return;

  Napi::Env env = state.emitter.Env();
  Napi::HandleScope scope(env);
  Napi::Object payload = Napi::Object::New(env);
  payload.Set("category", Napi::String::New(
                              env, message->Category ? message->Category : ""));
  payload.Set("message",
              Napi::String::New(env, message->Message ? message->Message : ""));
  payload.Set("level",
              Napi::Number::New(env, static_cast<int>(message->Level)));
  EmitEvent(env, "log", payload);
}

Napi::Value Init(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  State& state = GetState();

  if (state.shut_down) {
    Napi::Error::New(env,
                     "EOS has already been shut down in this process. The SDK "
                     "cannot be re-initialised; restart the process.")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (state.platform != nullptr) {
    Napi::Error::New(env, "EOS is already initialised in this process.")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "eos.init(options): options must be an object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object options = info[0].As<Napi::Object>();
  bool ok = true;
  const std::string product_id = RequiredString(options, "productId", &ok);
  const std::string sandbox_id = RequiredString(options, "sandboxId", &ok);
  const std::string deployment_id =
      RequiredString(options, "deploymentId", &ok);
  const std::string client_id = RequiredString(options, "clientId", &ok);
  const std::string client_secret =
      RequiredString(options, "clientSecret", &ok);
  const std::string encryption_key =
      RequiredString(options, "encryptionKey", &ok);
  if (!ok) return env.Undefined();

  // The SDK rejects a malformed key with a generic result; check it here, where
  // the message can be useful.
  if (encryption_key.size() != 64) {
    Napi::TypeError::New(env,
                         "eos.init: encryptionKey must be exactly 64 hex "
                         "characters (256 bits)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  const std::string product_name =
      OptionalString(options, "productName", "node-eos-sdk");
  const std::string product_version =
      OptionalString(options, "productVersion", "1.0");
  const std::string cache_directory =
      OptionalString(options, "cacheDirectory", "");
  const bool is_server = OptionalBool(options, "isServer", false);
  state.debug_logging = OptionalBool(options, "debug", false);

  if (!state.initialized) {
    EOS_InitializeOptions init_options = {};
    init_options.ApiVersion = EOS_INITIALIZE_API_LATEST;
    init_options.AllocateMemoryFunction = nullptr;
    init_options.ReallocateMemoryFunction = nullptr;
    init_options.ReleaseMemoryFunction = nullptr;
    init_options.ProductName = product_name.c_str();
    init_options.ProductVersion = product_version.c_str();
    init_options.Reserved = nullptr;
    init_options.SystemInitializeOptions = nullptr;
    init_options.OverrideThreadAffinity = nullptr;

    const EOS_EResult init_result = EOS_Initialize(&init_options);
    if (init_result != EOS_EResult::EOS_Success &&
        init_result != EOS_EResult::EOS_AlreadyConfigured) {
      MakeError(env, "EOS_Initialize", init_result).ThrowAsJavaScriptException();
      return env.Undefined();
    }
    state.initialized = true;
  }

  // Route SDK logging before Platform_Create so that a failing create explains
  // itself. The SDK is talkative; index.js gates delivery behind `debug`.
  EOS_Logging_SetCallback(&LogCallback);
  EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES,
                          state.debug_logging
                              ? EOS_ELogLevel::EOS_LOG_Verbose
                              : EOS_ELogLevel::EOS_LOG_Warning);

  EOS_Platform_Options platform_options = {};
  platform_options.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
  platform_options.Reserved = nullptr;
  platform_options.ProductId = product_id.c_str();
  platform_options.SandboxId = sandbox_id.c_str();
  platform_options.DeploymentId = deployment_id.c_str();
  platform_options.ClientCredentials.ClientId = client_id.c_str();
  platform_options.ClientCredentials.ClientSecret = client_secret.c_str();
  platform_options.EncryptionKey = encryption_key.c_str();
  platform_options.bIsServer = is_server ? EOS_TRUE : EOS_FALSE;
  platform_options.OverrideCountryCode = nullptr;
  platform_options.OverrideLocaleCode = nullptr;
  platform_options.CacheDirectory =
      cache_directory.empty() ? nullptr : cache_directory.c_str();
  platform_options.Flags =
      options.Has("flags") && options.Get("flags").IsNumber()
          ? options.Get("flags").As<Napi::Number>().Uint32Value()
          : 0;
  // 0 means "no budget": finish all queued work each tick. At 20 Hz with this
  // workload there is nothing to budget against.
  platform_options.TickBudgetInMilliseconds = 0;
  platform_options.RTCOptions = nullptr;
  platform_options.IntegratedPlatformOptionsContainerHandle = nullptr;
  platform_options.SystemSpecificOptions = nullptr;
  platform_options.TaskNetworkTimeoutSeconds = nullptr;

  state.platform = EOS_Platform_Create(&platform_options);
  if (state.platform == nullptr) {
    Napi::Error::New(
        env,
        "EOS_Platform_Create returned null. The usual causes are a malformed "
        "credential value or a client policy that does not permit this "
        "product; enable { debug: true } and read the SDK log.")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return env.Undefined();
}

// Called on the 50 ms interval owned by index.js. Everything asynchronous in
// EOS happens inside this call.
Napi::Value Tick(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  State& state = GetState();
  if (state.platform == nullptr) return env.Undefined();

  EOS_Platform_Tick(state.platform);

  // Drain P2P on the same tick. Several turns of packets can arrive between
  // ticks; stopping after one leaves a backlog that only grows.
  p2p::DrainPackets(env);
  return env.Undefined();
}

Napi::Value Shutdown(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  State& state = GetState();
  if (state.platform == nullptr) return env.Undefined();

  lobby::ClearNotifications();
  p2p::ClearNotifications();

  EOS_Platform_Release(state.platform);
  state.platform = nullptr;

  EOS_Logging_SetCallback(nullptr);
  EOS_Shutdown();
  state.shut_down = true;

  if (!state.emitter.IsEmpty()) state.emitter.Reset();
  return env.Undefined();
}

Napi::Value IsInitialized(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), GetState().platform != nullptr);
}

Napi::Value SetLogLevel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "setLogLevel(level): level must be a number")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  EOS_Logging_SetLogLevel(
      EOS_ELogCategory::EOS_LC_ALL_CATEGORIES,
      static_cast<EOS_ELogLevel>(info[0].As<Napi::Number>().Int32Value()));
  return env.Undefined();
}

}  // namespace

void Init(Napi::Env env, Napi::Object exports) {
  Napi::Object ns = Napi::Object::New(env);
  ns.Set("init", Napi::Function::New(env, Init));
  ns.Set("tick", Napi::Function::New(env, Tick));
  ns.Set("shutdown", Napi::Function::New(env, Shutdown));
  ns.Set("isInitialized", Napi::Function::New(env, IsInitialized));
  ns.Set("setLogLevel", Napi::Function::New(env, SetLogLevel));
  exports.Set("platform", ns);
}

}  // namespace platform
}  // namespace eosjs
