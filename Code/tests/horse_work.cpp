#include <catch2/catch.hpp>

#include <MountSync.h>

#include <map>
#include <vector>

namespace
{
using CopyWork::Verdict;
using MountSync::HorseJob;

constexpr uint32_t kHorse = 0xFF000D42;
constexpr uint32_t kPlayer = 0x7;
constexpr uint32_t kStableOwner = 0x1A2B3;

// The horse work of one game, in MountService's order and over its own bookkeeping (MountSync::HorseOwners and RideClaims, the
// CopyWork tickets): what the update decides (Ask, Update, Disconnect, QueueShare, QueueUnshare) and the main thread does at the
// start of a frame (Drain), with the game as the main thread finds it. Only the native calls are stood in for.
struct HorseBook
{
    struct Item
    {
        HorseJob What{};
        uint32_t Horse{};
        MountSync::HorseStamp Stamp{};
        double Until{};
    };

    // The game as the main thread finds it.
    double Now = 100.0;
    uint32_t Session = 1;
    uint32_t LoadingScreens = 0;
    bool Loading = false;
    bool InParty = true;
    std::map<uint32_t, uint32_t> Handles; // horse form id -> handle of the actor with that form id now
    std::map<uint32_t, bool> RunHere;     // this game runs it (handed over)
    std::map<uint32_t, uint32_t> Owner;   // the horse's actor owner here now
    std::map<uint32_t, bool> CopyOn;      // another game's rider copy is still on it here

    CopyWork::Tickets Shares;
    CopyWork::Tickets Rides;
    MountSync::HorseOwners Owners;
    MountSync::RideClaims Claims;
    std::vector<Item> ShareQueue;
    std::vector<Item> RideQueue;
    std::vector<std::pair<uint32_t, uint32_t>> SharesDone;
    std::vector<std::pair<uint32_t, uint32_t>> RidesDone;

    // What the main thread did.
    std::vector<uint32_t> Ridden;
    std::vector<uint32_t> Shared;
    std::vector<uint32_t> OwnersPutBack;
    std::vector<uint32_t> LetOff;

    uint32_t HandleOf(uint32_t aHorse) const { return Handles.count(aHorse) ? Handles.at(aHorse) : 0; }

    Item Stamp(HorseJob aWhat, uint32_t aHorse, uint32_t aTicket, uint32_t aUndoes = 0) const
    {
        Item item{aWhat, aHorse};
        item.Stamp.Work.Ticket = aTicket;
        item.Stamp.Work.Undoes = aUndoes;
        item.Stamp.Work.LoadingScreens = LoadingScreens;
        item.Stamp.Session = Session;
        item.Stamp.Handle = HandleOf(aHorse);
        return item;
    }

    // UpdateSharedHorses.
    uint32_t QueueShare(uint32_t aHorse)
    {
        const uint32_t cShare = Shares.Issue(aHorse);
        ShareQueue.push_back(Stamp(HorseJob::Share, aHorse, cShare));
        return cShare;
    }

    // QueueUnshare.
    void QueueUnshare(uint32_t aHorse, uint32_t aHandle, uint32_t aShare)
    {
        Shares.Retire(aHorse);
        Item item = Stamp(HorseJob::Unshare, aHorse, 0, aShare);
        item.Stamp.Handle = aHandle;
        ShareQueue.push_back(item);
    }

    // OnActivate: the claim keeps what it was asked under.
    void Ask(uint32_t aHorse)
    {
        MountSync::RideClaim claim{};
        claim.Until = Now + 3.0;
        claim.Stamp.Work.Ticket = Rides.Issue(aHorse);
        claim.Stamp.Work.LoadingScreens = LoadingScreens;
        claim.Stamp.Session = Session;
        claim.Stamp.Handle = HandleOf(aHorse);
        Claims.Ask(aHorse, claim);
    }

