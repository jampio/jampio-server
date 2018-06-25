#include <jampio/common/g2/api.h>
#include <jampio/common/com_vars.h>
#include <jampio/common/protocol.h>
#include <jampio/common/msg.h>
#include <jampio/common/huffman.h>
#include <jampio/common/interface/server.h>
#include <jampio/common/sys.h>
#include "server.h"

serverStatic_t	svs;				// persistant server info
server_t		sv;					// local server
std::unique_ptr<VM> gvm;				// game virtual machine // bk001212 init

Cvar *sv_fps;				// time rate for running non-clients
Cvar *sv_timeout;			// seconds without any message
Cvar *sv_zombietime;			// seconds to sink messages after disconnect
Cvar *sv_rconPassword;		// password for remote server commands
Cvar *sv_privatePassword;	// password for the privateClient slots
Cvar *sv_maxclients;
Cvar *sv_privateClients;		// number of clients reserved for password
Cvar *sv_hostname;
Cvar *sv_allowDownload;
Cvar *sv_master[MAX_MASTER_SERVERS];		// master server ip address
Cvar *sv_reconnectlimit;		// minimum seconds between connect messages
Cvar *sv_showghoultraces;	// report ghoul2 traces
Cvar *sv_showloss;			// report when usercmds are lost
Cvar *sv_padPackets;			// add nop bytes to messages
Cvar *sv_killserver;			// menu system can set to 1 to shut server down
Cvar *sv_mapname;
Cvar *sv_mapChecksum;
Cvar *sv_serverid;
Cvar *sv_maxRate;
Cvar *sv_minPing;
Cvar *sv_maxPing;
Cvar *sv_gametype;
Cvar *sv_pure;
Cvar *sv_floodProtect;
Cvar *sv_needpass;

#ifdef USE_CD_KEY
Cvar *sv_allowAnonymous;
#endif
/*
=============================================================================

EVENT MESSAGES

=============================================================================
*/

/*
===============
SV_ExpandNewlines

Converts newlines to "\n" so a line prints nicer
===============
*/
template <std::size_t N>
void SV_ExpandNewlines(const char *in, char (&string)[N]) {
	int l = 0;
	while (*in && l < N - 3) {
		if (*in == '\n') {
			string[l++] = '\\';
			string[l++] = 'n';
		} else {
			string[l++] = *in;
		}
		in++;
	}
	string[l] = 0;
}

/*
======================
SV_ReplacePendingServerCommands

  This is ugly
======================
*/
int SV_ReplacePendingServerCommands( client_t *client, const char *cmd ) {
	int i, index, csnum1, csnum2;

	for ( i = client->reliableSent+1; i <= client->reliableSequence; i++ ) {
		index = i & ( MAX_RELIABLE_COMMANDS - 1 );
		//
		if ( !Q_strncmp(cmd, client->reliableCommands[ index ], strlen("cs")) ) {
			sscanf(cmd, "cs %i", &csnum1);
			sscanf(client->reliableCommands[ index ], "cs %i", &csnum2);
			if ( csnum1 == csnum2 ) {
				Q_strncpyz( client->reliableCommands[ index ], cmd, sizeof( client->reliableCommands[ index ] ) );
				/*
				if ( client->netchan.remoteAddress.type != NA_BOT ) {
					Com_Printf( "WARNING: client %i removed double pending config string %i: %s\n", client-svs.clients, csnum1, cmd );
				}
				*/
				return qtrue;
			}
		}
	}
	return qfalse;
}

