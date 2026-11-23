#pragma once

namespace WorldCommandRules
{
// Inputs must describe the authenticated sender; no packet-supplied player id participates.
constexpr bool CanSetTime(bool aAuthenticatedAdmin, bool aPartyLeader, bool aPublicServer) noexcept
{
    return aAuthenticatedAdmin || (aPartyLeader && !aPublicServer);
}
} // namespace WorldCommandRules