    // UpdateRideClaims.
    void Update()
    {
        for (const auto& [cHorse, cTicket] : RidesDone)
            if (Claims.Spent(cHorse, cTicket))
                Rides.Retire(cHorse);
        RidesDone.clear();
        Claims.Update(
            Now, InParty,
            [this](uint32_t aHorse)
            {
                // LetCopiesOff: a copy left on the horse is let off at the start of the next frame.
                const bool cCopyLeftOn = RunHere[aHorse] && CopyOn[aHorse];
                if (cCopyLeftOn)
                {
                    CopyOn[aHorse] = false;
                    LetOff.push_back(aHorse);
                }
                return MountSync::RideReady(RunHere[aHorse], cCopyLeftOn);
            },
            [this](uint32_t aHorse, const MountSync::RideClaim& acClaim)
            {
                Item item{HorseJob::Ride, aHorse, acClaim.Stamp, acClaim.Until};
                RideQueue.push_back(item);
            },
            [this](uint32_t aHorse) { Rides.Retire(aHorse); });
    }

    // OnDisconnected.
    void Disconnect()
    {
        ++Session;
        RidesDone.clear();
        Claims.Clear([this](uint32_t aHorse) { Rides.Retire(aHorse); });
    }

    void Load()
    {
        ++LoadingScreens;
        Loading = true;
    }

    // RunQueuedWork: shares, owed owners, rides. aWaitedMs is how long the work has waited by now.
    void Drain(uint64_t aWaitedMs = 0)
    {
        Run(ShareQueue, Shares, SharesDone, aWaitedMs);
        if (!Loading)
            Owners.Settle(
                [this](uint32_t aHorse, const MountSync::OwnerBefore& acBefore)
                {
                    if (HandleOf(aHorse) == acBefore.Handle)
                        PutBack(aHorse, acBefore);
                });
        Run(RideQueue, Rides, RidesDone, aWaitedMs);
    }

    // The character save of a horse: the owner written to it, and the owner it has here after the save (ShareSaveGuard).
    std::pair<uint32_t, uint32_t> Save(uint32_t aHorse)
    {
        const MountSync::OwnerBefore* pBefore = Owners.Find(aHorse);
        const uint32_t cHandle = HandleOf(aHorse);
        if (!MountSync::GuardLifts(pBefore != nullptr, pBefore ? MountSync::SharedOwner{pBefore->Handle, pBefore->Share} : MountSync::SharedOwner{}, cHandle))
            return {Owner[aHorse], Owner[aHorse]};
        const MountSync::SharedOwner cLifted{pBefore->Handle, pBefore->Share};
        const uint32_t cSaved = pBefore->ActorOwner;
        const MountSync::OwnerBefore* pAfter = Owners.Find(aHorse);
        const bool cRestores =
            MountSync::GuardRestores(pAfter != nullptr, pAfter ? MountSync::SharedOwner{pAfter->Handle, pAfter->Share} : MountSync::SharedOwner{}, cLifted, cHandle);
        return {cSaved, cRestores ? kPlayer : cSaved};
    }

private:
    void PutBack(uint32_t aHorse, const MountSync::OwnerBefore& acBefore)
    {
        Owner[aHorse] = acBefore.ActorOwner;
        OwnersPutBack.push_back(aHorse);
    }

