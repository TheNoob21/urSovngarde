ScriptName urSovngardeCoop Hidden
; The co-op menu's natives. The urSovngarde client binds every function here when the game starts
; (Code/client/Games/CoopNatives.cpp), so they only do anything in a game launched through urSovngarde;
; in any other launch a native returns false, 0 or "".
;
; Connection status numbers: 0 offline, 1 connecting, 2 online, 3 waiting to try again.

; Join the server named by the player's connect.txt (or the mod's). The HUD says why it did not start.
bool Function Connect() global native

bool Function ConnectSaved(int id) global native
bool Function ConnectLastUsed() global native
; 0 when the slot is empty. -1 when the list could not be read.
int Function GetServerId(int index) global native
string Function GetServerName(int id) global native
string Function GetServerAddress(int id) global native
int Function GetLastUsedServer() global native
bool Function AddSavedServer(string address) global native
bool Function ImportSavedServer() global native
bool Function RenameSavedServer(int id, string name) global native
bool Function RemoveSavedServer(int id) global native

; Leave the server. On VR this also stops the automatic retries.
bool Function Disconnect() global native

int Function GetStatus() global native

; Connection, server name, player count and ping.
string Function GetStatusText() global native

; Another player's follower (urSovngardeFollowerEffect). Her player's game runs her; a word to her goes there.
; The player's choice: 1 wait here, 2 follow her player again; 0 asks nothing (a trade, or cancelled).
bool Function AskFollower(int command) global native

; The form id of the other player's follower this player activated, while the choice is open; 0 for none.
int Function ChosenFollower() global native

; In her player's game: the word waiting for this follower (her form id), 0 for none.
int Function TakeFollowerCommand(int formId) global native

; In her player's game: how it went (1 done, 2 busy, 3 not the game's own follower), told to the player who asked.
bool Function FollowerCommandDone(int formId, int command, int reply) global native

; A dragon's soul for this player (urSovngardeDragonSoulEffect): a dragon's form id to absorb, -1 for the soul alone, 0 for none.
int Function TakeDragonSoul() global native
