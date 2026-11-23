import { createTest, expect } from '@ngx-playwright/test';
import { ApplicationScreen } from '../screens/main-screen.js';

const test = createTest(ApplicationScreen);

async function connect(page, address: string, password = '', remember = false) {
  await page.getByRole('textbox', { name: 'Address', exact: true }).fill(address);
  await page.locator('app-connect input[aria-label="Password"]').fill(password);
  await page.locator('#save-password').setChecked(remember);
  await page.locator('app-connect app-action-buttons').getByRole('button', { name: 'Connect', exact: true }).click();
}

async function disconnect(page) {
  await page.locator('.app-root-disconnect').click();
  await page.locator('app-disconnect').getByRole('button', { name: 'Disconnect', exact: true }).click();
  await expect(page.locator('.app-root-disconnect')).toHaveCount(0);
}

async function savedState(page) {
  return page.evaluate(() => (globalThis as any).skyrimtogether.__savedReplies.at(-1));
}

test.describe('Connection', () => {
  test.beforeEach(async ({ page }) => {
    await page.waitForSelector('.app-root-controls', { state: 'attached' });
    await page.evaluate(() => {
      const game = (globalThis as any).skyrimtogether;
      game.__savedReplies = [];
      game.__connectCalls = [];
      game.on('savedServers', (text: string) => game.__savedReplies.push(JSON.parse(text)));
      const directConnect = game.connect.bind(game);
      game.connect = (host, port, password) => {
        game.__connectCalls.push({ host, port, password });
        directConnect(host, port, password);
      };
    });
    await page.press('body', 'F2');
    await page.locator('.app-root-menu').getByRole('button', { name: /Connect to a server/ }).click();
    await expect(page.locator('app-connect')).toBeVisible();
  });

  test('invalid joins keep the specific server error', async ({ page }) => {
    const cases = [
      ['t-port', '', /Could not connect to the specified server/],
      ['t-host', '', /Could not connect to the specified server/],
      ['t-full', '', /This server is full/],
      ['t-password', 'wrong', /The password you entered is incorrect/],
      ['t-version', '', /\[expected-version].*\[current-version]/s],
      ['t-client-mods', '', /SKSE/],
      ['t-mods', '', /remove\.esp\s*remove_2\.esp/],
    ] as const;
    for (const [address, password, error] of cases) {
      await connect(page, address, password);
      await expect(page.locator('app-error')).toHaveText(error);
      await page.locator('app-error app-action-buttons button').first().click();
      await expect(page.getByRole('textbox', { name: 'Address', exact: true })).toBeVisible();
    }
  });

  test('manual Connect remembers only in the native list and Rejoin uses the same ID', async ({ page }) => {
    await page.getByRole('textbox', { name: 'Server name (optional)', exact: true }).fill('Friends');
    await connect(page, 't-password', 'test', true);
    await expect(page.locator('.app-root-disconnect')).toBeVisible();
    await expect.poll(async () => (await savedState(page))?.lastUsed).toBeGreaterThan(0);
    const before = await savedState(page);
    expect(before.servers).toHaveLength(1);
    expect(before.servers[0]).toMatchObject({ name: 'Friends', address: 't-password', hasPassword: true });
    expect(before.lastUsed).toBe(before.servers[0].id);
    expect(before.servers[0]).not.toHaveProperty('password');
    expect(await page.evaluate(() => localStorage.getItem('last_connected_password'))).toBeNull();
    expect(await page.evaluate(() => localStorage.getItem('last_connected_address'))).toBeNull();

    await disconnect(page);
    await page.locator('.app-root-menu').getByRole('button', { name: /Rejoin/ }).click();
    await expect(page.locator('.app-root-disconnect')).toBeVisible();
    await expect.poll(() => page.evaluate(() => (globalThis as any).skyrimtogether.__connectCalls.length)).toBe(2);
    expect(await page.evaluate(() => (globalThis as any).skyrimtogether.__connectCalls.at(-1))).toEqual({ host: 't-password', port: 10578, password: 'test' });
    const after = await savedState(page);
    expect(after.servers).toHaveLength(1);
    expect(after.servers[0].id).toBe(before.servers[0].id);
  });

  test('unchecked password is ephemeral and Rejoin asks through the same form', async ({ page }) => {
    await connect(page, 't-password', 'test');
    await expect(page.locator('.app-root-disconnect')).toBeVisible();
    await expect.poll(async () => (await savedState(page))?.lastUsed).toBeGreaterThan(0);
    const before = await savedState(page);
    expect(before.servers[0].hasPassword).toBe(false);
    expect(before.servers[0]).not.toHaveProperty('password');
    expect(await page.evaluate(() => (globalThis as any).skyrimtogether.savedServerList[0].password)).toBe('');
    await disconnect(page);
    await page.locator('.app-root-menu').getByRole('button', { name: /Rejoin/ }).click();
    await expect(page.getByRole('textbox', { name: 'Address', exact: true })).toHaveValue('t-password');
    await expect(page.locator('app-connect input[aria-label="Password"]')).toHaveValue('');
    await expect(page.locator('#save-password')).not.toBeChecked();
    await connect(page, 't-password', 'test');
    await expect(page.locator('.app-root-disconnect')).toBeVisible();
    const after = await savedState(page);
    expect(after.servers).toHaveLength(1);
    expect(after.servers[0].id).toBe(before.servers[0].id);
  });

  test('Save blocks Connect and Enter until its delayed acknowledgement', async ({ page }) => {
    await page.evaluate(() => {
      const game = (globalThis as any).skyrimtogether;
      const save = game.saveServer.bind(game);
      game.saveServer = (...args) => { game.__finishSave = () => { game.saveServer = save; save(...args); }; };
    });
    const address = page.getByRole('textbox', { name: 'Address', exact: true });
    await address.fill('t-password');
    await page.getByRole('button', { name: 'Save server', exact: true }).click();
    const connectButton = page.locator('app-connect app-action-buttons').getByRole('button', { name: 'Connect', exact: true });
    await expect(connectButton).toBeDisabled();
    await address.press('Enter');
    await expect(page.locator('app-connecting')).toHaveCount(0);
    expect(await page.evaluate(() => (globalThis as any).skyrimtogether.__connectCalls)).toHaveLength(0);
    await page.evaluate(() => (globalThis as any).skyrimtogether.__finishSave());
    await expect(connectButton).toBeEnabled();
    await connect(page, 't-password', 'test');
    await expect(page.locator('.app-root-disconnect')).toBeVisible();
    expect(await page.evaluate(() => (globalThis as any).skyrimtogether.__connectCalls)).toHaveLength(1);
  });

  test('an unwritable list preserves an unchecked direct join', async ({ page }) => {
    await page.evaluate(() => {
      const game = (globalThis as any).skyrimtogether;
      game.saveServer = (...args) => game.emit('savedServers', JSON.stringify({ request: args[5], servers: [], lastUsed: 0, error: 'could not write servers.txt' }));
      game.__hud = [];
      game.showHudMessage = text => game.__hud.push(text);
    });
    await connect(page, 't-password', 'test');
    await expect(page.locator('.app-root-disconnect')).toBeVisible();
    expect(await page.evaluate(() => (globalThis as any).skyrimtogether.__hud.join(''))).toContain('could not write servers.txt');
    expect(await page.evaluate(() => (globalThis as any).skyrimtogether.__connectCalls)).toEqual([{ host: 't-password', port: 10578, password: 'test' }]);
  });

  test('a full list still allows an unchecked direct join', async ({ page }) => {
    await page.evaluate(() => {
      const game = (globalThis as any).skyrimtogether;
      const servers = Array.from({ length: 64 }, (_, i) => ({ id: i + 1, name: 'Existing ' + i, address: 'existing-' + i, hasPassword: false }));
      game.saveServer = (...args) => game.emit('savedServers', JSON.stringify({ request: args[5], servers, lastUsed: 0, error: 'check the server fields or free a server slot' }));
      game.showHudMessage = () => {};
    });
    await connect(page, 't-password', 'test');
    await expect(page.locator('.app-root-disconnect')).toBeVisible();
    expect(await page.evaluate(() => (globalThis as any).skyrimtogether.__connectCalls)).toEqual([{ host: 't-password', port: 10578, password: 'test' }]);
  });

  test('an obsolete saved ID cannot turn a failed edit into a direct join', async ({ page }) => {
    await page.evaluate(() => {
      const game = (globalThis as any).skyrimtogether;
      game.saveServer(0, 'Friends', 't-password', 'test', true);
    });
    await page.getByRole('button', { name: 'Edit Friends', exact: true }).click();
    await page.evaluate(() => {
      const game = (globalThis as any).skyrimtogether;
      game.saveServer = (...args) => {
        // A refresh containing the old row can arrive before its failed edit is acknowledged.
        game.getSavedServers();
        game.emit('savedServers', JSON.stringify({ request: args[5], servers: [], lastUsed: 0, error: 'check the server fields or free a server slot' }));
      };
    });
    await connect(page, 't-password', 'test');
    await expect(page.locator('app-connect [role="alert"]')).toContainText('check the server fields');
    expect(await page.evaluate(() => (globalThis as any).skyrimtogether.__connectCalls)).toHaveLength(0);
  });
});