    void Run(std::vector<Item>& aQueue, CopyWork::Tickets& aTickets, std::vector<std::pair<uint32_t, uint32_t>>& aDone, uint64_t aWaitedMs)
    {
        std::vector<Item> held;
        for (const Item& cItem : aQueue)
        {
            const bool cSame = cItem.Stamp.Handle && HandleOf(cItem.Horse) == cItem.Stamp.Handle;
            MountSync::HorseFound found{};
            found.Loading = Loading;
            found.LoadingScreens = LoadingScreens;
            found.Session = Session;
            found.SameHorse = cSame;
            found.Living = cSame;
            found.IsMount = cSame;
            found.RunHere = cSame && RunHere[cItem.Horse];
            found.Live = cItem.Stamp.Work.Ticket && aTickets.Live(cItem.Horse, cItem.Stamp.Work.Ticket);
            found.WaitedMs = aWaitedMs;
            found.Expired = cItem.What == HorseJob::Ride && Now >= cItem.Until;
            found.Shared = Owners.Shared(cItem.Horse);

            const Verdict cVerdict = MountSync::CheckHorse(cItem.What, cItem.Stamp, found);
            if (cVerdict == Verdict::kHold)
            {
                held.push_back(cItem);
                continue;
            }
            switch (cItem.What)
            {
            case HorseJob::Share:
                if (cVerdict != Verdict::kDo)
                {
                    aDone.emplace_back(cItem.Horse, cItem.Stamp.Work.Ticket);
                    break;
                }
                if (Owners.CarryOn(cItem.Horse, cItem.Stamp.Handle, cItem.Stamp.Work.Ticket))
                    break;
                Owners.Record(cItem.Horse, {Owner[cItem.Horse], 0, cItem.Stamp.Handle, cItem.Stamp.Work.Ticket});
                Owner[cItem.Horse] = kPlayer;
                Shared.push_back(cItem.Horse);
                break;
            case HorseJob::Unshare:
            {
                MountSync::OwnerBefore before{};
                if (Owners.Unshare(cItem.Horse, cItem.Stamp.Work.Undoes, cVerdict, found.Loading, before) == MountSync::Undo::kPutBack)
                    PutBack(cItem.Horse, before);
                break;
            }
            case HorseJob::Ride:
                aDone.emplace_back(cItem.Horse, cItem.Stamp.Work.Ticket);
                if (cVerdict == Verdict::kDo)
                    Ridden.push_back(cItem.Horse);
                break;
            }
        }
        aQueue = held;
    }
};

HorseBook SharedHorseBook(uint32_t& aShare)
{
    HorseBook book;
    book.Handles[kHorse] = 0x100401;
    book.Owner[kHorse] = kStableOwner;
    aShare = book.QueueShare(kHorse);
    book.Drain();
    return book;
}
} // namespace

TEST_CASE("A ride queued before a disconnect is not taken after it", "[mount][horse_work]")
{
    HorseBook book;
    book.Handles[kHorse] = 0x100401;
    book.Ask(kHorse);
    book.RunHere[kHorse] = true;
    book.Update();
    REQUIRE(book.RideQueue.size() == 1);
    book.Disconnect();
    book.Drain();
    REQUIRE(book.Ridden.empty());
    REQUIRE(book.RideQueue.empty());

    // The same claim in the same connection is taken, and spent once the main thread says so.
    HorseBook kept;
    kept.Handles[kHorse] = 0x100401;
    kept.Ask(kHorse);
    kept.RunHere[kHorse] = true;
    kept.Update();
    const uint32_t cTicket = kept.Claims.Find(kHorse)->Stamp.Work.Ticket;
    kept.Drain();
    REQUIRE(kept.Ridden.size() == 1);
    REQUIRE(kept.Claims.Find(kHorse) != nullptr);
    kept.Update();
    REQUIRE(kept.Claims.Empty());
    REQUIRE_FALSE(kept.Rides.Live(kHorse, cTicket));
}

TEST_CASE("A ride queued for a claim is dropped when the player leaves the party before it is done", "[mount][horse_work]")
{
    HorseBook book;
    book.Handles[kHorse] = 0x100401;
    book.Ask(kHorse);
    // Handed over: the Ride is queued and the claim kept.
    book.RunHere[kHorse] = true;
    book.Update();
    REQUIRE(book.RideQueue.size() == 1);
    REQUIRE(book.Claims.Find(kHorse) != nullptr);
    REQUIRE(book.Claims.Find(kHorse)->Queued);

    // The party ends before the main thread gets to it: the update withdraws the queued claim.
    book.InParty = false;
    book.Update();
    REQUIRE(book.Claims.Empty());
    book.Drain();
    REQUIRE(book.Ridden.empty());
    REQUIRE(book.RideQueue.empty());
    // The main thread's word on the dropped Ride finds nothing to spend.
    book.Update();
    REQUIRE(book.Claims.Empty());

    // Turned off (or out of the party) with the Ride queued again on a later claim: the same.
    book.InParty = true;
    book.Ask(kHorse);
    book.Update();
    REQUIRE(book.RideQueue.size() == 1);
    book.InParty = false;
    book.Update();
    book.Drain();
    REQUIRE(book.Ridden.empty());
}

