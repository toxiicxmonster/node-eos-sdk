// M3 -- Lobby.
//
// The JS surface here deliberately mirrors the shape of steamworks.js's
// matchmaking.Lobby (getMembers, getOwner, setData, getFullData, sendInvite),
// so a game whose lobby UI is already written against that shape does not care
// which backend is underneath. See docs/design.md section 3, M3.
//
// Lobby attributes carry the match setup -- map, seed, mode, and each member's
// faction, team and ready state. The host owns the authoritative copy; members
// set only their own member attributes.

#include <string>
#include <vector>

#include "callbacks.h"
#include "eosjs.h"

namespace eosjs {
namespace lobby {
namespace {

EOS_NotificationId g_update_notification = EOS_INVALID_NOTIFICATIONID;
EOS_NotificationId g_member_update_notification = EOS_INVALID_NOTIFICATIONID;
EOS_NotificationId g_member_status_notification = EOS_INVALID_NOTIFICATIONID;
EOS_NotificationId g_invite_accepted_notification = EOS_INVALID_NOTIFICATIONID;

EOS_HLobby Handle() {
  return EOS_Platform_GetLobbyInterface(GetState().platform);
}

std::string StringField(const Napi::Object& options, const char* key,
                        const std::string& fallback = std::string()) {
  if (options.Has(key) && options.Get(key).IsString()) {
    return options.Get(key).As<Napi::String>().Utf8Value();
  }
  return fallback;
}

// Releases a lobby details handle however the scope exits. Every read path
// below copies a handle, and forgetting to release one leaks SDK memory for the
// life of the process.
class ScopedLobbyDetails {
 public:
  explicit ScopedLobbyDetails(EOS_HLobbyDetails handle) : handle_(handle) {}
  ~ScopedLobbyDetails() {
    if (handle_ != nullptr) EOS_LobbyDetails_Release(handle_);
  }
  ScopedLobbyDetails(const ScopedLobbyDetails&) = delete;
  ScopedLobbyDetails& operator=(const ScopedLobbyDetails&) = delete;

  EOS_HLobbyDetails get() const { return handle_; }
  explicit operator bool() const { return handle_ != nullptr; }