/*
======================
SV_AddServerCommand

The given command will be transmitted to the client, and is guaranteed to
not have future snapshot_t executed before it is executed
======================
*/
void SV_AddServerCommand( client_t *client, const char *cmd ) {
	int		index, i;

	// this is very ugly but it's also a waste to for instance send multiple config string updates
	// for the same config string index in one snapshot
//	if ( SV_ReplacePendingServerCommands( client, cmd ) ) {
//		return;
//	}

	client->reliableSequence++;
	// if we would be losing an old command that hasn't been acknowledged,
	// we must drop the connection
	// we check == instead of >= so a broadcast print added by SV_DropClient()
	// doesn't cause a recursive drop client
	if ( client->reliableSequence - client->reliableAcknowledge == MAX_RELIABLE_COMMANDS + 1 ) {
		Com_Printf( "===== pending server commands =====\n" );
		for ( i = client->reliableAcknowledge + 1 ; i <= client->reliableSequence ; i++ ) {
			Com_Printf( "cmd %5d: %s\n", i, client->reliableCommands[ i & (MAX_RELIABLE_COMMANDS-1) ] );
		}
		Com_Printf( "cmd %5d: %s\n", i, cmd );
		SV_DropClient( client, "Server command overflow" );
		return;
	}
	index = client->reliableSequence & ( MAX_RELIABLE_COMMANDS - 1 );
	Q_strncpyz( client->reliableCommands[ index ], cmd, sizeof( client->reliableCommands[ index ] ) );
}


/*
=================
SV_SendServerCommand

Sends a reliable command string to be interpreted by 
the client game module: "cp", "print", "chat", etc
A NULL client will broadcast to all clients
=================
*/
// TODO: verify buffer safety
void QDECL SV_SendServerCommand(client_t *cl, const char *fmt, ...) {
	char message[MAX_MSGLEN];

	{
		va_list argptr;	
		va_start(argptr, fmt);
		vsprintf(message, fmt, argptr);
		va_end(argptr);
	}

	if (cl != NULL) {
		SV_AddServerCommand(cl, message);
		return;
	}

	// hack to echo broadcast prints to console
	if (com_dedicated->integer() && !strncmp(message, "print", 5)) {
		char buf[sizeof message];
		SV_ExpandNewlines(message, buf);
		// TODO: verify any possible buf overflow
		Com_Printf("broadcast: %s\n", buf);
	}

	// send the data to all relevent clients
	for (int i = 0; i < sv_maxclients->integer(); i++) {
		auto client = svs.clients + i;
		if (client->state < CS_PRIMED) {
			continue;
		}
		SV_AddServerCommand(client, message);
	}
}


/*
==============================================================================

MASTER SERVER FUNCTIONS

==============================================================================
*/
#ifndef _XBOX	// No master on Xbox
#define NEW_RESOLVE_DURATION		86400000 //24 hours
static int g_lastResolveTime[MAX_MASTER_SERVERS];

static inline bool SV_MasterNeedsResolving(int server, int time)
{ //refresh every so often regardless of if the actual address was modified -rww
	if (g_lastResolveTime[server] > time)
	{ //time flowed backwards?
		return true;
	}

	if ((time-g_lastResolveTime[server]) > NEW_RESOLVE_DURATION)
	{ //it's time again
		return true;
	}

	return false;
}

/*
================
SV_MasterHeartbeat

Send a message to the masters every few minutes to
let it know we are alive, and log information.
We will also have a heartbeat sent when a server
changes from empty to non-empty, and full to non-full,
but not on every player enter or exit.
================
*/
#define	HEARTBEAT_MSEC	300*1000
#define	HEARTBEAT_GAME	"QuakeArena-1"
void SV_MasterHeartbeat(CvarSystem& cvars) {
	static netadr_t	adr[MAX_MASTER_SERVERS];
	int			i;
	int			time;

	// "dedicated 1" is for lan play, "dedicated 2" is for inet public play
	if ( !com_dedicated || com_dedicated->integer() != 2 ) {
		return;		// only dedicated servers send heartbeats
	}

	// if not time yet, don't send anything
	if ( svs.time < svs.nextHeartbeatTime ) {
		return;
	}
	svs.nextHeartbeatTime = svs.time + HEARTBEAT_MSEC;

	//we need to use this instead of svs.time since svs.time resets over map changes (or rather
	//every time the game restarts), and we don't really need to resolve every map change
	time = Com_Milliseconds();

	// send to group masters
	for ( i = 0 ; i < MAX_MASTER_SERVERS ; i++ ) {
		if ( !sv_master[i]->string()[0] ) {
			continue;
		}

		// see if we haven't already resolved the name
		// resolving usually causes hitches on win95, so only
		// do it when needed
		if ( sv_master[i]->modified() || SV_MasterNeedsResolving(i, time) ) {
			sv_master[i]->setModified(false);

			g_lastResolveTime[i] = time;
	
			Com_Printf( "Resolving %s\n", sv_master[i]->string() );
			if ( !NET_StringToAdr( sv_master[i]->string(), &adr[i] ) ) {
				// if the address failed to resolve, clear it
				// so we don't take repeated dns hits
				Com_Printf( "Couldn't resolve address: %s\n", sv_master[i]->string() );
				cvars.Set( sv_master[i]->name(), "" );
				sv_master[i]->setModified(false);
				continue;
			}
			if ( !strstr( ":", sv_master[i]->string() ) ) {
				adr[i].port = BigShort( PORT_MASTER );
			}
			Com_Printf( "%s resolved to %i.%i.%i.%i:%i\n", sv_master[i]->string(),
				adr[i].ip[0], adr[i].ip[1], adr[i].ip[2], adr[i].ip[3],
				BigShort( adr[i].port ) );
		}


		Com_Printf ("Sending heartbeat to %s\n", sv_master[i]->string() );
		// this command should be changed if the server info / status format
		// ever incompatably changes
		NET_OutOfBandPrint( NS_SERVER, adr[i], "heartbeat %s\n", HEARTBEAT_GAME );
	}
}

