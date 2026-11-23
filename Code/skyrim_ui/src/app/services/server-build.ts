/**
 * The public server list is shared with other Skyrim Together builds. A urSovngarde server marks its announcement
 * with `urs:<protocol>` in the tags, the protocol its clients must have to join. This is the page's rule for
 * which listed servers this client can join.
 */
export const BUILD_TAG_PREFIX = 'urs:';

/**
 * Whether a listed server runs a build this client can join.
 *
 * @param tags The server's announced tags.
 * @param clientProtocol This client's protocol; empty when it is not known (the page in a browser), which shows every server.
 */
export function isJoinableBuild(
  tags: string | null | undefined,
  clientProtocol: string | null | undefined,
): boolean {
  if (!clientProtocol) {
    return true;
  }
  const wanted = `${BUILD_TAG_PREFIX}${clientProtocol}`;
  return (tags ?? '').split(/[\s,;]+/).some(tag => tag === wanted);
}