 private:
  EOS_HLobbyDetails handle_;
};

EOS_HLobbyDetails CopyDetails(const Napi::Env& env, const std::string& lobby_id,
                              const std::string& local_user_id) {
  EOS_Lobby_CopyLobbyDetailsHandleOptions options = {};
  options.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLE_API_LATEST;
  options.LobbyId = lobby_id.c_str();
  options.LocalUserId = PuidFromString(local_user_id);

  EOS_HLobbyDetails details = nullptr;
  const EOS_EResult result =
      EOS_Lobby_CopyLobbyDetailsHandle(Handle(), &options, &details);
  if (result != EOS_EResult::EOS_Success) {
    MakeError(env, "EOS_Lobby_CopyLobbyDetailsHandle", result)
        .ThrowAsJavaScriptException();
    return nullptr;
  }
  return details;
}

EOS_ELobbyPermissionLevel ParsePermissionLevel(const std::string& name) {
  if (name == "joinViaPresence") {
    return EOS_ELobbyPermissionLevel::EOS_LPL_JOINVIAPRESENCE;
  }
  if (name == "inviteOnly") {
    return EOS_ELobbyPermissionLevel::EOS_LPL_INVITEONLY;
  }
  return EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED;
}

const char* PermissionLevelName(EOS_ELobbyPermissionLevel level) {
  switch (level) {
    case EOS_ELobbyPermissionLevel::EOS_LPL_JOINVIAPRESENCE:
      return "joinViaPresence";
    case EOS_ELobbyPermissionLevel::EOS_LPL_INVITEONLY:
      return "inviteOnly";
    default:
      return "publicAdvertised";
  }
}

// Converts one EOS attribute into its JS value. Ownership of `attribute` stays
// with the caller.
Napi::Value AttributeToJs(const Napi::Env& env,
                          const EOS_Lobby_AttributeData* data) {
  switch (data->ValueType) {
    case EOS_EAttributeType::EOS_AT_BOOLEAN:
      return Napi::Boolean::New(env, data->Value.AsBool == EOS_TRUE);
    case EOS_EAttributeType::EOS_AT_INT64:
      return Napi::Number::New(env, static_cast<double>(data->Value.AsInt64));
    case EOS_EAttributeType::EOS_AT_DOUBLE:
      return Napi::Number::New(env, data->Value.AsDouble);
    default:
      return Napi::String::New(
          env, data->Value.AsUtf8 != nullptr ? data->Value.AsUtf8 : "");
  }
}

// Fills an EOS_Lobby_AttributeData from a JS value, using `storage` to keep the
// backing strings alive until the SDK has copied them.
bool JsToAttribute(const Napi::Env& env, const std::string& key,
                   const Napi::Value& value, EOS_Lobby_AttributeData* out,
                   std::vector<std::string>* storage) {
  storage->push_back(key);
  *out = {};
  out->ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
  out->Key = storage->back().c_str();

  if (value.IsBoolean()) {
    out->ValueType = EOS_EAttributeType::EOS_AT_BOOLEAN;
    out->Value.AsBool = value.As<Napi::Boolean>().Value() ? EOS_TRUE : EOS_FALSE;
    return true;
  }
  if (value.IsNumber()) {
    const double number = value.As<Napi::Number>().DoubleValue();
    // Integers travel as INT64 so that comparisons in lobby search behave the
    // way callers expect; anything fractional stays a double.
    if (number == static_cast<double>(static_cast<int64_t>(number))) {
      out->ValueType = EOS_EAttributeType::EOS_AT_INT64;
      out->Value.AsInt64 = static_cast<int64_t>(number);
    } else {
      out->ValueType = EOS_EAttributeType::EOS_AT_DOUBLE;
      out->Value.AsDouble = number;
    }
    return true;
  }
  if (value.IsString()) {
    storage->push_back(value.As<Napi::String>().Utf8Value());
    out->ValueType = EOS_EAttributeType::EOS_AT_STRING;
    out->Value.AsUtf8 = storage->back().c_str();
    return true;
  }

  Napi::TypeError::New(env, "lobby attribute '" + key +
                                "' must be a string, number or boolean")
      .ThrowAsJavaScriptException();
  return false;
}

// ---------------------------------------------------------------------------
// Create / join / leave
// ---------------------------------------------------------------------------

Napi::Value CreateLobby(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "lobby.create(options): options must be an object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object options = info[0].As<Napi::Object>();

  const std::string local_user_id = StringField(options, "localUserId");
  const std::string bucket_id = StringField(options, "bucketId", "default");
  const std::string permission =
      StringField(options, "permissionLevel", "inviteOnly");

  EOS_Lobby_CreateLobbyOptions create_options = {};
  create_options.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
  create_options.LocalUserId = PuidFromString(local_user_id);
  create_options.MaxLobbyMembers =
      options.Has("maxMembers") && options.Get("maxMembers").IsNumber()
          ? options.Get("maxMembers").As<Napi::Number>().Uint32Value()
          : 4;
  create_options.PermissionLevel = ParsePermissionLevel(permission);
  create_options.bPresenceEnabled = EOS_FALSE;
  create_options.bAllowInvites = EOS_TRUE;
  create_options.BucketId = bucket_id.c_str();
  create_options.bDisableHostMigration = EOS_TRUE;
  create_options.bEnableRTCRoom = EOS_FALSE;
  create_options.LocalRTCOptions = nullptr;
  create_options.bEnableJoinById = EOS_TRUE;
  create_options.bRejoinAfterKickRequiresInvite = EOS_FALSE;

  using Op = AsyncOp<EOS_Lobby_CreateLobbyCallbackInfo>;
  auto* op = Op::Start(
      env, "EOS_Lobby_CreateLobby",
      [](Napi::Env env, const EOS_Lobby_CreateLobbyCallbackInfo* data) {
        return Napi::Value(Napi::String::New(
            env, data->LobbyId != nullptr ? data->LobbyId : ""));
      });

  EOS_Lobby_CreateLobby(Handle(), &create_options, op->ClientData(),
                        &Op::OnComplete);
  return op->Promise();
}

// join({ localUserId, lobbyId }) -> Promise<string lobbyId>
//
// EOS has no "join by id" primitive: you search for the id, take the single
// result's details handle and join that. Three SDK calls behind one promise.
Napi::Value JoinLobby(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "lobby.join(options): options must be an object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object options = info[0].As<Napi::Object>();

  const std::string local_user_id = StringField(options, "localUserId");
  const std::string lobby_id = StringField(options, "lobbyId");
  if (lobby_id.empty()) {
    Napi::TypeError::New(env, "lobby.join: options.lobbyId is required")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  EOS_HLobby handle = Handle();

  EOS_Lobby_CreateLobbySearchOptions search_options = {};
  search_options.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST;
  search_options.MaxResults = 1;

  EOS_HLobbySearch search = nullptr;
  EOS_EResult result =
      EOS_Lobby_CreateLobbySearch(handle, &search_options, &search);
  if (result != EOS_EResult::EOS_Success) {
    return RejectedPromise(env, "EOS_Lobby_CreateLobbySearch", result);
  }

  EOS_LobbySearch_SetLobbyIdOptions id_options = {};
  id_options.ApiVersion = EOS_LOBBYSEARCH_SETLOBBYID_API_LATEST;
  id_options.LobbyId = lobby_id.c_str();
  result = EOS_LobbySearch_SetLobbyId(search, &id_options);
  if (result != EOS_EResult::EOS_Success) {
    EOS_LobbySearch_Release(search);
    return RejectedPromise(env, "EOS_LobbySearch_SetLobbyId", result);
  }

  EOS_ProductUserId local_user = PuidFromString(local_user_id);

  using FindOp = AsyncOp<EOS_LobbySearch_FindCallbackInfo>;
  auto* find_op = FindOp::StartWithSettler(
      env, "EOS_LobbySearch_Find",
      [search, local_user, lobby_id](Napi::Env env,
                                     const EOS_LobbySearch_FindCallbackInfo* data,
                                     Napi::Promise::Deferred& deferred) {
        // The search handle is owned by this lambda from here on, whichever way
        // the branch goes.
        if (data->ResultCode != EOS_EResult::EOS_Success) {
          EOS_LobbySearch_Release(search);
          deferred.Reject(
              MakeError(env, "EOS_LobbySearch_Find", data->ResultCode).Value());
          return;
        }

        EOS_LobbySearch_CopySearchResultByIndexOptions copy_options = {};
        copy_options.ApiVersion =
            EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST;
        copy_options.LobbyIndex = 0;

        EOS_HLobbyDetails details = nullptr;
        const EOS_EResult copy_result =
            EOS_LobbySearch_CopySearchResultByIndex(search, &copy_options,
                                                    &details);
        EOS_LobbySearch_Release(search);

        if (copy_result != EOS_EResult::EOS_Success || details == nullptr) {
          deferred.Reject(
              MakeError(env, "EOS_LobbySearch_CopySearchResultByIndex",
                        copy_result == EOS_EResult::EOS_Success
                            ? EOS_EResult::EOS_NotFound
                            : copy_result)
                  .Value());
          return;
        }

        EOS_Lobby_JoinLobbyOptions join_options = {};
        join_options.ApiVersion = EOS_LOBBY_JOINLOBBY_API_LATEST;
        join_options.LobbyDetailsHandle = details;
        join_options.LocalUserId = local_user;
        join_options.bPresenceEnabled = EOS_FALSE;
        join_options.LocalRTCOptions = nullptr;
        join_options.bCrossplayOptOut = EOS_FALSE;

        using JoinOp = AsyncOp<EOS_Lobby_JoinLobbyCallbackInfo>;
        auto* join_op = JoinOp::Adopt(
            deferred, "EOS_Lobby_JoinLobby",
            [details](Napi::Env env,
                      const EOS_Lobby_JoinLobbyCallbackInfo* join_data) {
              EOS_LobbyDetails_Release(details);
              return Napi::Value(Napi::String::New(
                  env, join_data->LobbyId != nullptr ? join_data->LobbyId : ""));
            });

        EOS_Lobby_JoinLobby(Handle(), &join_options, join_op->ClientData(),
                            &JoinOp::OnComplete);
      });

  EOS_LobbySearch_FindOptions find_options = {};
  find_options.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST;
  find_options.LocalUserId = local_user;
  EOS_LobbySearch_Find(search, &find_options, find_op->ClientData(),
                       &FindOp::OnComplete);
  return find_op->Promise();
}

Napi::Value LeaveLobby(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "lobby.leave(options): options must be an object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object options = info[0].As<Napi::Object>();
  const std::string lobby_id = StringField(options, "lobbyId");

  EOS_Lobby_LeaveLobbyOptions leave_options = {};
  leave_options.ApiVersion = EOS_LOBBY_LEAVELOBBY_API_LATEST;
  leave_options.LocalUserId = PuidFromString(StringField(options, "localUserId"));
  leave_options.LobbyId = lobby_id.c_str();

  using Op = AsyncOp<EOS_Lobby_LeaveLobbyCallbackInfo>;
  auto* op = Op::Start(env, "EOS_Lobby_LeaveLobby");
  EOS_Lobby_LeaveLobby(Handle(), &leave_options, op->ClientData(),
                       &Op::OnComplete);
  return op->Promise();
}

// ---------------------------------------------------------------------------
// Reads -- all synchronous, all against a copied details handle
// ---------------------------------------------------------------------------

Napi::Value GetMembers(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();
  Napi::Object options = info[0].As<Napi::Object>();

  ScopedLobbyDetails details(CopyDetails(env, StringField(options, "lobbyId"),
                                         StringField(options, "localUserId")));
  if (!details) return env.Undefined();

  EOS_LobbyDetails_GetMemberCountOptions count_options = {};
  count_options.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERCOUNT_API_LATEST;
  const uint32_t count =
      EOS_LobbyDetails_GetMemberCount(details.get(), &count_options);

  Napi::Array members = Napi::Array::New(env, count);
  for (uint32_t i = 0; i < count; ++i) {
    EOS_LobbyDetails_GetMemberByIndexOptions member_options = {};
    member_options.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERBYINDEX_API_LATEST;
    member_options.MemberIndex = i;
    members.Set(i, Napi::String::New(
                       env, PuidToString(EOS_LobbyDetails_GetMemberByIndex(
                                details.get(), &member_options))));
  }
  return members;
}

Napi::Value GetInfo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();
  Napi::Object options = info[0].As<Napi::Object>();

  ScopedLobbyDetails details(CopyDetails(env, StringField(options, "lobbyId"),
                                         StringField(options, "localUserId")));
  if (!details) return env.Undefined();

  EOS_LobbyDetails_CopyInfoOptions copy_options = {};
  copy_options.ApiVersion = EOS_LOBBYDETAILS_COPYINFO_API_LATEST;

  EOS_LobbyDetails_Info* lobby_info = nullptr;
  const EOS_EResult result =
      EOS_LobbyDetails_CopyInfo(details.get(), &copy_options, &lobby_info);
  if (result != EOS_EResult::EOS_Success || lobby_info == nullptr) {
    MakeError(env, "EOS_LobbyDetails_CopyInfo", result)
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result_object = Napi::Object::New(env);
  result_object.Set("lobbyId", Napi::String::New(env, lobby_info->LobbyId
                                                          ? lobby_info->LobbyId
                                                          : ""));
  result_object.Set("ownerUserId", Napi::String::New(env, PuidToString(
                                       lobby_info->LobbyOwnerUserId)));
  result_object.Set("maxMembers", Napi::Number::New(env, lobby_info->MaxMembers));
  result_object.Set("availableSlots",
                    Napi::Number::New(env, lobby_info->AvailableSlots));
  result_object.Set(
      "permissionLevel",
      Napi::String::New(env, PermissionLevelName(lobby_info->PermissionLevel)));
  result_object.Set("allowInvites",
                    Napi::Boolean::New(env, lobby_info->bAllowInvites == EOS_TRUE));
  result_object.Set("bucketId", Napi::String::New(env, lobby_info->BucketId
                                                           ? lobby_info->BucketId
                                                           : ""));
  EOS_LobbyDetails_Info_Release(lobby_info);
  return result_object;
}

Napi::Value GetAttributes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();
  Napi::Object options = info[0].As<Napi::Object>();

  ScopedLobbyDetails details(CopyDetails(env, StringField(options, "lobbyId"),
                                         StringField(options, "localUserId")));
  if (!details) return env.Undefined();

  EOS_LobbyDetails_GetAttributeCountOptions count_options = {};
  count_options.ApiVersion = EOS_LOBBYDETAILS_GETATTRIBUTECOUNT_API_LATEST;
  const uint32_t count =
      EOS_LobbyDetails_GetAttributeCount(details.get(), &count_options);

  Napi::Object attributes = Napi::Object::New(env);
  for (uint32_t i = 0; i < count; ++i) {
    EOS_LobbyDetails_CopyAttributeByIndexOptions copy_options = {};
    copy_options.ApiVersion = EOS_LOBBYDETAILS_COPYATTRIBUTEBYINDEX_API_LATEST;
    copy_options.AttrIndex = i;

    EOS_Lobby_Attribute* attribute = nullptr;
    if (EOS_LobbyDetails_CopyAttributeByIndex(details.get(), &copy_options,
                                              &attribute) !=
            EOS_EResult::EOS_Success ||
        attribute == nullptr || attribute->Data == nullptr) {
      continue;
    }
    attributes.Set(attribute->Data->Key, AttributeToJs(env, attribute->Data));
    EOS_Lobby_Attribute_Release(attribute);
  }
  return attributes;
}

Napi::Value GetMemberAttributes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();
  Napi::Object options = info[0].As<Napi::Object>();

