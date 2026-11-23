import { createTest, expect } from '@ngx-playwright/test';
import { ApplicationScreen } from '../screens/main-screen.js';

const test = createTest(ApplicationScreen);

test('the page edits and joins the native saved list without reading passwords', async ({ page }) => {
  await page.waitForSelector('.app-root-controls', { state: 'attached' });
  await page.evaluate(() => {
    const game = (globalThis as any).skyrimtogether;
    game.__savedReplies = [];
    game.on('savedServers', (text: string) => game.__savedReplies.push(text));
    game.saveServer(0, 'Friends', 'friends.example:10578', 'MiXeD secret', true);
    const connect = game.connect.bind(game);
    game.connect = (host, port, password) => {
      game.__lastConnect = { host, port, password };
      connect(host, port, password);
    };
  });
  await page.press('body', 'F2');
  await page.getByRole('button', { name: /Connect to a server/ }).click();
  await page.getByRole('button', { name: 'Edit Friends', exact: true }).click();
  await expect(page.locator('app-connect input[type="password"]')).toHaveValue('');
  await page.getByRole('textbox', { name: 'Server name (optional)', exact: true }).fill('Renamed');
  await page.getByRole('button', { name: 'Update saved server', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Edit Renamed', exact: true })).toBeVisible();
  await page.getByRole('button', { name: 'Renamed', exact: true }).click();
  await expect.poll(() => page.evaluate(() => (globalThis as any).skyrimtogether.__lastConnect)).toEqual({
    host: 'friends.example', port: 10578, password: 'MiXeD secret',
  });
  const replies = await page.evaluate(() => (globalThis as any).skyrimtogether.__savedReplies);
  expect(replies.join('')).not.toContain('MiXeD secret');
});
