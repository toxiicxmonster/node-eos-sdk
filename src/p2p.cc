// M4 -- P2P.
//
// A "socket" in EOS is just a named channel; both peers must use the same name.
// Packets are drained on the same 50 ms tick as the platform (see
// platform.cc), in a loop until the queue reports empty: several turns of
// packets can arrive between ticks, and stopping after one leaves a backlog
// that only grows.
//
// Reliability defaults to EOS_PR_ReliableOrdered. For deterministic lockstep,
// a lost or reordered turn is a permanently stalled match, so head-of-line
// blocking is the right trade -- a pause is recoverable, divergence is not.

#include <cstdint>
#include <string>
#include <vector>

#include "callbacks.h"
#include "eosjs.h"

namespace eosjs {
namespace p2p {
namespace {

EOS_ProductUserId g_local_user = nullptr;
std::string g_socket_name;
bool g_auto_accept = true;
uint32_t g_max_packets_per_tick = 1024;
EOS_NotificationId g_request_notification = EOS_INVALID_NOTIFICATIONID;
EOS_NotificationId g_established_notification = EOS_INVALID_NOTIFICATIONID;
EOS_NotificationId g_closed_notification = EOS_INVALID_NOTIFICATIONID;

EOS_HP2P Handle() {
  return EOS_Platform_GetP2PInterface(GetState().platform);
}

void FillSocketId(EOS_P2P_SocketId* socket_id, const std::string& name) {
  *socket_id = {};
  socket_id->ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
  CopyToFixedBuffer(name, socket_id->SocketName,
                    sizeof(socket_id->SocketName));
}

std::string StringField(const Napi::Object& options, const char* key,
                        const std::string& fallback) {
  if (options.Has(key) && options.Get(key).IsString()) {
    return options.Get(key).As<Napi::String>().Utf8Value();
  }
  return fallback;
}

EOS_EPacketReliability ParseReliability(const std::string& name) {
  if (name == "unreliableUnordered") {
    return EOS_EPacketReliability::EOS_PR_UnreliableUnordered;
  }
  if (name == "reliableUnordered") {
    return EOS_EPacketReliability::EOS_PR_ReliableUnordered;
  }
  return EOS_EPacketReliability::EOS_PR_ReliableOrdered;
}

// configure({ localUserId, socketName, autoAccept?, maxPacketsPerTick? })
//
// Tells the tick loop which user to drain packets for. Until this is called,
// DrainPackets is a no-op.
Napi::Value Configure(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "p2p.configure(options): options must be an object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object options = info[0].As<Napi::Object>();

  g_local_user = PuidFromString(StringField(options, "localUserId", ""));
  g_socket_name = StringField(options, "socketName", "");
  if (options.Has("autoAccept") && options.Get("autoAccept").IsBoolean()) {
    g_auto_accept = options.Get("autoAccept").As<Napi::Boolean>().Value();
  }
  if (options.Has("maxPacketsPerTick") &&
      options.Get("maxPacketsPerTick").IsNumber()) {
    g_max_packets_per_tick =
        options.Get("maxPacketsPerTick").As<Napi::Number>().Uint32Value();
  }
  return env.Undefined();
}

// send({ remoteUserId, data, localUserId?, socketName?, channel?, reliability? })
Napi::Value Send(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "p2p.send(options): options must be an object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object options = info[0].As<Napi::Object>();

  if (!options.Has("data") || !options.Get("data").IsBuffer()) {
    Napi::TypeError::New(env, "p2p.send: options.data must be a Buffer")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Buffer<uint8_t> data = options.Get("data").As<Napi::Buffer<uint8_t>>();

  EOS_ProductUserId remote =
      PuidFromString(StringField(options, "remoteUserId", ""));
  if (remote == nullptr) {
    Napi::TypeError::New(env, "p2p.send: options.remoteUserId is required")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  EOS_P2P_SocketId socket_id;
  FillSocketId(&socket_id, StringField(options, "socketName", g_socket_name));

  EOS_P2P_SendPacketOptions send_options = {};
  send_options.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
  send_options.LocalUserId =
      options.Has("localUserId")
          ? PuidFromString(StringField(options, "localUserId", ""))
          : g_local_user;
  send_options.RemoteUserId = remote;
  send_options.SocketId = &socket_id;
  send_options.Channel = static_cast<uint8_t>(
      options.Has("channel") && options.Get("channel").IsNumber()
          ? options.Get("channel").As<Napi::Number>().Uint32Value()
          : 0);
  send_options.DataLengthBytes = static_cast<uint32_t>(data.Length());
  send_options.Data = data.Data();
  send_options.bAllowDelayedDelivery = EOS_TRUE;
  send_options.Reliability =
      ParseReliability(StringField(options, "reliability", "reliableOrdered"));
  send_options.bDisableAutoAcceptConnection = EOS_FALSE;

  const EOS_EResult result = EOS_P2P_SendPacket(Handle(), &send_options);
  if (result != EOS_EResult::EOS_Success) {
    MakeError(env, "EOS_P2P_SendPacket", result).ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

// Accept or close a connection. Both take the same shape, so they share a body.
Napi::Value ConnectionCall(const Napi::CallbackInfo& info, bool accept) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "options must be an object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object options = info[0].As<Napi::Object>();

  EOS_P2P_SocketId socket_id;
  FillSocketId(&socket_id, StringField(options, "socketName", g_socket_name));

  EOS_ProductUserId local =
      options.Has("localUserId")
          ? PuidFromString(StringField(options, "localUserId", ""))
          : g_local_user;
  EOS_ProductUserId remote =
      PuidFromString(StringField(options, "remoteUserId", ""));

  EOS_EResult result;
  if (accept) {
    EOS_P2P_AcceptConnectionOptions accept_options = {};
    accept_options.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
    accept_options.LocalUserId = local;
    accept_options.RemoteUserId = remote;
    accept_options.SocketId = &socket_id;
    result = EOS_P2P_AcceptConnection(Handle(), &accept_options);
  } else {
    EOS_P2P_CloseConnectionOptions close_options = {};
    close_options.ApiVersion = EOS_P2P_CLOSECONNECTION_API_LATEST;
    close_options.LocalUserId = local;
    close_options.RemoteUserId = remote;
    close_options.SocketId = &socket_id;
    result = EOS_P2P_CloseConnection(Handle(), &close_options);
  }

  if (result != EOS_EResult::EOS_Success) {
    MakeError(env, accept ? "EOS_P2P_AcceptConnection"
                          : "EOS_P2P_CloseConnection",
              result)
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value AcceptConnection(const Napi::CallbackInfo& info) {
  return ConnectionCall(info, true);
}

Napi::Value CloseConnection(const Napi::CallbackInfo& info) {
  return ConnectionCall(info, false);
}

// Nothing arrives until the incoming connection is accepted, so the default is
// to accept every request on the configured socket and tell JS about it. A
// caller that wants to vet peers passes autoAccept: false and calls
// acceptConnection itself from the event.
void EOS_CALL OnConnectionRequest(
    const EOS_P2P_OnIncomingConnectionRequestInfo* data) {
  if (data == nullptr) return;
  State& state = GetState();
  if (state.emitter.IsEmpty()) return;
  Napi::Env env = state.emitter.Env();
  Napi::HandleScope scope(env);

  if (g_auto_accept && state.platform != nullptr) {
    EOS_P2P_AcceptConnectionOptions accept_options = {};
    accept_options.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
    accept_options.LocalUserId = data->LocalUserId;
    accept_options.RemoteUserId = data->RemoteUserId;
    accept_options.SocketId = data->SocketId;
    EOS_P2P_AcceptConnection(Handle(), &accept_options);
  }

  Napi::Object payload = Napi::Object::New(env);
  payload.Set("remoteUserId",
              Napi::String::New(env, PuidToString(data->RemoteUserId)));
  payload.Set("socketName",
              Napi::String::New(env, data->SocketId ? data->SocketId->SocketName
                                                    : ""));
  payload.Set("accepted", Napi::Boolean::New(env, g_auto_accept));
  EmitEvent(env, "p2p:connection-request", payload);
}

void EOS_CALL OnConnectionEstablished(
    const EOS_P2P_OnPeerConnectionEstablishedInfo* data) {
  if (data == nullptr) return;
  State& state = GetState();
  if (state.emitter.IsEmpty()) return;
  Napi::Env env = state.emitter.Env();
  Napi::HandleScope scope(env);

  Napi::Object payload = Napi::Object::New(env);
  payload.Set("remoteUserId",
              Napi::String::New(env, PuidToString(data->RemoteUserId)));
  payload.Set("socketName",
              Napi::String::New(env, data->SocketId ? data->SocketId->SocketName
                                                    : ""));
  // Relayed connections are the symmetric-NAT fallback path. Surfacing this is
  // how you find out in testing that it is being used.
  payload.Set("connectionType",
              Napi::Number::New(env, static_cast<int>(data->ConnectionType)));
  payload.Set("networkType",
              Napi::Number::New(env, static_cast<int>(data->NetworkType)));
  EmitEvent(env, "p2p:connected", payload);
}

void EOS_CALL OnConnectionClosed(
    const EOS_P2P_OnRemoteConnectionClosedInfo* data) {
  if (data == nullptr) return;
  State& state = GetState();
  if (state.emitter.IsEmpty()) return;
  Napi::Env env = state.emitter.Env();
  Napi::HandleScope scope(env);

  Napi::Object payload = Napi::Object::New(env);
  payload.Set("remoteUserId",
              Napi::String::New(env, PuidToString(data->RemoteUserId)));
  payload.Set("socketName",
              Napi::String::New(env, data->SocketId ? data->SocketId->SocketName
                                                    : ""));
  payload.Set("reason", Napi::Number::New(env, static_cast<int>(data->Reason)));
  EmitEvent(env, "p2p:disconnected", payload);
}

// watchConnections() -- registers the three P2P notifications. Safe to call
// more than once; later calls are no-ops.
Napi::Value WatchConnections(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequirePlatform(env)) return env.Undefined();
  if (g_request_notification != EOS_INVALID_NOTIFICATIONID) {
    return env.Undefined();
  }

  EOS_HP2P handle = Handle();
  EOS_P2P_SocketId socket_id;
  FillSocketId(&socket_id, g_socket_name);
  const bool scoped = !g_socket_name.empty();

  EOS_P2P_AddNotifyPeerConnectionRequestOptions request_options = {};
  request_options.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST;
  request_options.LocalUserId = g_local_user;
  request_options.SocketId = scoped ? &socket_id : nullptr;
  g_request_notification = EOS_P2P_AddNotifyPeerConnectionRequest(
      handle, &request_options, nullptr, &OnConnectionRequest);

  EOS_P2P_AddNotifyPeerConnectionEstablishedOptions established_options = {};
  established_options.ApiVersion =
      EOS_P2P_ADDNOTIFYPEERCONNECTIONESTABLISHED_API_LATEST;
  established_options.LocalUserId = g_local_user;
  established_options.SocketId = scoped ? &socket_id : nullptr;
  g_established_notification = EOS_P2P_AddNotifyPeerConnectionEstablished(
      handle, &established_options, nullptr, &OnConnectionEstablished);

  EOS_P2P_AddNotifyPeerConnectionClosedOptions closed_options = {};
  closed_options.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONCLOSED_API_LATEST;
  closed_options.LocalUserId = g_local_user;
  closed_options.SocketId = scoped ? &socket_id : nullptr;
  g_closed_notification = EOS_P2P_AddNotifyPeerConnectionClosed(
      handle, &closed_options, nullptr, &OnConnectionClosed);

  return env.Undefined();
}

}  // namespace

void DrainPackets(const Napi::Env& env) {
  State& state = GetState();
  if (state.platform == nullptr || g_local_user == nullptr) return;
  if (state.emitter.IsEmpty()) return;

  EOS_HP2P handle = Handle();
  std::vector<uint8_t> buffer;

  for (uint32_t drained = 0; drained < g_max_packets_per_tick; ++drained) {
    EOS_P2P_GetNextReceivedPacketSizeOptions size_options = {};
    size_options.ApiVersion = EOS_P2P_GETNEXTRECEIVEDPACKETSIZE_API_LATEST;
    size_options.LocalUserId = g_local_user;
    size_options.RequestedChannel = nullptr;  // any channel

    uint32_t packet_size = 0;
    if (EOS_P2P_GetNextReceivedPacketSize(handle, &size_options,
                                          &packet_size) !=
        EOS_EResult::EOS_Success) {
      return;  // queue empty
    }

    buffer.resize(packet_size);
    EOS_ProductUserId peer_id = nullptr;
    EOS_P2P_SocketId socket_id = {};
    uint8_t channel = 0;
    uint32_t bytes_written = 0;

    EOS_P2P_ReceivePacketOptions receive_options = {};
    receive_options.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
    receive_options.LocalUserId = g_local_user;
    receive_options.MaxDataSizeBytes = packet_size;
    receive_options.RequestedChannel = nullptr;

    if (EOS_P2P_ReceivePacket(handle, &receive_options, &peer_id, &socket_id,
                              &channel, buffer.data(),
                              &bytes_written) != EOS_EResult::EOS_Success) {
      return;
    }

    Napi::HandleScope scope(env);
    Napi::Object payload = Napi::Object::New(env);
    payload.Set("peerId", Napi::String::New(env, PuidToString(peer_id)));
    payload.Set("socketName", Napi::String::New(env, socket_id.SocketName));
    payload.Set("channel", Napi::Number::New(env, channel));
    payload.Set("data", Napi::Buffer<uint8_t>::Copy(env, buffer.data(),
                                                    bytes_written));
    EmitEvent(env, "p2p:packet", payload);
  }

  // Hitting the cap means the tick is not keeping up. Say so rather than
  // spinning here and starving the event loop.
  Napi::HandleScope scope(env);
  Napi::Object payload = Napi::Object::New(env);
  payload.Set("maxPacketsPerTick",
              Napi::Number::New(env, g_max_packets_per_tick));
  EmitEvent(env, "p2p:backlog", payload);
}

void ClearNotifications() {
  State& state = GetState();
  if (state.platform == nullptr) return;
  EOS_HP2P handle = Handle();

  if (g_request_notification != EOS_INVALID_NOTIFICATIONID) {
    EOS_P2P_RemoveNotifyPeerConnectionRequest(handle, g_request_notification);
    g_request_notification = EOS_INVALID_NOTIFICATIONID;
  }
  if (g_established_notification != EOS_INVALID_NOTIFICATIONID) {
    EOS_P2P_RemoveNotifyPeerConnectionEstablished(handle,
                                                  g_established_notification);
    g_established_notification = EOS_INVALID_NOTIFICATIONID;
  }
  if (g_closed_notification != EOS_INVALID_NOTIFICATIONID) {
    EOS_P2P_RemoveNotifyPeerConnectionClosed(handle, g_closed_notification);
    g_closed_notification = EOS_INVALID_NOTIFICATIONID;
  }
  g_local_user = nullptr;
}

void Init(Napi::Env env, Napi::Object exports) {
  Napi::Object ns = Napi::Object::New(env);
  ns.Set("configure", Napi::Function::New(env, Configure));
  ns.Set("send", Napi::Function::New(env, Send));
  ns.Set("acceptConnection", Napi::Function::New(env, AcceptConnection));
  ns.Set("closeConnection", Napi::Function::New(env, CloseConnection));
  ns.Set("watchConnections", Napi::Function::New(env, WatchConnections));
  exports.Set("p2p", ns);
}

}  // namespace p2p
}  // namespace eosjs