  ScopedLobbyDetails details(CopyDetails(env, StringField(options, "lobbyId"),
                                         StringField(options, "localUserId")));
  if (!details) return env.Undefined();

  EOS_ProductUserId target =
      PuidFromString(StringField(options, "targetUserId"));

  EOS_LobbyDetails_GetMemberAttributeCountOptions count_options = {};
  count_options.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERATTRIBUTECOUNT_API_LATEST;
  count_options.TargetUserId = target;
  const uint32_t count =
      EOS_LobbyDetails_GetMemberAttributeCount(details.get(), &count_options);

  Napi::Object attributes = Napi::Object::New(env);
  for (uint32_t i = 0; i < count; ++i) {
    EOS_LobbyDetails_CopyMemberAttributeByIndexOptions copy_options = {};
    copy_options.ApiVersion =
        EOS_LOBBYDETAILS_COPYMEMBERATTRIBUTEBYINDEX_API_LATEST;
    copy_options.TargetUserId = target;
    copy_options.AttrIndex = i;

    EOS_Lobby_Attribute* attribute = nullptr;
    if (EOS_LobbyDetails_CopyMemberAttributeByIndex(details.get(), &copy_options,
                                                    &attribute) !=
            EOS_EResult::EOS_Success ||
        attribute == nullptr || attribute->Data == nullptr) {
      continue;
    }
    attributes.Set(attribute->Data->Key, AttributeToJs(env, attribute->Data));
    EOS_Lobby_Attribute_Release(attribute);
  }
  return attributes;
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

// Shared by setAttributes (host, lobby-wide) and setMemberAttributes (any
// member, their own row). The only difference is which Add*Attribute call the
// modification receives.
Napi::Value ApplyAttributes(const Napi::CallbackInfo& info, bool member_scope) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "options must be an object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object options = info[0].As<Napi::Object>();
  const std::string lobby_id = StringField(options, "lobbyId");

