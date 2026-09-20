/**
 * node-eos-sdk -- Node.js and Electron binding for the Epic Online Services
 * C SDK. Platform, Connect, Lobby and P2P.
 */

/// <reference types="node" />

import { EventEmitter } from 'node:events';

export interface InitOptions {
  /** Developer Portal -> Product Settings. */
  productId: string;
  sandboxId: string;
  deploymentId: string;
  clientId: string;
  clientSecret: string;
  /** Exactly 64 hex characters. Required by the SDK even when unused. */
  encryptionKey: string;
  isServer?: boolean;
  /** Route SDK logging to the `log` event. The SDK is talkative. */
  debug?: boolean;
  /** Default 50 ms (20 Hz). */
  tickIntervalMs?: number;
  productName?: string;
  productVersion?: string;
  cacheDirectory?: string;
  /** Raw EOS_EPlatformFlags bitmask. */
  flags?: number;
}

export type CredentialTypeName =
  | 'EPIC'
  | 'STEAM_SESSION_TICKET'
  | 'DEVICE_ID_ACCESS_TOKEN'
  | 'OPENID_ACCESS_TOKEN'
  | 'DISCORD_ACCESS_TOKEN'
  | 'GOG_SESSION_TICKET'
  | 'APPLE_ID_TOKEN'
  | 'GOOGLE_ID_TOKEN'
  | 'ITCHIO_JWT'
  | 'ITCHIO_KEY'
  | 'AMAZON_ACCESS_TOKEN'
  | 'XBL_XSTS_TOKEN'
  | 'PSN_ID_TOKEN'
  | 'NINTENDO_NSA_ID_TOKEN';

export interface LoginOptions {
  type: CredentialTypeName;
  /** Not required for DEVICE_ID_ACCESS_TOKEN. */
  token?: string;
  /** Required for device-id logins; ignored by most other types. */
  displayName?: string;
}

export type PacketReliabilityName =
  | 'unreliableUnordered'
  | 'reliableUnordered'
  | 'reliableOrdered';

export type LobbyPermissionLevelName =
  | 'publicAdvertised'
  | 'joinViaPresence'
  | 'inviteOnly';

export interface CreateLobbyOptions {
  localUserId: string;
  maxMembers?: number;
  bucketId?: string;
  permissionLevel?: LobbyPermissionLevelName;
}

export interface JoinLobbyOptions {
  localUserId: string;
  lobbyId: string;
}

export interface LobbyInfo {
  lobbyId: string;
  ownerUserId: string;
  maxMembers: number;
  availableSlots: number;
  permissionLevel: LobbyPermissionLevelName;
  allowInvites: boolean;
  bucketId: string;
}

export type LobbyAttributeValue = string | number | boolean;
export type LobbyAttributes = Record<string, LobbyAttributeValue>;

/**
 * A joined lobby. The shape mirrors steamworks.js's `matchmaking.Lobby`, so a
 * lobby UI written against that works unchanged against EOS.
 */
export declare class Lobby {
  readonly id: string;
  readonly localUserId: string;

  /** Product user ids of every member, including the local user. */
  getMembers(): string[];
  /** The host's product user id. */
  getOwner(): string;
  getInfo(): LobbyInfo;

  getFullData(): LobbyAttributes;
  getData(key: string): LobbyAttributeValue | undefined;
  /** Host only. Members get a permission error. */
  setData(key: string, value: LobbyAttributeValue): Promise<void>;
  setData(attributes: LobbyAttributes): Promise<void>;

  /** Attributes a member set on themselves. Defaults to the local user. */
  getMemberData(targetUserId?: string): LobbyAttributes;
  setMemberData(key: string, value: LobbyAttributeValue): Promise<void>;
  setMemberData(attributes: LobbyAttributes): Promise<void>;

  sendInvite(targetUserId: string): Promise<void>;
  leave(): Promise<void>;
}

export interface P2PConfigureOptions {
  localUserId: string;
  /** Both peers must use the same name. */
  socketName: string;
  /** Default true. Nothing arrives until a connection is accepted. */
  autoAccept?: boolean;
  /** Default 1024. Exceeding it emits `p2p:backlog`. */
  maxPacketsPerTick?: number;
  /**
   * Channel reserved for sendLarge() fragments. Default 255. Traffic on it is
   * consumed by reassembly and surfaces as `p2p:message`, so do not also use
   * this channel for raw send().
   */
  fragmentChannel?: number;
}

export interface P2PSendLargeOptions {
  remoteUserId: string;
  data: Buffer;
  localUserId?: string;
  socketName?: string;
  /** Default 'reliableOrdered'. An unreliable mode can strand a partial. */
  reliability?: PacketReliabilityName;
}

export interface P2PSendOptions {
  remoteUserId: string;
  data: Buffer;
  localUserId?: string;
  socketName?: string;
  channel?: number;
  /** Default 'reliableOrdered'. */
  reliability?: PacketReliabilityName;
}

export interface P2PConnectionOptions {
  remoteUserId: string;
  localUserId?: string;
  socketName?: string;
}

export interface LogEvent {
  category: string;
  message: string;
  level: number;
}

export interface PacketEvent {
  peerId: string;
  socketName: string;
  channel: number;
  data: Buffer;
}

/** A payload sent with sendLarge(), delivered once every fragment has arrived. */
export interface MessageEvent {
  peerId: string;
  socketName: string;
  data: Buffer;
}

