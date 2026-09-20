'use strict';

/**
 * A transport for deterministic lockstep netcode, built on this binding.
 *
 * Included because lockstep is the case that drove the design, and because the
 * two decisions it forces are not obvious:
 *
 *   Reliability is reliableOrdered. A lost or reordered turn stalls a lockstep
 *   match permanently, so head-of-line blocking is the right trade: a pause is
 *   recoverable, divergence is not.
 *
 *   Topology is a star -- the host relays. Every peer still simulates
 *   everything; only the routing changes. The reason is NAT: a four-player mesh
 *   needs six traversals to all succeed, a star needs three. The host is also
 *   the natural arbiter for seats and drop detection. Relaying costs nothing at
 *   these volumes -- a four-player match is around 0.08 KB/s per peer.
 *
 * The interface below is the seam a lockstep engine sits behind:
 *
 *   connect(), send(payload), onPayload(fn), onPeers(fn), disconnect(),
 *   localId, peerIds
 *
 * Host migration, if you want it later, is unusually cheap in this shape: every
 * peer already holds a bit-identical world, so a survivor becomes the new relay
 * and everyone resumes on an agreed turn. There is no state to transfer.
 */

const eos = require('node-eos-sdk');

const SOCKET_NAME = 'lockstep';

class EosTransport {
  #lobby = null;
  #payloadHandlers = new Set();
  #peerHandlers = new Set();
  #unsubscribe = [];

  /**
   * @param {object} options
   * @param {string} options.localUserId  from connect.login()
   * @param {string} [options.lobbyId]    omit to host
   */
  constructor({ localUserId, lobbyId }) {
    this.localId = localUserId;
    this.peerIds = [];
    this.isHost = !lobbyId;
    this.lobbyId = lobbyId;
  }

  async connect() {
    eos.p2p.configure({
      localUserId: this.localId,
      socketName: SOCKET_NAME,
    });

    this.#lobby = this.isHost
      ? await eos.lobby.create({
          localUserId: this.localId,
          maxMembers: 4,
          bucketId: 'lockstep',
          permissionLevel: 'publicAdvertised',
        })
      : await eos.lobby.join({
          localUserId: this.localId,
          lobbyId: this.lobbyId,
        });

    this.lobbyId = this.#lobby.id;
    this.hostId = this.#lobby.getOwner();
    this.#refreshPeers();

    this.#listen('p2p:packet', (packet) => this.#onPacket(packet));
    this.#listen('lobby:member-status', () => this.#refreshPeers());
    return this;
  }

  #listen(event, handler) {
    eos.on(event, handler);
    this.#unsubscribe.push(() => eos.off(event, handler));
  }

  #refreshPeers() {
    const members = this.#lobby.getMembers();
    // Seat order must be identical on every peer, or the simulations disagree
    // about who is who. Sorting the PUIDs is the cheapest agreement there is.
    this.peerIds = [...members].sort();
    for (const handler of this.#peerHandlers) handler(this.peerIds);
  }

  /**
   * Hand a turn's commands to the wire.
   *
   * Non-hosts send only to the host; the host fans out. Both paths deliver the
   * payload to the local engine as well, so every peer sees its own commands on
   * the same turn as everyone else does.
   */
  send(payload) {
    const message = Buffer.from(
      JSON.stringify({ from: this.localId, payload }),
    );

    if (this.isHost) {
      this.#relay(message, null);
    } else {
      eos.p2p.send({ remoteUserId: this.hostId, data: message });
    }
    this.#deliver(this.localId, payload);
  }

  #relay(message, exceptPeerId) {
    for (const peer of this.peerIds) {
      if (peer === this.localId || peer === exceptPeerId) continue;
      eos.p2p.send({ remoteUserId: peer, data: message });
    }
  }

  #onPacket({ peerId, socketName, data }) {
    if (socketName !== SOCKET_NAME) return;

    const message = JSON.parse(data.toString());
    // The host is the relay: forward to everyone except the sender, then
    // deliver locally. Non-hosts only deliver.
    if (this.isHost) this.#relay(data, peerId);
    this.#deliver(message.from, message.payload);
  }

  #deliver(from, payload) {
    for (const handler of this.#payloadHandlers) handler(from, payload);
  }

  /** @param {(from: string, payload: unknown) => void} fn */
  onPayload(fn) {
    this.#payloadHandlers.add(fn);
    return () => this.#payloadHandlers.delete(fn);
  }

  /** @param {(peerIds: string[]) => void} fn */
  onPeers(fn) {
    this.#peerHandlers.add(fn);
    return () => this.#peerHandlers.delete(fn);
  }

  async disconnect() {
    for (const off of this.#unsubscribe) off();
    this.#unsubscribe = [];
    this.#payloadHandlers.clear();
    this.#peerHandlers.clear();
    if (this.#lobby) await this.#lobby.leave();
    this.#lobby = null;
  }
}

module.exports = { EosTransport };