  if (!options.Has("attributes") || !options.Get("attributes").IsObject()) {
    Napi::TypeError::New(env, "options.attributes must be an object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object attributes = options.Get("attributes").As<Napi::Object>();

  EOS_Lobby_UpdateLobbyModificationOptions modification_options = {};
  modification_options.ApiVersion =
      EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
  modification_options.LocalUserId =
      PuidFromString(StringField(options, "localUserId"));
  modification_options.LobbyId = lobby_id.c_str();

  EOS_HLobbyModification modification = nullptr;
  EOS_EResult result = EOS_Lobby_UpdateLobbyModification(
      Handle(), &modification_options, &modification);
  if (result != EOS_EResult::EOS_Success) {
    return RejectedPromise(env, "EOS_Lobby_UpdateLobbyModification", result);
  }

  // Backing store for every key and string value handed to the SDK; it must
  // outlive the Add*Attribute calls below.
  std::vector<std::string> storage;
  storage.reserve(static_cast<size_t>(attributes.GetPropertyNames().Length()) *
                  2);

  Napi::Array keys = attributes.GetPropertyNames();
  for (uint32_t i = 0; i < keys.Length(); ++i) {
    const std::string key =
        keys.Get(i).As<Napi::String>().Utf8Value();
    EOS_Lobby_AttributeData data;
    if (!JsToAttribute(env, key, attributes.Get(key), &data, &storage)) {
      EOS_LobbyModification_Release(modification);
      return env.Undefined();
    }

    if (member_scope) {
      EOS_LobbyModification_AddMemberAttributeOptions add_options = {};
      add_options.ApiVersion =
          EOS_LOBBYMODIFICATION_ADDMEMBERATTRIBUTE_API_LATEST;
      add_options.Attribute = &data;
      add_options.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
      result = EOS_LobbyModification_AddMemberAttribute(modification,
                                                        &add_options);
    } else {
      EOS_LobbyModification_AddAttributeOptions add_options = {};
      add_options.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
      add_options.Attribute = &data;
      add_options.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
      result = EOS_LobbyModification_AddAttribute(modification, &add_options);
    }

    if (result != EOS_EResult::EOS_Success) {
      EOS_LobbyModification_Release(modification);
      return RejectedPromise(env, "EOS_LobbyModification_AddAttribute", result);
    }
  }

  EOS_Lobby_UpdateLobbyOptions update_options = {};
  update_options.ApiVersion = EOS_LOBBY_UPDATELOBBY_API_LATEST;
  update_options.LobbyModificationHandle = modification;

  using Op = AsyncOp<EOS_Lobby_UpdateLobbyCallbackInfo>;
  auto* op = Op::Start(env, "EOS_Lobby_UpdateLobby");
  EOS_Lobby_UpdateLobby(Handle(), &update_options, op->ClientData(),
                        &Op::OnComplete);

  // UpdateLobby has taken its own reference; releasing here is correct and
  // avoids leaking the modification on every attribute write.
  EOS_LobbyModification_Release(modification);
  return op->Promise();
}

Napi::Value SetAttributes(const Napi::CallbackInfo& info) {
  return ApplyAttributes(info, false);
}

Napi::Value SetMemberAttributes(const Napi::CallbackInfo& info) {
  return ApplyAttributes(info, true);
}

Napi::Value SendInvite(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();
  Napi::Object options = info[0].As<Napi::Object>();
  const std::string lobby_id = StringField(options, "lobbyId");

  EOS_Lobby_SendInviteOptions invite_options = {};
  invite_options.ApiVersion = EOS_LOBBY_SENDINVITE_API_LATEST;
  invite_options.LobbyId = lobby_id.c_str();
  invite_options.LocalUserId =
      PuidFromString(StringField(options, "localUserId"));
  invite_options.TargetUserId =
      PuidFromString(StringField(options, "targetUserId"));

  using Op = AsyncOp<EOS_Lobby_SendInviteCallbackInfo>;
  auto* op = Op::Start(env, "EOS_Lobby_SendInvite");
  EOS_Lobby_SendInvite(Handle(), &invite_options, op->ClientData(),
                       &Op::OnComplete);
  return op->Promise();
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

void EOS_CALL OnLobbyUpdate(const EOS_Lobby_LobbyUpdateReceivedCallbackInfo* data) {
  if (data == nullptr) return;
  State& state = GetState();
  if (state.emitter.IsEmpty()) return;
  Napi::Env env = state.emitter.Env();
  Napi::HandleScope scope(env);

  Napi::Object payload = Napi::Object::New(env);
  payload.Set("lobbyId",
              Napi::String::New(env, data->LobbyId ? data->LobbyId : ""));
  EmitEvent(env, "lobby:updated", payload);
}

void EOS_CALL OnMemberUpdate(
    const EOS_Lobby_LobbyMemberUpdateReceivedCallbackInfo* data) {
  if (data == nullptr) return;
  State& state = GetState();
  if (state.emitter.IsEmpty()) return;
  Napi::Env env = state.emitter.Env();
  Napi::HandleScope scope(env);

  Napi::Object payload = Napi::Object::New(env);
  payload.Set("lobbyId",
              Napi::String::New(env, data->LobbyId ? data->LobbyId : ""));
  payload.Set("targetUserId",
              Napi::String::New(env, PuidToString(data->TargetUserId)));
  EmitEvent(env, "lobby:member-updated", payload);
}

void EOS_CALL OnMemberStatus(
    const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo* data) {
  if (data == nullptr) return;
  State& state = GetState();
  if (state.emitter.IsEmpty()) return;
  Napi::Env env = state.emitter.Env();
  Napi::HandleScope scope(env);

  Napi::Object payload = Napi::Object::New(env);
  payload.Set("lobbyId",
              Napi::String::New(env, data->LobbyId ? data->LobbyId : ""));
  payload.Set("targetUserId",
              Napi::String::New(env, PuidToString(data->TargetUserId)));
  payload.Set("status",
              Napi::Number::New(env, static_cast<int>(data->CurrentStatus)));
  EmitEvent(env, "lobby:member-status", payload);
}

void EOS_CALL OnInviteAccepted(
    const EOS_Lobby_LobbyInviteAcceptedCallbackInfo* data) {
  if (data == nullptr) return;
  State& state = GetState();
  if (state.emitter.IsEmpty()) return;
  Napi::Env env = state.emitter.Env();
  Napi::HandleScope scope(env);

  Napi::Object payload = Napi::Object::New(env);
  payload.Set("inviteId",
              Napi::String::New(env, data->InviteId ? data->InviteId : ""));
  payload.Set("lobbyId",
              Napi::String::New(env, data->LobbyId ? data->LobbyId : ""));
  payload.Set("localUserId",
              Napi::String::New(env, PuidToString(data->LocalUserId)));
  payload.Set("targetUserId",
              Napi::String::New(env, PuidToString(data->TargetUserId)));
  EmitEvent(env, "lobby:invite-accepted", payload);
}

Napi::Value WatchLobby(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();
  if (g_update_notification != EOS_INVALID_NOTIFICATIONID) return env.Undefined();

  EOS_HLobby handle = Handle();

  EOS_Lobby_AddNotifyLobbyUpdateReceivedOptions update_options = {};
  update_options.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYUPDATERECEIVED_API_LATEST;
  g_update_notification = EOS_Lobby_AddNotifyLobbyUpdateReceived(
      handle, &update_options, nullptr, &OnLobbyUpdate);

  EOS_Lobby_AddNotifyLobbyMemberUpdateReceivedOptions member_update_options = {};
  member_update_options.ApiVersion =
      EOS_LOBBY_ADDNOTIFYLOBBYMEMBERUPDATERECEIVED_API_LATEST;
  g_member_update_notification = EOS_Lobby_AddNotifyLobbyMemberUpdateReceived(
      handle, &member_update_options, nullptr, &OnMemberUpdate);

  EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions member_status_options = {};
  member_status_options.ApiVersion =
      EOS_LOBBY_ADDNOTIFYLOBBYMEMBERSTATUSRECEIVED_API_LATEST;
  g_member_status_notification = EOS_Lobby_AddNotifyLobbyMemberStatusReceived(
      handle, &member_status_options, nullptr, &OnMemberStatus);

  EOS_Lobby_AddNotifyLobbyInviteAcceptedOptions invite_options = {};
  invite_options.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYINVITEACCEPTED_API_LATEST;
  g_invite_accepted_notification = EOS_Lobby_AddNotifyLobbyInviteAccepted(
      handle, &invite_options, nullptr, &OnInviteAccepted);

  return env.Undefined();
}

}  // namespace

void ClearNotifications() {
  State& state = GetState();
  if (state.platform == nullptr) return;
  EOS_HLobby handle = Handle();

  if (g_update_notification != EOS_INVALID_NOTIFICATIONID) {
    EOS_Lobby_RemoveNotifyLobbyUpdateReceived(handle, g_update_notification);
    g_update_notification = EOS_INVALID_NOTIFICATIONID;
  }
  if (g_member_update_notification != EOS_INVALID_NOTIFICATIONID) {
    EOS_Lobby_RemoveNotifyLobbyMemberUpdateReceived(
        handle, g_member_update_notification);
    g_member_update_notification = EOS_INVALID_NOTIFICATIONID;
  }
  if (g_member_status_notification != EOS_INVALID_NOTIFICATIONID) {
    EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived(
        handle, g_member_status_notification);
    g_member_status_notification = EOS_INVALID_NOTIFICATIONID;
  }
  if (g_invite_accepted_notification != EOS_INVALID_NOTIFICATIONID) {
    EOS_Lobby_RemoveNotifyLobbyInviteAccepted(handle,
                                              g_invite_accepted_notification);
    g_invite_accepted_notification = EOS_INVALID_NOTIFICATIONID;
  }
}

void Init(Napi::Env env, Napi::Object exports) {
  Napi::Object ns = Napi::Object::New(env);
  ns.Set("create", Napi::Function::New(env, CreateLobby));
  ns.Set("join", Napi::Function::New(env, JoinLobby));
  ns.Set("leave", Napi::Function::New(env, LeaveLobby));
  ns.Set("getMembers", Napi::Function::New(env, GetMembers));
  ns.Set("getInfo", Napi::Function::New(env, GetInfo));
  ns.Set("getAttributes", Napi::Function::New(env, GetAttributes));
  ns.Set("getMemberAttributes", Napi::Function::New(env, GetMemberAttributes));
  ns.Set("setAttributes", Napi::Function::New(env, SetAttributes));
  ns.Set("setMemberAttributes", Napi::Function::New(env, SetMemberAttributes));
  ns.Set("sendInvite", Napi::Function::New(env, SendInvite));
  ns.Set("watchLobby", Napi::Function::New(env, WatchLobby));
  exports.Set("lobby", ns);
}

}  // namespace lobby
}  // namespace eosjs