TEST_CASE("A claim whose time is up, or with a load since it was asked, gets no horse handed over late", "[mount][horse_work]")
{
    // The handover comes after the claim's time: nothing is queued, the ticket is withdrawn.
    HorseBook late;
    late.Handles[kHorse] = 0x100401;
    late.Ask(kHorse);
    const uint32_t cTicket = late.Claims.Find(kHorse)->Stamp.Work.Ticket;
    late.Now += 2.0;
    late.Update();
    REQUIRE(late.RideQueue.empty());
    late.Now += 1.5;
    late.RunHere[kHorse] = true;
    late.Update();
    REQUIRE(late.RideQueue.empty());
    REQUIRE(late.Claims.Empty());
    REQUIRE_FALSE(late.Rides.Live(kHorse, cTicket));
    late.Drain();
    REQUIRE(late.Ridden.empty());

    // Queued in time, but the main thread only gets to it after the claim's time: its time went with it.
    HorseBook slow;
    slow.Handles[kHorse] = 0x100401;
    slow.Ask(kHorse);
    slow.RunHere[kHorse] = true;
    slow.Update();
    REQUIRE(slow.RideQueue.size() == 1);
    slow.Now += 3.5;
    slow.Drain();
    REQUIRE(slow.Ridden.empty());

    // A load between the ask and the handover: the Ride keeps the ask's count and is dropped, and its claim is spent.
    HorseBook loaded;
    loaded.Handles[kHorse] = 0x100401;
    loaded.Ask(kHorse);
    loaded.Load();
    loaded.Loading = false;
    loaded.RunHere[kHorse] = true;
    loaded.Update();
    REQUIRE(loaded.RideQueue.size() == 1);
    loaded.Drain();
    REQUIRE(loaded.Ridden.empty());
    loaded.Update();
    REQUIRE(loaded.Claims.Empty());

    // The load made another actor under the same form id: the ask's handle names no such horse.
    HorseBook replaced;
    replaced.Handles[kHorse] = 0x100401;
    replaced.Ask(kHorse);
    replaced.Handles[kHorse] = 0x200777;
    replaced.RunHere[kHorse] = true;
    replaced.Update();
    replaced.Drain();
    REQUIRE(replaced.Ridden.empty());
}

TEST_CASE("A newer claim on the same horse is not spent by the older one's Ride", "[mount][horse_work]")
{
    HorseBook book;
    book.Handles[kHorse] = 0x100401;
    book.Ask(kHorse);
    book.RunHere[kHorse] = true;
    book.Update();
    // Asked again before the first Ride ran: the first is withdrawn, the second waits.
    book.Ask(kHorse);
    book.Drain();
    REQUIRE(book.Ridden.empty());
    book.Update();
    REQUIRE(book.Claims.Find(kHorse) != nullptr);
    REQUIRE(book.RideQueue.size() == 1);
    book.Drain();
    REQUIRE(book.Ridden.size() == 1);
}

TEST_CASE("A share queued before a disconnect is dropped, and its undoing then finds nothing to put back", "[mount][horse_work]")
{
    HorseBook book;
    book.Handles[kHorse] = 0x100401;
    book.Owner[kHorse] = kStableOwner;
    const uint32_t cShare = book.QueueShare(kHorse);
    // The disconnect withdraws the share and asks for the owner back.
    book.Disconnect();
    book.QueueUnshare(kHorse, 0x100401, cShare);
    book.Drain();
    REQUIRE(book.Shared.empty());
    REQUIRE(book.OwnersPutBack.empty());
    REQUIRE(book.Owners.Find(kHorse) == nullptr);
    REQUIRE(book.Owner[kHorse] == kStableOwner);
}

