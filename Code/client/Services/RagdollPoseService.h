#pragma once

#include <Games/Skyrim/RagdollBodiesPose.h>
#include <GameApi.h>
#include <Games/Skyrim/CopyBodyRules.h>

struct World;
struct TransportService;
struct UpdateEvent;
struct DisconnectedEvent;
struct NotifyRagdollPose;

/**
 * @brief A carried or dragged creature bends on every screen, not only its owner's.
 *
 * The body pose speaks of a person's bones, so a body without them (a wolf, a chicken, a spider) was sent as a position only, and
 * the other games moved their copy whole, as a stiff statue. While such a body is moved, its owner's game now also sends where
 * each rigid body of its ragdoll is (RagdollBodiesPose), ten times a second. The other games set their copy's ragdoll into those
 * bodies each frame, with the speed they move at, as they set a person's (VRBodySync's carried body follow).
 */
struct RagdollPoseService
{
    RagdollPoseService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport);
    ~RagdollPoseService() noexcept = default;

    TP_NOCOPYMOVE(RagdollPoseService);

    static uint32_t Present(uint32_t aId, uint32_t aEpoch, uint64_t aTick, uint32_t aLayout, uint32_t aTotal, const UrGameBody* apBodies, uint32_t aCount);
    //! The bodies of a body this game runs, read as it is moved (VRBodySync::CaptureBodyPose): sent with the next update. Any thread.
    static void
    NoteOwnBodies(uint32_t aFormId, uint32_t aServerId, uint32_t aOwnershipEpoch, const glm::vec3& acRoot, std::vector<RagdollBodiesPose::Body> aBodies, uint32_t aLayout) noexcept;
    //! Another game sent a copy's bodies lately.
    [[nodiscard]] static bool HasOwnerBodies(uint32_t aFormId, const CopyBodyRules::CopyAuthority& acAuthority) noexcept;
    //! Where the owner's game has a copy's bodies now, for a ragdoll of aCount bodies making aLayout here: the first ones, as many as it
    //! sends. False when it sent none lately, or for another skeleton. Any thread.
    [[nodiscard]] static bool
    OwnerBodies(uint32_t aFormId, const CopyBodyRules::CopyAuthority& acAuthority, size_t aCount, uint32_t aLayout, std::vector<RagdollBodiesPose::Body>& aOut) noexcept;
    //! Where a dead creature's bodies came to rest at its owner's, sent with its spawn, wait for its copy's ragdoll and have not been
    //! handed to its settle yet (VRBodySync::PlaceCorpse). Any thread.
    [[nodiscard]] static bool HoldsRestBodies(uint32_t aFormId) noexcept;
    //! They were handed to the copy's settle: kept for it to read, and so that the same bodies sent again are not settled into twice.
    static void HandRestBodies(uint32_t aFormId) noexcept;
    //! Its owner's bodies arrive and place it, or the copy is gone: what was held is dropped.
    static void ForgetRestBodies(uint32_t aFormId) noexcept;
    //! A new session or a load (VRBodySync::ForgetAllCorpses): everything held is dropped.
    static void ForgetAllRestBodies() noexcept;
    //! The bodies held for a copy whose ragdoll has aCount bodies making aLayout here: the first ones, as many as were sent.
    //! False when none are held, or for another skeleton. Any thread.
    [[nodiscard]] static bool RestBodies(uint32_t aFormId, size_t aCount, uint32_t aLayout, std::vector<RagdollBodiesPose::Body>& aOut) noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnNotifyRagdollPose(const NotifyRagdollPose& acMessage) noexcept;

private:
    World& m_world;
    TransportService& m_transport;

    //! When this game last sent each of its bodies, by form id.
    TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> m_sentAt;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_notifyConnection;
};
