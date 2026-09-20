'use strict';

/**
 * node-eos-sdk -- Node.js and Electron binding for the Epic Online Services
 * C SDK.
 *
 * Everything convenient lives here rather than in C++: the tick interval, the
 * EventEmitter, the Lobby object. The native layer stays as close to the C API
 * as it can, because that is the part that is expensive to change.
 *
 * The SDK is a polled library. Nothing progresses -- no login completes, no
 * lobby update arrives, no packet is delivered -- unless EOS_Platform_Tick runs
 * regularly. init() starts a 50 ms interval that does this, and every SDK
 * callback therefore fires inside that tick, on the main thread. Keep the event
 * loop responsive and EOS stays responsive with it.
 */

const { EventEmitter } = require('node:events');
const path = require('node:path');

const DEFAULT_TICK_INTERVAL_MS = 50;

/** EOS_EExternalCredentialType values accepted by connect.login(). */
const CredentialType = Object.freeze({
  EPIC: 'EPIC',
  STEAM_SESSION_TICKET: 'STEAM_SESSION_TICKET',
  DEVICE_ID_ACCESS_TOKEN: 'DEVICE_ID_ACCESS_TOKEN',
  OPENID_ACCESS_TOKEN: 'OPENID_ACCESS_TOKEN',
  DISCORD_ACCESS_TOKEN: 'DISCORD_ACCESS_TOKEN',
  GOG_SESSION_TICKET: 'GOG_SESSION_TICKET',
  APPLE_ID_TOKEN: 'APPLE_ID_TOKEN',
  GOOGLE_ID_TOKEN: 'GOOGLE_ID_TOKEN',
  ITCHIO_JWT: 'ITCHIO_JWT',
  ITCHIO_KEY: 'ITCHIO_KEY',
  AMAZON_ACCESS_TOKEN: 'AMAZON_ACCESS_TOKEN',
  XBL_XSTS_TOKEN: 'XBL_XSTS_TOKEN',
  PSN_ID_TOKEN: 'PSN_ID_TOKEN',
  NINTENDO_NSA_ID_TOKEN: 'NINTENDO_NSA_ID_TOKEN',
});

const PacketReliability = Object.freeze({
  UNRELIABLE_UNORDERED: 'unreliableUnordered',
  RELIABLE_UNORDERED: 'reliableUnordered',
  RELIABLE_ORDERED: 'reliableOrdered',
});

const LobbyPermissionLevel = Object.freeze({
  PUBLIC_ADVERTISED: 'publicAdvertised',
  JOIN_VIA_PRESENCE: 'joinViaPresence',
  INVITE_ONLY: 'inviteOnly',
});

/** EOS_ELogLevel. */
const LogLevel = Object.freeze({
  OFF: 0,
  FATAL: 100,
  ERROR: 200,
  WARNING: 300,
  INFO: 400,
  VERBOSE: 500,
  VERY_VERBOSE: 600,
});

/** EOS_ELobbyMemberStatus, as reported by the `lobby:member-status` event. */
const LobbyMemberStatus = Object.freeze({
  0: 'joined',
  1: 'left',
  2: 'disconnected',
  3: 'kicked',
  4: 'promoted',
  5: 'closed',
});

let native = null;

function loadNative() {
  if (native) return native;
  try {
    native = require('node-gyp-build')(path.join(__dirname));
  } catch (cause) {
    const error = new Error(
      'Failed to load the node-eos-sdk native addon.\n\n' +
        'The most common cause is that the Epic Online Services SDK has not ' +
        'been placed in vendor/eos/. The SDK is not redistributable, so this ' +
        'package cannot ship it; you download it from Epic and drop it in.\n\n' +
        'Run `node node_modules/node-eos-sdk/scripts/check-sdk.js` for a ' +
        'diagnosis, and see docs/sdk-setup.md for the full procedure.\n\n' +
        `Underlying error: ${cause.message}`,
    );
    error.code = 'EOS_ADDON_NOT_LOADED';
    error.cause = cause;
    throw error;
  }
  return native;
}

/**
 * A joined lobby.
 *
 * The shape is deliberately close to steamworks.js's `matchmaking.Lobby`, so a
 * lobby UI written against that does not need to know which backend is
 * underneath.
 */
class Lobby {
  #client;

  constructor(client, lobbyId, localUserId) {
    this.#client = client;
    this.id = lobbyId;
    this.localUserId = localUserId;
  }

