#pragma once

#include <CrimeReports.h>

#include <chrono>

struct World;
struct TransportService;
struct UpdateEvent;
struct NotifyCrimeReport;

/**
 * @brief A crime in one game reaches the games that run its witnesses, and their NPCs react to it as the game would.
 *
 * This game's player's crime that NPC copies here saw (DetectionService said "seen") is reported once, with those copies as
 * the witnesses. In the game that runs a witness, an assault or a murder runs the native assault alarm against that copy.
 * The theft alarm and the pickpocket alarm refuse a copy, so a witness that saw a theft calls it out (the "Steal" line) and does not fight,
 * and one that saw a pickpocketing fights or flees.
 * Guards leave these crimes to the offender's own game. The receiving game's player is never substituted as the offender.
 */
struct CrimeService
{
    CrimeService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher);
    ~CrimeService() noexcept = default;

    TP_NOCOPYMOVE(CrimeService);

    //! This game's player committed a crime, and these NPC copies saw it (the game's crime alarms). Form ids, 0 for none. Any thread.
    static void OnLocalCrime(
        CrimeReports::Kind aKind, uint32_t aVictimId, uint32_t aRefId, uint32_t aObjectId, int32_t aNum, int32_t aTotal, uint32_t aOwnerId, const uint32_t* apWitnesses,
        size_t aWitnessCount) noexcept;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnNotifyCrimeReport(const NotifyCrimeReport& acMessage) noexcept;

private:
    World& m_world;
    TransportService& m_transport;

    //! When this game last reported a crime against each owner or victim: emptying a shelf gives the same reaction once a second.
    TiltedPhoques::Map<uint64_t, std::chrono::steady_clock::time_point> m_reportedAt;
    //! When each of this game's NPCs last reacted to a copy for a theft or pickpocketing it saw.
    TiltedPhoques::Map<uint32_t, std::chrono::steady_clock::time_point> m_turnedAt;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_notifyConnection;
};