/*
=================
SV_MasterShutdown

Informs all masters that this server is going down
=================
*/
void SV_MasterShutdown( void ) {
	// send a hearbeat right now
	svs.nextHeartbeatTime = -9999;
	SV_MasterHeartbeat();

	// send it again to minimize chance of drops
	svs.nextHeartbeatTime = -9999;
	SV_MasterHeartbeat();

	// when the master tries to poll the server, it won't respond, so
	// it will be removed from the list
}
#endif	// _XBOX	- No master on Xbox


/*
==============================================================================

CONNECTIONLESS COMMANDS

==============================================================================
*/

/*
================
SVC_Status

Responds with all the info that qplug or qspy can see about the server
and all connected players.  Used for getting detailed information after
the simple info query.
================
*/
void SVC_Status(CvarSystem& cvars, CommandArgs& args, netadr_t from) {
	char	player[1024];
	char	status[MAX_MSGLEN];
	int		i;
	client_t	*cl;
	playerState_t	*ps;
	int		statusLength;
	int		playerLength;

	auto infostring = cvars.InfoString(CVAR_SERVERINFO);

	// echo back the parameter to status. so master servers can use it as a challenge
	// to prevent timed spoofed reply packets that add ghost servers
	Info_SetValueForKey(infostring.data(), "challenge", args.Argv(1));

	// add "demo" to the sv_keywords if restricted
	if (cvars.VariableValue("fs_restrict")) {
		char keywords[MAX_INFO_STRING];
		Com_sprintf(keywords, sizeof(keywords), "demo %s", Info_ValueForKey(infostring.data(), "sv_keywords"));
		Info_SetValueForKey(infostring.data(), "sv_keywords", keywords);
	}

	status[0] = 0;
	statusLength = 0;

	for (i=0 ; i < sv_maxclients->integer() ; i++) {
		cl = &svs.clients[i];
		if ( cl->state >= CS_CONNECTED ) {
			ps = SV_GameClientNum( i );
			Com_sprintf (player, sizeof(player), "%i %i \"%s\"\n", 
				ps->persistant[PERS_SCORE], cl->ping, cl->name);
			playerLength = strlen(player);
			if (statusLength + playerLength >= sizeof(status) ) {
				break;		// can't hold any more
			}
			strcpy (status + statusLength, player);
			statusLength += playerLength;
		}
	}

	NET_OutOfBandPrint( NS_SERVER, from, "statusResponse\n%s\n%s", infostring.data(), status );
}