TEST_CASE("A share in place is still taken back after the disconnect", "[mount][horse_work]")
{
    uint32_t share{};
    HorseBook book = SharedHorseBook(share);
    REQUIRE(book.Shared.size() == 1);
    REQUIRE(book.Owner[kHorse] == kPlayer);
    book.Disconnect();
    book.QueueUnshare(kHorse, 0x100401, share);
    book.Drain();
    REQUIRE(book.OwnersPutBack.size() == 1);
    REQUIRE(book.Owner[kHorse] == kStableOwner);
    REQUIRE(book.Owners.Find(kHorse) == nullptr);
}

TEST_CASE("Share work queued before a load does not act on the horse the load made under the same form id", "[mount][horse_work]")
{
    HorseBook book;
    book.Handles[kHorse] = 0x100401;
    book.QueueShare(kHorse);

    // The loading screen is still up: nothing is done, nothing waits for it.
    book.Load();
    book.Drain();
    REQUIRE(book.ShareQueue.empty());
    REQUIRE(book.Shared.empty());
    // The dropped share comes back to the update.
    REQUIRE(book.SharesDone.size() == 1);
}

TEST_CASE("A share in place on a horse a load replaced is forgotten, not put back on the new one", "[mount][horse_work]")
{
    uint32_t share{};
    HorseBook book = SharedHorseBook(share);
    REQUIRE(book.Owners.Find(kHorse) != nullptr);

    book.Load();
    book.Loading = false;
    book.Handles[kHorse] = 0x200777;
    book.Owner[kHorse] = 0x99;
    book.QueueUnshare(kHorse, 0x100401, share);
    book.Drain();
    REQUIRE(book.OwnersPutBack.empty());
    REQUIRE(book.Owners.Find(kHorse) == nullptr);
    REQUIRE(book.Owner[kHorse] == 0x99);

    // An undoing waits out a short loading screen, as copies' undoing does.
    uint32_t waitShare{};
    HorseBook waits = SharedHorseBook(waitShare);
    waits.QueueUnshare(kHorse, 0x100401, waitShare);
    waits.Loading = true;
    waits.Drain(1000);
    REQUIRE(waits.ShareQueue.size() == 1);
    waits.Loading = false;
    waits.Drain(1500);
    REQUIRE(waits.OwnersPutBack.size() == 1);
    REQUIRE(waits.Owner[kHorse] == kStableOwner);
}

TEST_CASE("An unshare a long loading screen outlasts keeps the owner owed, and saves masked, until the same horse is back", "[mount][horse_work]")
{
    uint32_t share{};
    HorseBook book = SharedHorseBook(share);
    REQUIRE(book.Owner[kHorse] == kPlayer);

    // The party member left: the share is to go. A loading screen comes up first and stays past the wait.
    book.QueueUnshare(kHorse, 0x100401, share);
    book.Load();
    book.Drain(1000);
    REQUIRE(book.ShareQueue.size() == 1);
    book.Drain(CopyWork::kHoldUndoMs + 1000);
    REQUIRE(book.ShareQueue.empty());
    REQUIRE(book.OwnersPutBack.empty());

    // The record stays, owed; the horse is still this player's here, and a save writes the owner it had.
    const MountSync::OwnerBefore* pOwed = book.Owners.Find(kHorse);
    REQUIRE(pOwed != nullptr);
    REQUIRE(pOwed->Owed);
    REQUIRE(pOwed->ActorOwner == kStableOwner);
    REQUIRE(book.Owner[kHorse] == kPlayer);
    REQUIRE(book.Save(kHorse) == std::pair<uint32_t, uint32_t>{kStableOwner, kPlayer});

    // Still loading: nothing is settled.
    book.Drain();
    REQUIRE(book.Owners.Find(kHorse) != nullptr);
    REQUIRE(book.Save(kHorse) == std::pair<uint32_t, uint32_t>{kStableOwner, kPlayer});

    // The loading screen goes and the same horse (same handle) is there: the owner goes back, and the record with it.
    book.Loading = false;
    book.Drain();
    REQUIRE(book.OwnersPutBack.size() == 1);
    REQUIRE(book.Owner[kHorse] == kStableOwner);
    REQUIRE(book.Owners.Find(kHorse) == nullptr);
    REQUIRE(book.Save(kHorse) == std::pair<uint32_t, uint32_t>{kStableOwner, kStableOwner});
}