  #scope(extra = {}) {
    return { lobbyId: this.id, localUserId: this.localUserId, ...extra };
  }

  /** @returns {string[]} product user ids, including the local user. */
  getMembers() {
    return loadNative().lobby.getMembers(this.#scope());
  }

  /** @returns {string} the host's product user id. */
  getOwner() {
    return this.getInfo().ownerUserId;
  }

  getInfo() {
    return loadNative().lobby.getInfo(this.#scope());
  }

  /** Every lobby-wide attribute, as a plain object. */
  getFullData() {
    return loadNative().lobby.getAttributes(this.#scope());
  }

  getData(key) {
    return this.getFullData()[key];
  }

  /**
   * Set lobby-wide attributes. Host only -- a member gets a permission error.
   * Accepts either setData('key', value) or setData({ key: value }).
   */
  setData(keyOrObject, maybeValue) {
    const attributes =
      typeof keyOrObject === 'string'
        ? { [keyOrObject]: maybeValue }
        : keyOrObject;
    return loadNative().lobby.setAttributes(this.#scope({ attributes }));
  }

  /** Attributes a given member set on themselves. Defaults to the local user. */
  getMemberData(targetUserId = this.localUserId) {
    return loadNative().lobby.getMemberAttributes(this.#scope({ targetUserId }));
  }

  /** Set the local member's own attributes (faction, team, ready, ...). */
  setMemberData(keyOrObject, maybeValue) {
    const attributes =
      typeof keyOrObject === 'string'
        ? { [keyOrObject]: maybeValue }
        : keyOrObject;
    return loadNative().lobby.setMemberAttributes(this.#scope({ attributes }));
  }

  sendInvite(targetUserId) {
    return loadNative().lobby.sendInvite(this.#scope({ targetUserId }));
  }

  leave() {
    return loadNative().lobby.leave(this.#scope());
  }
}

/**
 * The EOS client. A process-wide singleton, because EOS_Initialize may be
 * called at most once per process and cannot be called again after shutdown.
 *
 * Events:
 *   'log'                     { category, message, level }   SDK logging
 *   'connect:auth-expiring'   { productUserId }              refresh the ticket
 *   'lobby:updated'           { lobbyId }
 *   'lobby:member-updated'    { lobbyId, targetUserId }
 *   'lobby:member-status'     { lobbyId, targetUserId, status, statusName }
 *   'lobby:invite-accepted'   { inviteId, lobbyId, localUserId, targetUserId }
 *   'p2p:connection-request'  { remoteUserId, socketName, accepted }
 *   'p2p:connected'           { remoteUserId, socketName, connectionType, ... }
 *   'p2p:disconnected'        { remoteUserId, socketName, reason }
 *   'p2p:packet'              { peerId, socketName, channel, data }
 *   'p2p:backlog'             { maxPacketsPerTick }
 */
class EosClient extends EventEmitter {
  #tickTimer = null;
  #debug = false;

  constructor() {
    super();
    // Packet delivery is an event; a game that forgets a listener for a moment
    // during startup should not take down the process.
    this.setMaxListeners(0);
  }

  /**
   * Initialise the SDK and start ticking.
   *
   * @param {object} options
   * @param {string} options.productId      Developer Portal -> Product Settings
   * @param {string} options.sandboxId
   * @param {string} options.deploymentId
   * @param {string} options.clientId
   * @param {string} options.clientSecret
   * @param {string} options.encryptionKey  64 hex characters; required even if unused
   * @param {boolean} [options.isServer=false]
   * @param {boolean} [options.debug=false]  route SDK logging to the 'log' event
   * @param {number}  [options.tickIntervalMs=50]
   * @param {string}  [options.productName]
   * @param {string}  [options.productVersion]
   * @param {string}  [options.cacheDirectory]
   */
  init(options) {
    const api = loadNative();
    if (this.#tickTimer) {
      throw new Error('eos.init() has already been called in this process.');
    }

    this.#debug = Boolean(options && options.debug);

    // Register the sink before init so that a failing EOS_Platform_Create can
    // explain itself through the 'log' event.
    api._setEventSink((name, payload) => this.#dispatch(name, payload));
    api.platform.init(options);

    const interval = Number(options.tickIntervalMs) || DEFAULT_TICK_INTERVAL_MS;
    this.#tickTimer = setInterval(() => {
      try {
        api.platform.tick();
      } catch (error) {
        this.emit('error', error);
      }
    }, interval);

    // The timer is deliberately not unref'd: while EOS is initialised the
    // process should stay alive, and shutdown() is the way out.
    api.connect.watchAuthExpiration();
    api.lobby.watchLobby();
    return this;
  }

  /** True once init() has succeeded and before shutdown(). */
  get isInitialized() {
    return Boolean(native && native.platform.isInitialized());
  }

  /**
   * Release the platform and shut the SDK down.
   *
   * EOS cannot be re-initialised afterwards in the same process; a second
   * init() throws. Electron reloads during development hit this, which is why
   * it is an explicit error rather than a silent failure.
   */
  shutdown() {
    if (this.#tickTimer) {
      clearInterval(this.#tickTimer);
      this.#tickTimer = null;
    }
    if (native) {
      native.platform.shutdown();
      native._setEventSink(null);
    }
    return this;
  }

  #dispatch(name, payload) {
    if (name === 'log' && !this.#debug) return;
    if (name === 'lobby:member-status') {
      payload.statusName = LobbyMemberStatus[payload.status] ?? 'unknown';
    }
    this.emit(name, payload);
  }

  // -- connect ------------------------------------------------------------

  get connect() {
    return {
      /**
       * Log in and resolve to a Product User ID.
       *
       * The PUID is the handle Lobby and P2P address, and it is what makes a
       * Steam player and an Epic player addressable by the same call. A first
       * login for an account is completed with EOS_Connect_CreateUser
       * internally, so callers see one promise either way.
       *
       * @param {{ type: string, token?: string, displayName?: string }} options
       * @returns {Promise<string>}
       */
      login: (options) => loadNative().connect.login(options),

      /**
       * Create a device-id account for this machine. The cheapest way to get a
       * PUID with no external store involved, which is what the loopback test
       * uses. EOS_DuplicateNotAllowed is treated as success.
       *
       * @param {string} [deviceModel]
       * @returns {Promise<void>}
       */
      createDeviceId: (deviceModel) =>
        loadNative().connect.createDeviceId(deviceModel),

      /** @returns {string[]} */
      getLoggedInUsers: () => loadNative().connect.getLoggedInUsers(),
    };
  }

  // -- lobby --------------------------------------------------------------

  get lobby() {
    return {
      /**
       * @param {object} options
       * @param {string} options.localUserId
       * @param {number} [options.maxMembers=4]
       * @param {string} [options.bucketId='default']
       * @param {string} [options.permissionLevel='inviteOnly']
       * @returns {Promise<Lobby>}
       */
      create: async (options) => {
        const lobbyId = await loadNative().lobby.create(options);
        return new Lobby(this, lobbyId, options.localUserId);
      },

      /**
       * Join by lobby id. EOS has no join-by-id primitive, so this searches for
       * the id and joins the single result; that is hidden behind one promise.
       *
       * @param {{ localUserId: string, lobbyId: string }} options
       * @returns {Promise<Lobby>}
       */
      join: async (options) => {
        const lobbyId = await loadNative().lobby.join(options);
        return new Lobby(this, lobbyId, options.localUserId);
      },

      /** Re-attach to a lobby this process is already in, without a round trip. */
      attach: (lobbyId, localUserId) => new Lobby(this, lobbyId, localUserId),
    };
  }

  // -- p2p ----------------------------------------------------------------

  get p2p() {
    return {
      /**
       * Tell the tick loop which user to drain packets for. Until this is
       * called, no 'p2p:packet' event will ever fire.
       *
       * A socket in EOS is just a named channel; both peers must use the same
       * name.
       *
       * @param {object} options
       * @param {string} options.localUserId
       * @param {string} options.socketName
       * @param {boolean} [options.autoAccept=true] accept incoming connections
       *   automatically. Nothing arrives until a connection is accepted.
       * @param {number} [options.maxPacketsPerTick=1024]
       */
      configure: (options) => {
        const api = loadNative();
        api.p2p.configure(options);
        api.p2p.watchConnections();
      },

      /**
       * @param {object} options
       * @param {string} options.remoteUserId
       * @param {Buffer} options.data
       * @param {number} [options.channel=0]
       * @param {string} [options.reliability='reliableOrdered']
       */
      send: (options) => loadNative().p2p.send(options),

      acceptConnection: (options) => loadNative().p2p.acceptConnection(options),
      closeConnection: (options) => loadNative().p2p.closeConnection(options),
    };
  }

  /** Change SDK log verbosity at runtime. See LogLevel. */
  setLogLevel(level) {
    loadNative().platform.setLogLevel(level);
    return this;
  }
}

const client = new EosClient();

module.exports = client;
module.exports.EosClient = EosClient;
module.exports.Lobby = Lobby;
module.exports.CredentialType = CredentialType;
module.exports.PacketReliability = PacketReliability;
module.exports.LobbyPermissionLevel = LobbyPermissionLevel;
module.exports.LobbyMemberStatus = LobbyMemberStatus;
module.exports.LogLevel = LogLevel;
/** EOS_P2P_MAX_PACKET_SIZE. Larger payloads must be fragmented by the caller. */
module.exports.MAX_PACKET_SIZE = 1170;
