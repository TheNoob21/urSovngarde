// Run with: node --experimental-strip-types --test tests/server-build.test.ts (Node 22.6 or newer) from Code/skyrim_ui.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { isJoinableBuild } from '../src/app/services/server-build.ts';

test('a server with the same protocol is joinable', () => {
  assert.equal(isJoinableBuild('urs:92038558', '92038558'), true);
  assert.equal(isJoinableBuild('pvp, urs:92038558', '92038558'), true);
  assert.equal(isJoinableBuild('pvp;urs:92038558 rp', '92038558'), true);
});

test('another protocol, another mod or no tags is hidden', () => {
  assert.equal(isJoinableBuild('urs:11111111', '92038558'), false);
  assert.equal(isJoinableBuild('urs:920385581', '92038558'), false);
  assert.equal(isJoinableBuild('xurs:92038558', '92038558'), false);
  assert.equal(isJoinableBuild('', '92038558'), false);
  assert.equal(isJoinableBuild(undefined, '92038558'), false);
  assert.equal(isJoinableBuild(null, '92038558'), false);
});

test('an unknown client protocol shows every server', () => {
  assert.equal(isJoinableBuild('', ''), true);
  assert.equal(isJoinableBuild('x', undefined), true);
});