TEST_CASE("An owed owner is forgotten when the load replaced its horse, and paid off by a new share on it", "[mount][horse_work]")
{
    // The load made another actor under the form id: nothing is put back on it.
    uint32_t share{};
    HorseBook replaced = SharedHorseBook(share);
    replaced.QueueUnshare(kHorse, 0x100401, share);
    replaced.Load();
    replaced.Drain(CopyWork::kHoldUndoMs + 1000);
    REQUIRE(replaced.Owners.Find(kHorse) != nullptr);
    replaced.Loading = false;
    replaced.Handles[kHorse] = 0x200777;
    replaced.Owner[kHorse] = 0x99;
    replaced.Drain();
    REQUIRE(replaced.OwnersPutBack.empty());
    REQUIRE(replaced.Owners.Find(kHorse) == nullptr);
    REQUIRE(replaced.Owner[kHorse] == 0x99);

    // The party member rides it again before the load ends: the new share carries the record on, and nothing is owed.
    uint32_t again{};
    HorseBook shared = SharedHorseBook(again);
    shared.QueueUnshare(kHorse, 0x100401, again);
    shared.Load();
    shared.Drain(CopyWork::kHoldUndoMs + 1000);
    shared.Loading = false;
    const uint32_t cNewShare = shared.QueueShare(kHorse);
    shared.Drain();
    REQUIRE(shared.OwnersPutBack.empty());
    REQUIRE(shared.Owner[kHorse] == kPlayer);
    const MountSync::OwnerBefore* pRecord = shared.Owners.Find(kHorse);
    REQUIRE(pRecord != nullptr);
    REQUIRE_FALSE(pRecord->Owed);
    REQUIRE(pRecord->Share == cNewShare);
    REQUIRE(pRecord->ActorOwner == kStableOwner);
    REQUIRE(shared.Save(kHorse) == std::pair<uint32_t, uint32_t>{kStableOwner, kPlayer});
}

TEST_CASE("The unshare record rules: a later share's record stays, a gone horse's goes", "[mount][horse_work]")
{
    MountSync::HorseOwners owners;
    MountSync::OwnerBefore out{};
    owners.Record(kHorse, {kStableOwner, 0, 0x100401, 5});
    // An undo of an older share leaves the later one's record alone, whatever its verdict.
    REQUIRE(owners.Unshare(kHorse, 4, Verdict::kDrop, true, out) == MountSync::Undo::kNothing);
    REQUIRE(owners.Unshare(kHorse, 4, Verdict::kDrop, false, out) == MountSync::Undo::kNothing);
    REQUIRE_FALSE(owners.Find(kHorse)->Owed);
    // Held: not yet.
    REQUIRE(owners.Unshare(kHorse, 5, Verdict::kHold, true, out) == MountSync::Undo::kNothing);
    // Dropped with no loading screen up (its horse gone): forgotten.
    REQUIRE(owners.Unshare(kHorse, 5, Verdict::kDrop, false, out) == MountSync::Undo::kForget);
    REQUIRE(owners.Find(kHorse) == nullptr);
    // A record for another horse with the form id is replaced by a new share's.
    owners.Record(kHorse, {kStableOwner, 0, 0x100401, 6});
    REQUIRE_FALSE(owners.CarryOn(kHorse, 0x200777, 7));
    REQUIRE(owners.Find(kHorse) == nullptr);
}