/*
================
SVC_Info

Responds with a short info message that should be enough to determine
if a user is interested in a server to do a full status
================
*/
void SVC_Info(CvarSystem& cvars, CommandArgs& args, netadr_t from) {
	int		i, count, wDisable;
	const char	*gamedir;
	char	infostring[MAX_INFO_STRING];

	if (cvars.VariableValue("ui_singlePlayerActive")) {
		return;
	}

	// don't count privateclients
	count = 0;
	for ( i = sv_privateClients->integer() ; i < sv_maxclients->integer() ; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			count++;
		}
	}

	infostring[0] = 0;

	// echo back the parameter to status. so servers can use it as a challenge
	// to prevent timed spoofed reply packets that add ghost servers
	Info_SetValueForKey( infostring, "challenge", args.Argv(1) );

	Info_SetValueForKey( infostring, "protocol", va("%i", PROTOCOL_VERSION) );
	Info_SetValueForKey( infostring, "hostname", sv_hostname->string() );
	Info_SetValueForKey( infostring, "mapname", sv_mapname->string() );
	Info_SetValueForKey( infostring, "clients", va("%i", count) );
	Info_SetValueForKey( infostring, "sv_maxclients", 
		va("%i", sv_maxclients->integer() - sv_privateClients->integer() ) );
	Info_SetValueForKey( infostring, "gametype", va("%i", sv_gametype->integer() ) );
	Info_SetValueForKey( infostring, "needpass", va("%i", sv_needpass->integer() ) );
	Info_SetValueForKey( infostring, "truejedi", va("%i", cvars.VariableIntegerValue( "g_jediVmerc" ) ) );
	if ( sv_gametype->integer() == GT_DUEL || sv_gametype->integer() == GT_POWERDUEL )
	{
		wDisable = cvars.VariableIntegerValue( "g_duelWeaponDisable" );
	}
	else
	{
		wDisable = cvars.VariableIntegerValue( "g_weaponDisable" );
	}
	Info_SetValueForKey( infostring, "wdisable", va("%i", wDisable ) );
	Info_SetValueForKey( infostring, "fdisable", va("%i", cvars.VariableIntegerValue( "g_forcePowerDisable" ) ) );
	//Info_SetValueForKey( infostring, "pure", va("%i", sv_pure->integer ) );

	if( sv_minPing->integer() ) {
		Info_SetValueForKey( infostring, "minPing", va("%i", sv_minPing->integer()) );
	}
	if( sv_maxPing->integer() ) {
		Info_SetValueForKey( infostring, "maxPing", va("%i", sv_maxPing->integer()) );
	}
	gamedir = cvars.VariableString( "fs_game" );
	if( *gamedir ) {
		Info_SetValueForKey( infostring, "game", gamedir );
	}
#ifdef USE_CD_KEY
	Info_SetValueForKey( infostring, "sv_allowAnonymous", va("%i", sv_allowAnonymous->integer) );
#endif

	NET_OutOfBandPrint( NS_SERVER, from, "infoResponse\n%s", infostring );
}

/*
================
SVC_FlushRedirect

================
*/
void SV_FlushRedirect( char *outputbuf ) {
	NET_OutOfBandPrint( NS_SERVER, svs.redirectAddress, "print\n%s", outputbuf );
}

/*
===============
SVC_RemoteCommand

An rcon packet arrived from the network.
Shift down the remaining args
Redirect all printfs
===============
*/
void SVC_RemoteCommand(CommandSystem& cmd, CommandArgs& args, netadr_t from, msg_t *msg) {
	qboolean	valid;
	unsigned int	i, time;
	char		remaining[1024];
#define	SV_OUTPUTBUF_LENGTH	(MAX_MSGLEN - 16)
	char		sv_outputbuf[SV_OUTPUTBUF_LENGTH];
	static		unsigned int	lasttime = 0;

	time = Com_Milliseconds();
	if (time<(lasttime+500)) {
		return;
	}
	lasttime = time;

	if ( !strlen( sv_rconPassword->string() ) ||
		strcmp (args.Argv(1), sv_rconPassword->string()) ) {
		valid = qfalse;
		Com_DPrintf ("Bad rcon from %s:\n%s\n", NET_AdrToString (from), args.Argv(2) );
	} else {
		valid = qtrue;
		Com_DPrintf ("Rcon from %s:\n%s\n", NET_AdrToString (from), args.Argv(2) );
	}

	// start redirecting all print outputs to the packet
	svs.redirectAddress = from;
	Com_BeginRedirect (sv_outputbuf, SV_OUTPUTBUF_LENGTH, SV_FlushRedirect);

	if ( !strlen( sv_rconPassword->string() ) ) {
		Com_Printf ("No rconpassword set.\n");
	} else if ( !valid ) {
		Com_Printf ("Bad rconpassword.\n");
	} else {
		remaining[0] = 0;

		for (i=2 ; i<args.Argc() ; i++) {
			strcat (remaining, args.Argv(i) );
			strcat (remaining, " ");
		}

		cmd.ExecuteString (remaining);
	}

	Com_EndRedirect ();
}