export interface EosEvents {
  log: [LogEvent];
  error: [Error];
  'connect:auth-expiring': [{ productUserId: string }];
  'lobby:updated': [{ lobbyId: string }];
  'lobby:member-updated': [{ lobbyId: string; targetUserId: string }];
  'lobby:member-status': [
    {
      lobbyId: string;
      targetUserId: string;
      status: number;
      statusName: string;
    },
  ];
  'lobby:invite-accepted': [
    {
      inviteId: string;
      lobbyId: string;
      localUserId: string;
      targetUserId: string;
    },
  ];
  'p2p:connection-request': [
    { remoteUserId: string; socketName: string; accepted: boolean },
  ];
  'p2p:connected': [
    {
      remoteUserId: string;
      socketName: string;
      connectionType: number;
      networkType: number;
    },
  ];
  'p2p:disconnected': [
    { remoteUserId: string; socketName: string; reason: number },
  ];
  'p2p:packet': [PacketEvent];
  'p2p:message': [MessageEvent];
  'p2p:backlog': [{ maxPacketsPerTick: number }];
}

/** An error thrown or rejected by an EOS call. */
export interface EosError extends Error {
  /** The EOS_EResult name, e.g. 'EOS_InvalidAuth'. */
  code: string;
  /** The SDK function that failed, e.g. 'EOS_Connect_Login'. */
  operation: string;
  resultCode: number;
}

/**
 * The addon could not be loaded at all -- usually because the SDK was never
 * vendored. It carries no EOS result, because no EOS call was reached.
 */
export interface AddonLoadError extends Error {
  code: 'EOS_ADDON_NOT_LOADED';
  cause?: unknown;
}

export declare class EosClient extends EventEmitter<EosEvents> {
  /**
   * Can the native addon be loaded? Probes once and caches.
   *
   * `require()` of this package succeeds even with no SDK vendored, so this is
   * how to decide whether to offer multiplayer at all. `isInitialized` does not
   * answer it: that is false for a missing addon and for an uninitialised one
   * alike.
   */
  isAvailable(): boolean;
  /** Why the addon would not load, or null if it loaded or was never tried. */
  readonly loadError: AddonLoadError | null;
  /** Initialise the SDK and start ticking. Once per process. */
  init(options: InitOptions): this;
  /** Release the platform. EOS cannot be re-initialised afterwards. */
  shutdown(): this;
  readonly isInitialized: boolean;
  setLogLevel(level: number): this;

  readonly connect: {
    /** Resolves to a Product User ID. Handles first-login user creation. */
    login(options: LoginOptions): Promise<string>;
    /** Device-id account for this machine; no external store required. */
    createDeviceId(deviceModel?: string): Promise<void>;
    getLoggedInUsers(): string[];
  };

  readonly lobby: {
    create(options: CreateLobbyOptions): Promise<Lobby>;
    join(options: JoinLobbyOptions): Promise<Lobby>;
    /** Re-attach to a lobby this process already joined. */
    attach(lobbyId: string, localUserId: string): Lobby;
  };

  readonly p2p: {
    /** Required before any packet is delivered. */
    configure(options: P2PConfigureOptions): void;
    send(options: P2PSendOptions): void;
    /**
     * Send a payload of any size, fragmenting past the 1170-byte wire limit.
     * Arrives as one `p2p:message`. Returns the fragment count.
     */
    sendLarge(options: P2PSendLargeOptions): number;
    acceptConnection(options: P2PConnectionOptions): void;
    closeConnection(options: P2PConnectionOptions): void;
  };
}

declare const client: EosClient;
export default client;

export declare const CredentialType: Readonly<
  Record<CredentialTypeName, CredentialTypeName>
>;
export declare const PacketReliability: Readonly<{
  UNRELIABLE_UNORDERED: 'unreliableUnordered';
  RELIABLE_UNORDERED: 'reliableUnordered';
  RELIABLE_ORDERED: 'reliableOrdered';
}>;
export declare const LobbyPermissionLevel: Readonly<{
  PUBLIC_ADVERTISED: 'publicAdvertised';
  JOIN_VIA_PRESENCE: 'joinViaPresence';
  INVITE_ONLY: 'inviteOnly';
}>;
export declare const LobbyMemberStatus: Readonly<Record<number, string>>;
export declare const LogLevel: Readonly<{
  OFF: 0;
  FATAL: 100;
  ERROR: 200;
  WARNING: 300;
  INFO: 400;
  VERBOSE: 500;
  VERY_VERBOSE: 600;
}>;
/** EOS_P2P_MAX_PACKET_SIZE. Use `p2p.sendLarge()` for anything bigger. */
export declare const MAX_PACKET_SIZE: 1170;
/** Bytes of binary header each fragment carries. */
export declare const FRAGMENT_HEADER_BYTES: 8;
/** Largest payload that fits in one fragment, header deducted. */
export declare const MAX_FRAGMENT_PAYLOAD: 1162;
/** Channel reserved for fragments unless overridden in `p2p.configure`. */
export declare const DEFAULT_FRAGMENT_CHANNEL: 255;

/**
 * Split a payload into wire-sized fragments. Exported for testing and for
 * implementing the other half of the protocol in another language.
 *
 * Header: message id (uint32le), index (uint16le), total (uint16le).
 */
export declare function fragment(
  data: Buffer,
  messageId: number,
  maxPayload?: number,
): Buffer[];

/** Rebuilds messages from fragments, bounded in both bytes and age. */
export declare class Reassembler {
  constructor(options?: { maxPendingBytes?: number; ttlMs?: number });
  /** The complete message, or null if more fragments are needed. */
  accept(peerId: string, packet: Buffer, now?: number): Buffer | null;
  clear(): void;
  readonly pendingBytes: number;
  readonly pendingMessages: number;
}