TEST_CASE("Horse work is checked for the horse itself: a mount, alive, and for a ride run here", "[mount][horse_work]")
{
    MountSync::HorseStamp stamp{};
    stamp.Work.Ticket = 4;
    stamp.Session = 2;
    stamp.Handle = 0x100401;
    MountSync::HorseFound found{};
    found.Session = 2;
    found.SameHorse = true;
    found.Living = true;
    found.IsMount = true;
    found.RunHere = true;
    found.Live = true;
    REQUIRE(MountSync::CheckHorse(HorseJob::Ride, stamp, found) == Verdict::kDo);
    REQUIRE(MountSync::CheckHorse(HorseJob::Share, stamp, found) == Verdict::kDo);

    auto notMount = found;
    notMount.IsMount = false;
    REQUIRE(MountSync::CheckHorse(HorseJob::Ride, stamp, notMount) == Verdict::kDrop);
    REQUIRE(MountSync::CheckHorse(HorseJob::Share, stamp, notMount) == Verdict::kDrop);
    auto copy = found;
    copy.RunHere = false;
    REQUIRE(MountSync::CheckHorse(HorseJob::Ride, stamp, copy) == Verdict::kDrop);
    // A shared horse is usually a party member's copy here.
    REQUIRE(MountSync::CheckHorse(HorseJob::Share, stamp, copy) == Verdict::kDo);
    auto withdrawn = found;
    withdrawn.Live = false;
    REQUIRE(MountSync::CheckHorse(HorseJob::Ride, stamp, withdrawn) == Verdict::kDrop);
    auto dead = found;
    dead.Living = false;
    REQUIRE(MountSync::CheckHorse(HorseJob::Ride, stamp, dead) == Verdict::kDrop);
    auto exiting = found;
    exiting.Exiting = true;
    REQUIRE(MountSync::CheckHorse(HorseJob::Ride, stamp, exiting) == Verdict::kDrop);
    auto expired = found;
    expired.Expired = true;
    REQUIRE(MountSync::CheckHorse(HorseJob::Ride, stamp, expired) == Verdict::kDrop);
    // Only a ride's claim runs out.
    REQUIRE(MountSync::CheckHorse(HorseJob::Share, stamp, expired) == Verdict::kDo);

    // An undoing names the share it takes back, and is no claim.
    MountSync::HorseStamp undo{};
    undo.Work.Undoes = 4;
    auto inPlace = found;
    inPlace.Shared = 4;
    REQUIRE(MountSync::CheckHorse(HorseJob::Unshare, undo, inPlace) == Verdict::kDo);
    inPlace.Shared = 5;
    REQUIRE(MountSync::CheckHorse(HorseJob::Unshare, undo, inPlace) == Verdict::kDrop);
    auto ticketed = undo;
    ticketed.Work.Ticket = 4;
    inPlace.Shared = 4;
    REQUIRE(MountSync::CheckHorse(HorseJob::Unshare, ticketed, inPlace) == Verdict::kDrop);
}

TEST_CASE("A share withdrawn before it is made is not made, and its undoing puts nothing back", "[mount][horse_work]")
{
    HorseBook book;
    book.Handles[kHorse] = 0x100401;
    book.Owner[kHorse] = kStableOwner;
    const uint32_t cShare = book.QueueShare(kHorse);
    // The party member left: the share goes before it is made.
    book.QueueUnshare(kHorse, 0x100401, cShare);
    book.Drain();
    REQUIRE(book.Shared.empty());
    REQUIRE(book.OwnersPutBack.empty());
    REQUIRE(book.Owner[kHorse] == kStableOwner);
}

TEST_CASE("A save lifts a share only off its own horse, and gives back only the share still in place", "[mount][horse_work]")
{
    const MountSync::SharedOwner cRecord{0x100401, 7};
    REQUIRE(MountSync::GuardLifts(true, cRecord, 0x100401));
    // A horse that has the form id now but is not the one shared (a recycled form id) is saved as it is.
    REQUIRE_FALSE(MountSync::GuardLifts(true, cRecord, 0x200777));
    REQUIRE_FALSE(MountSync::GuardLifts(false, cRecord, 0x100401));
    REQUIRE_FALSE(MountSync::GuardLifts(true, cRecord, 0));
    REQUIRE_FALSE(MountSync::GuardLifts(true, {0x100401, 0}, 0x100401));

    // Unchanged over the save: the share is given back.
    REQUIRE(MountSync::GuardRestores(true, cRecord, cRecord, 0x100401));
    // Taken back while the save ran: it stays taken back.
    REQUIRE_FALSE(MountSync::GuardRestores(false, {}, cRecord, 0x100401));
    // Taken back and made again meanwhile: the new share set its own owner.
    REQUIRE_FALSE(MountSync::GuardRestores(true, {0x100401, 8}, cRecord, 0x100401));
    // Replaced by a record for another horse with that form id.
    REQUIRE_FALSE(MountSync::GuardRestores(true, {0x200777, 7}, cRecord, 0x100401));
    REQUIRE_FALSE(MountSync::GuardRestores(true, cRecord, cRecord, 0x200777));
}