/*
=================
SV_ConnectionlessPacket

A connectionless packet has four leading 0xff
characters to distinguish it from a game channel.
Clients that are in the game can still send
connectionless packets.
=================
*/
void SV_ConnectionlessPacket(CommandSystem& cmd, CvarSystem& cvars, netadr_t from, msg_t *msg) {
	char	*s;
	const char	*c;

	MSG_BeginReadingOOB( msg );
	MSG_ReadLong( msg );		// skip the -1 marker

	if (!Q_strncmp("connect", (const char *)&msg->data[4], 7)) {
		Huff_Decompress(msg, 12);
	}

	s = MSG_ReadStringLine( msg );
	auto args = CommandArgs::TokenizeString(s);

	c = args.Argv(0);
	Com_DPrintf ("SV packet %s : %s\n", NET_AdrToString(from), c);

	if (!Q_stricmp(c, "getstatus")) {
		SVC_Status(cvars, args, from);
	} else if (!Q_stricmp(c, "getinfo")) {
		SVC_Info(cvars, args, from);
	} else if (!Q_stricmp(c, "getchallenge")) {
		SV_GetChallenge( from );
	} else if (!Q_stricmp(c, "connect")) {
		SV_DirectConnect( from );
#ifndef _XBOX	// No authorization on Xbox
	} else if (!Q_stricmp(c, "ipAuthorize")) {
		SV_AuthorizeIpPacket( from );
#endif
	} else if (!Q_stricmp(c, "rcon")) {
		SVC_RemoteCommand(cmd, args, from, msg);
	} else if (!Q_stricmp(c, "disconnect")) {
		// if a client starts up a local server, we may see some spurious
		// server disconnect messages when their new server sees our final
		// sequenced messages to the old client
	} else {
		Com_DPrintf ("bad connectionless packet from %s:\n%s\n"
		, NET_AdrToString (from), s);
	}
}


//============================================================================

/*
=================
SV_ReadPackets
=================
*/
void SV_PacketEvent(CommandSystem& cmd, CvarSystem& cvars, netadr_t from, msg_t *msg) {
	int			i;
	client_t	*cl;
	int			qport;

	// check for connectionless packet (0xffffffff) first
	if ( msg->cursize >= 4 && *(int *)msg->data == -1) {
		SV_ConnectionlessPacket(cmd, cvars, from, msg);
		return;
	}

	// read the qport out of the message so we can fix up
	// stupid address translating router
	MSG_BeginReadingOOB( msg );
	MSG_ReadLong( msg );				// sequence number
	qport = MSG_ReadShort( msg ) & 0xffff;

	// find which client the message is from
	for (i=0, cl=svs.clients ; i < sv_maxclients->integer() ; i++,cl++) {
		if (cl->state == CS_FREE) {
			continue;
		}
		if ( !NET_CompareBaseAdr( from, cl->netchan.remoteAddress ) ) {
			continue;
		}
		// it is possible to have multiple clients from a single IP
		// address, so they are differentiated by the qport variable
		if (cl->netchan.qport != qport) {
			continue;
		}

		// the IP port can't be used to differentiate them, because
		// some address translating routers periodically change UDP
		// port assignments
		if (cl->netchan.remoteAddress.port != from.port) {
			Com_Printf( "SV_ReadPackets: fixing up a translated port\n" );
			cl->netchan.remoteAddress.port = from.port;
		}

		// make sure it is a valid, in sequence packet
		if (SV_Netchan_Process(cl, msg)) {
			// zombie clients still need to do the Netchan_Process
			// to make sure they don't need to retransmit the final
			// reliable message, but they don't do any other processing
			if (cl->state != CS_ZOMBIE) {
				cl->lastPacketTime = svs.time;	// don't timeout
				SV_ExecuteClientMessage( cl, msg );
			}
		}
		return;
	}
	
	// if we received a sequenced packet from an address we don't reckognize,
	// send an out of band disconnect packet to it
	NET_OutOfBandPrint( NS_SERVER, from, "disconnect" );
}


