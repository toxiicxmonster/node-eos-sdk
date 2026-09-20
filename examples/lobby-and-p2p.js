'use strict';

/**
 * Host or join a lobby, then exchange packets with everyone in it.
 *
 *   node examples/lobby-and-p2p.js host
 *   node examples/lobby-and-p2p.js join <lobbyId>
 *
 * Shows the whole shape of a session: lobby attributes carry the match setup,
 * member attributes carry per-player state, and P2P carries the traffic once
 * everyone has agreed to start.
 */

const eos = require('node-eos-sdk');

const SOCKET_NAME = 'example';
const [mode, lobbyIdArg] = process.argv.slice(2);

async function main() {
  eos.init({
    productId: process.env.EOS_PRODUCT_ID,
    sandboxId: process.env.EOS_SANDBOX_ID,
    deploymentId: process.env.EOS_DEPLOYMENT_ID,
    clientId: process.env.EOS_CLIENT_ID,
    clientSecret: process.env.EOS_CLIENT_SECRET,
    encryptionKey: process.env.EOS_ENCRYPTION_KEY,
  });

  await eos.connect.createDeviceId('example');
  const me = await eos.connect.login({
    type: eos.CredentialType.DEVICE_ID_ACCESS_TOKEN,
    displayName: mode === 'host' ? 'host' : 'guest',
  });
  console.log('me:', me);

  // Until configure() runs, no p2p:packet event will ever fire. autoAccept is
  // on by default: an EOS connection must be accepted before anything is
  // delivered, and refusing by default is a silent way to receive nothing.
  eos.p2p.configure({ localUserId: me, socketName: SOCKET_NAME });

  const lobby =
    mode === 'host'
      ? await eos.lobby.create({
          localUserId: me,
          maxMembers: 4,
          bucketId: 'example',
          permissionLevel: eos.LobbyPermissionLevel.PUBLIC_ADVERTISED,
        })
      : await eos.lobby.join({ localUserId: me, lobbyId: lobbyIdArg });

  console.log('lobby:', lobby.id);

  if (mode === 'host') {
    // The host owns the authoritative match setup.
    await lobby.setData({ map: 'ridge', seed: 20260920, timeOfDay: 'dusk' });
    console.log('Share this lobby id:', lobby.id);
  }

  // Members set only their own attributes.
  await lobby.setMemberData({ faction: 'north', team: 1, ready: false });

  console.log('setup:', lobby.getFullData());
  console.log('members:', lobby.getMembers());

  eos.on('lobby:member-status', ({ targetUserId, statusName }) => {
    console.log(`member ${targetUserId}: ${statusName}`);
    console.log('members now:', lobby.getMembers());
  });

  eos.on('lobby:updated', () => {
    console.log('setup changed:', lobby.getFullData());
  });

  eos.on('p2p:connected', ({ remoteUserId, connectionType }) => {
    // connectionType tells you whether EOS fell back to its relay, which is
    // what happens behind a symmetric NAT. Worth logging in real builds.
    console.log(`connected to ${remoteUserId} (type ${connectionType})`);
  });

  eos.on('p2p:packet', ({ peerId, data }) => {
    console.log(`from ${peerId}: ${data.toString()}`);
  });

  // Say hello to everyone else in the lobby, once a second.
  setInterval(() => {
    for (const peer of lobby.getMembers()) {
      if (peer === me) continue;
      eos.p2p.send({
        remoteUserId: peer,
        data: Buffer.from(`hello from ${me} at ${Date.now()}`),
      });
    }
  }, 1000);

  process.on('SIGINT', async () => {
    await lobby.leave();
    eos.shutdown();
    process.exit(0);
  });
}

main().catch((error) => {
  console.error(error.message);
  if (error.code) console.error('EOS result:', error.code);
  process.exit(1);
});