TEST_CASE("A dismount before the rider's copy spawns cancels the word to get on that waits for it", "[mount][horse_work]")
{
    constexpr uint32_t cRider = 0x31;
    constexpr uint32_t cHorseId = 0x32;
    // Words waiting for their copies, by rider server id (MountService::m_pendingWords).
    std::map<uint32_t, MountSync::Identity> pending;
    std::map<uint32_t, uint32_t> seated;
    const auto cDismount = [&](uint32_t aRiderEpoch, uint32_t aMount)
    {
        if (const auto it = pending.find(cRider); it != pending.end() && MountSync::DismountCancels(it->second, cRider, aRiderEpoch, aMount))
            pending.erase(it);
    };
    const auto cSpawn = [&](uint32_t aRiderEpochHere)
    {
        for (auto it = pending.begin(); it != pending.end();)
        {
            if (MountSync::Current(it->second, aRiderEpochHere, it->second.MountEpoch))
            {
                seated[it->first] = it->second.Mount;
                it = pending.erase(it);
            }
            else
                ++it;
        }
    };

    // Mount word, then its dismount, both before the rider's copy: the copy spawns standing.
    pending[cRider] = {cRider, 3, cHorseId, 5};
    cDismount(3, cHorseId);
    cSpawn(3);
    REQUIRE(seated.empty());
    REQUIRE(pending.empty());

    // The copy is here but its new epoch not yet: the dismount still cancels the word.
    pending[cRider] = {cRider, 4, cHorseId, 5};
    cDismount(4, cHorseId);
    cSpawn(4);
    REQUIRE(seated.empty());

    // An older dismount (an earlier owner of the rider, or another horse) leaves a newer ride alone.
    pending[cRider] = {cRider, 6, cHorseId, 5};
    cDismount(5, cHorseId);
    cDismount(6, 0x33);
    REQUIRE(pending.size() == 1);
    cSpawn(6);
    REQUIRE(seated.at(cRider) == cHorseId);

    // A dismount with no epoch cancels nothing.
    REQUIRE_FALSE(MountSync::DismountCancels({cRider, 0, cHorseId, 5}, cRider, 0, cHorseId));
}

TEST_CASE("A horse handed over with another game's rider copy still on it is got on once that copy is let off", "[mount][horse_work]")
{
    REQUIRE(MountSync::RideReady(true, false));
    REQUIRE_FALSE(MountSync::RideReady(true, true));
    REQUIRE_FALSE(MountSync::RideReady(false, false));

    // The rider's copy is still shown climbing off when the horse comes: it is let off first, and the ride waits for the next update.
    HorseBook book;
    book.Handles[kHorse] = 0x100401;
    book.Ask(kHorse);
    book.RunHere[kHorse] = true;
    book.CopyOn[kHorse] = true;
    book.Update();
    REQUIRE(book.LetOff.size() == 1);
    REQUIRE(book.RideQueue.empty());
    REQUIRE_FALSE(book.Claims.Find(kHorse)->Queued);
    book.Drain();
    REQUIRE(book.Ridden.empty());
    book.Update();
    REQUIRE(book.RideQueue.size() == 1);
    book.Drain();
    REQUIRE(book.Ridden.size() == 1);
    REQUIRE(book.LetOff.size() == 1);

    // Letting the copy off does not stretch the claim's time.
    HorseBook late;
    late.Handles[kHorse] = 0x100401;
    late.Ask(kHorse);
    late.RunHere[kHorse] = true;
    late.CopyOn[kHorse] = true;
    late.Update();
    late.Now += 3.0;
    late.Update();
    late.Drain();
    REQUIRE(late.Ridden.empty());
    REQUIRE(late.Claims.Empty());
}