/*
===================
SV_CalcPings

Updates the cl->ping variables
===================
*/
void SV_CalcPings( void ) {
	int			i, j;
	client_t	*cl;
	int			total, count;
	int			delta;
	playerState_t	*ps;

	for (i=0 ; i < sv_maxclients->integer() ; i++) {
		cl = &svs.clients[i];
		if ( cl->state != CS_ACTIVE ) {
			cl->ping = 999;
			continue;
		}
		if ( !cl->gentity ) {
			cl->ping = 999;
			continue;
		}
		if ( cl->gentity->r.svFlags & SVF_BOT ) {
			cl->ping = 0;
			continue;
		}

		total = 0;
		count = 0;
		for ( j = 0 ; j < PACKET_BACKUP ; j++ ) {
			if ( cl->frames[j].messageAcked <= 0 ) {
				continue;
			}
			delta = cl->frames[j].messageAcked - cl->frames[j].messageSent;
			count++;
			total += delta;
		}
		if (!count) {
			cl->ping = 999;
		} else {
			cl->ping = total/count;
			if ( cl->ping > 999 ) {
				cl->ping = 999;
			}
		}

		// let the game dll know about the ping
		ps = SV_GameClientNum( i );
		ps->ping = cl->ping;
	}
}

/*
==================
SV_CheckTimeouts

If a packet has not been received from a client for timeout->integer 
seconds, drop the conneciton.  Server time is used instead of
realtime to avoid dropping the local client while debugging.

When a client is normally dropped, the client_t goes into a zombie state
for a few seconds to make sure any final reliable message gets resent
if necessary
==================
*/
void SV_CheckTimeouts( void ) {
	int		i;
	client_t	*cl;
	int			droppoint;
	int			zombiepoint;

	droppoint = svs.time - 1000 * sv_timeout->integer();
	zombiepoint = svs.time - 1000 * sv_zombietime->integer();

	for (i=0,cl=svs.clients ; i < sv_maxclients->integer() ; i++,cl++) {
		// message times may be wrong across a changelevel
		if (cl->lastPacketTime > svs.time) {
			cl->lastPacketTime = svs.time;
		}

		if (cl->state == CS_ZOMBIE
		&& cl->lastPacketTime < zombiepoint) {
			Com_DPrintf( "Going from CS_ZOMBIE to CS_FREE for %s\n", cl->name );
			cl->state = CS_FREE;	// can now be reused
			continue;
		}
		if ( cl->state >= CS_CONNECTED && cl->lastPacketTime < droppoint) {
			// wait several frames so a debugger session doesn't
			// cause a timeout
			if ( ++cl->timeoutCount > 5 ) {
				SV_DropClient (cl, "timed out"); 
				cl->state = CS_FREE;	// don't bother with zombie state
			}
		} else {
			cl->timeoutCount = 0;
		}
	}
}


/*
==================
SV_CheckPaused
==================
*/
qboolean SV_CheckPaused(CvarSystem& cvars) {
	int		count;
	client_t	*cl;
	int		i;

	if ( !cl_paused->integer() ) {
		return qfalse;
	}

	// only pause if there is just a single client connected
	count = 0;
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer() ; i++,cl++) {
		if ( cl->state >= CS_CONNECTED && cl->netchan.remoteAddress.type != NA_BOT ) {
			count++;
		}
	}

	if ( count > 1 ) {
		// don't pause
		cvars.Set(sv_paused->name(), "0");
		return qfalse;
	}

	cvars.Set(sv_paused->name(), "1");
	return qtrue;
}

/*
==================
SV_CheckCvars
==================
*/
void SV_CheckCvars(CvarSystem& cvars) {
	static int lastMod = -1;
	qboolean	changed = qfalse;
	
	if ( sv_hostname->modificationCount() != lastMod ) {
		char hostname[MAX_INFO_STRING];
		char *c = hostname;
		lastMod = sv_hostname->modificationCount();
		
		strcpy( hostname, sv_hostname->string() );
		while( *c )
		{
			if ( (*c == '\\') || (*c == ';') || (*c == '"'))
			{
				*c = '.';
				changed = qtrue;
			}
			c++;
		}
		if( changed )
		{
			cvars.Set(sv_hostname->name(), hostname);
		}
		
	}
}

