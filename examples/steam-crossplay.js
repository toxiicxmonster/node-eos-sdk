'use strict';

/**
 * Cross-store login: a Steam player and an Epic player reaching the same lobby.
 *
 * This is the point of using EOS at all. Steam's own networking addresses Steam
 * accounts and nothing else, so a Steam-native transport can never carry a
 * match against an Epic Games Store buyer. EOS Connect maps any external
 * account onto a Product User ID, and Lobby and P2P address PUIDs -- so after
 * login, the two players are indistinguishable to the rest of your code.
 *
 * Requires steamworks.js for the Steam path:
 *   npm install steamworks.js
 *
 * And, in the Epic Developer Portal, Steam configured as an identity provider
 * with your Steam App ID and a Steam Web API key.
 */

const eos = require('node-eos-sdk');

const STEAM_APP_ID = Number(process.env.STEAM_APP_ID);

/**
 * Steam path. An Epic account is created and linked under the hood, with no
 * email or password prompt for the player.
 */
async function loginWithSteam() {
  const steamworks = require('steamworks.js');
  const client = steamworks.init(STEAM_APP_ID);
  const steamId = client.localplayer.getSteamId();

  const ticket = await client.auth.getSessionTicketWithSteamId(
    steamId.steamId64,
  );

  return {
    productUserId: await eos.connect.login({
      type: eos.CredentialType.STEAM_SESSION_TICKET,
      token: ticket.getBytes().toString('hex'),
    }),
    // Kept so the expiry handler below can re-acquire a ticket.
    refreshTicket: async () => {
      const fresh = await client.auth.getSessionTicketWithSteamId(
        steamId.steamId64,
      );
      return fresh.getBytes().toString('hex');
    },
  };
}

/**
 * Epic path. The token comes from EOS_Auth_Login -- either the Epic Games
 * Launcher's exchange code, passed on the command line when the game is
 * launched from the launcher, or an account portal prompt.
 *
 * Auth is only needed to obtain this token. Everything afterwards is Connect.
 */
async function loginWithEpic(epicAuthToken) {
  return {
    productUserId: await eos.connect.login({
      type: eos.CredentialType.EPIC,
      token: epicAuthToken,
    }),
    refreshTicket: null,
  };
}

async function main() {
  eos.init({
    productId: process.env.EOS_PRODUCT_ID,
    sandboxId: process.env.EOS_SANDBOX_ID,
    deploymentId: process.env.EOS_DEPLOYMENT_ID,
    clientId: process.env.EOS_CLIENT_ID,
    clientSecret: process.env.EOS_CLIENT_SECRET,
    encryptionKey: process.env.EOS_ENCRYPTION_KEY,
  });

  const epicToken = process.env.EPIC_AUTH_TOKEN;
  const session = epicToken
    ? await loginWithEpic(epicToken)
    : await loginWithSteam();

  console.log('Product User ID:', session.productUserId);

  // Steam session tickets expire after roughly 8 hours of continuous play,
  // after which Connect logins start failing with "invalid session ticket".
  // The SDK warns first. Handle it here rather than discovering it in a
  // playtest when someone gets dropped mid-match.
  eos.on('connect:auth-expiring', async ({ productUserId }) => {
    if (!session.refreshTicket) return;
    console.log('auth expiring for', productUserId, '- refreshing ticket');
    try {
      await eos.connect.login({
        type: eos.CredentialType.STEAM_SESSION_TICKET,
        token: await session.refreshTicket(),
      });
      console.log('ticket refreshed');
    } catch (error) {
      console.error('refresh failed:', error.code ?? error.message);
    }
  });

  // From here on, nothing else in the codebase needs to know which store this
  // player bought the game from.
  const lobby = await eos.lobby.create({
    localUserId: session.productUserId,
    maxMembers: 4,
    bucketId: 'crossplay',
    permissionLevel: eos.LobbyPermissionLevel.PUBLIC_ADVERTISED,
  });
  console.log('lobby:', lobby.id, '-- share this id to join');
}

main().catch((error) => {
  console.error(error.message);
  process.exit(1);
});
