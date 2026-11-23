#include <catch2/catch.hpp>

#include <LoginGate.h>

TEST_CASE("Only the login is read from a connection with no player", "[login_gate]")
{
    REQUIRE(LoginGate::IsReadBeforeLogin(kAuthenticationRequest));

    // Every other opcode, the last one included, is refused unread.
    for (uint64_t opcode = 0; opcode < kClientOpcodeMax; ++opcode)
        REQUIRE(LoginGate::IsReadBeforeLogin(opcode) == (opcode == kAuthenticationRequest));

    // A value no opcode has is refused too.
    REQUIRE(!LoginGate::IsReadBeforeLogin(kClientOpcodeMax));
    REQUIRE(!LoginGate::IsReadBeforeLogin(255));
}