/*
==================
SV_Frame

Player movement occurs as a result of packet events, which
happen before SV_Frame is called
==================
*/
void SV_Frame(CommandBuffer& cbuf, CvarSystem& cvars, int msec) {
	int		frameMsec;
	int		startTime;

	// the menu kills the server with this cvar
	if ( sv_killserver->integer() ) {
		SV_Shutdown ("Server was killed.\n");
		cvars.Set(sv_killserver->name(), "0");
		return;
	}

	if ( !com_sv_running->integer() ) {
		return;
	}

	// allow pause if only the local client is connected
	if ( SV_CheckPaused(cvars) ) {
		return;
	}

	// if it isn't time for the next frame, do nothing
	if ( sv_fps->integer() < 1 ) {
		cvars.Set(sv_fps->name(), "10");
	}
	frameMsec = 1000 / sv_fps->integer() ;

	sv.timeResidual += msec;

	if (!com_dedicated->integer()) SV_BotFrame( svs.time + sv.timeResidual );

	if ( com_dedicated->integer() && sv.timeResidual < frameMsec && (!com_timescale || com_timescale->value() >= 1) ) {
		// NET_Sleep will give the OS time slices until either get a packet
		// or time enough for a server frame has gone by
		NET_Sleep(frameMsec - sv.timeResidual);
		return;
	}

	// if time is about to hit the 32nd bit, kick all clients
	// and clear sv.time, rather
	// than checking for negative time wraparound everywhere.
	// 2giga-milliseconds = 23 days, so it won't be too often
	if ( svs.time > 0x70000000 ) {
		SV_Shutdown( "Restarting server due to time wrapping" );
		//Cbuf_AddText( "vstr nextmap\n" );
		cbuf.AddText( "map_restart 0\n" );
		return;
	}
	// this can happen considerably earlier when lots of clients play and the map doesn't change
	if ( svs.nextSnapshotEntities >= 0x7FFFFFFE - svs.numSnapshotEntities ) {
		SV_Shutdown( "Restarting server due to numSnapshotEntities wrapping" );
		//Cbuf_AddText( "vstr nextmap\n" );
		cbuf.AddText( "map_restart 0\n" );
		return;
	}

	if( sv.restartTime && svs.time >= sv.restartTime ) {
		sv.restartTime = 0;
		cbuf.AddText( "map_restart 0\n" );
		return;
	}

	// update infostrings if anything has been changed
	if (cvars.hasModifiedFlags(CVAR_SERVERINFO)) {
		auto infostring = cvars.InfoString(CVAR_SERVERINFO);
		SV_SetConfigstring(CS_SERVERINFO, infostring.data());
		cvars.removeModifiedFlags(CVAR_SERVERINFO);
	}
	if (cvars.hasModifiedFlags(CVAR_SYSTEMINFO)) {
		auto infostring = cvars.InfoStringBig(CVAR_SYSTEMINFO);
		SV_SetConfigstring(CS_SYSTEMINFO, infostring.data());
		cvars.removeModifiedFlags(CVAR_SYSTEMINFO);
	}

	if ( com_speeds->integer() ) {
		startTime = Sys_Milliseconds ();
	} else {
		startTime = 0;	// quite a compiler warning
	}

	// update ping based on the all received frames
	SV_CalcPings();

	if (com_dedicated->integer()) SV_BotFrame( svs.time );

	// run the game simulation in chunks
	while ( sv.timeResidual >= frameMsec ) {
		sv.timeResidual -= frameMsec;
		svs.time += frameMsec;

		// let everything in the world think and move
		gvm->call(GAME_RUN_FRAME, svs.time);
	}

	//rww - RAGDOLL_BEGIN
	G2API_SetTime(svs.time,0);
	//rww - RAGDOLL_END

	if ( com_speeds->integer() ) {
		time_game = Sys_Milliseconds () - startTime;
	}

	// check timeouts
	SV_CheckTimeouts();

	// send messages back to the clients
	SV_SendClientMessages();

	SV_CheckCvars(cvars);

	// send a heartbeat to the master if needed
	SV_MasterHeartbeat();
}
