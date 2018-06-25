#include <jampio/shared/configstrings.h>
#include <jampio/common/stringed_ingame.h>
#include <jampio/common/com_vars.h>
#include <jampio/common/fs.h>
#include <jampio/common/interface/server.h>
#include "server.h"

/*
===============================================================================

OPERATOR CONSOLE ONLY COMMANDS

These commands can only be entered from stdin or by a remote operator datagram
===============================================================================
*/

const char *SV_GetStringEdString(const char *refSection, const char *refName)
{
	/*
	static char text[1024]={0};
	trap_SP_GetStringTextString(va("%s_%s", refSection, refName), text, sizeof(text));
	return text;
	*/

	//Well, it would've been lovely doing it the above way, but it would mean mixing
	//languages for the client depending on what the server is. So we'll mark this as
	//a stringed reference with @@@ and send the refname to the client, and when it goes
	//to print it will get scanned for the stringed reference indication and dealt with
	//properly.
	static char text[1024]={0};
	Com_sprintf(text, sizeof(text), "@@@%s", refName);
	return text;
}



/*
==================
SV_GetPlayerByName

Returns the player with name from Cmd_Argv(1)
==================
*/
static client_t *SV_GetPlayerByName(CommandArgs& args) {
	client_t	*cl;
	int			i;
	const char		*s;
	char		cleanName[64];

	// make sure server is running
	if ( !com_sv_running->integer() ) {
		return NULL;
	}

	if ( args.Argc() < 2 ) {
		Com_Printf( "No player specified.\n" );
		return NULL;
	}

	s = args.Argv(1);

	// check for a name match
	for ( i=0, cl=svs.clients ; i < sv_maxclients->integer() ; i++,cl++ ) {
		if ( !cl->state ) {
			continue;
		}
		if ( !Q_stricmp( cl->name, s ) ) {
			return cl;
		}

		Q_strncpyz( cleanName, cl->name, sizeof(cleanName) );
		Q_CleanStr( cleanName );
		if ( !Q_stricmp( cleanName, s ) ) {
			return cl;
		}
	}

	Com_Printf( "Player %s is not on the server\n", s );

	return NULL;
}

/*
==================
SV_GetPlayerByNum

Returns the player with idnum from Cmd_Argv(1)
==================
*/
static client_t *SV_GetPlayerByNum(CommandArgs& args) {
	client_t	*cl;
	int			i;
	int			idnum;
	const char		*s;

	// make sure server is running
	if ( !com_sv_running->integer() ) {
		return NULL;
	}

	if ( args.Argc() < 2 ) {
		Com_Printf( "No player specified.\n" );
		return NULL;
	}

	s = args.Argv(1);

	for (i = 0; s[i]; i++) {
		if (s[i] < '0' || s[i] > '9') {
			Com_Printf( "Bad slot number: %s\n", s);
			return NULL;
		}
	}
	idnum = atoi( s );
	if ( idnum < 0 || idnum >= sv_maxclients->integer() ) {
		Com_Printf( "Bad client slot: %i\n", idnum );
		return NULL;
	}

	cl = &svs.clients[idnum];
	if ( !cl->state ) {
		Com_Printf( "Client %i is not active\n", idnum );
		return NULL;
	}
	return cl;
}

//=========================================================



/*
==================
SV_Map_f

Restart the server on a different map
==================
*/
static void SV_Map_f(CvarSystem& cvars, CommandArgs& args) {
	const char		*cmd;
	const char		*map;
	qboolean	killBots, cheat;
	char		expanded[MAX_QPATH];
	char		mapname[MAX_QPATH];

	map = args.Argv(1);
	if ( !map ) {
		return;
	}

	// make sure the level exists before trying to change, so that
	// a typo at the server console won't end the game
	if (strchr (map, '\\') ) {
		Com_Printf ("Can't have mapnames with a \\\n");
		return;
	}

#ifndef _XBOX
	Com_sprintf (expanded, sizeof(expanded), "maps/%s.bsp", map);
	if ( FS_ReadFile (expanded, NULL) == -1 ) {
		Com_Printf ("Can't find map %s\n", expanded);
		return;
	}
#endif

	// force latched values to get set
	cvars.Get ("g_gametype", "0", CVAR_SERVERINFO | CVAR_LATCH );

	cmd = args.Argv(0);
	if( Q_stricmpn( cmd, "sp", 2 ) == 0 ) {
		cvars.SetValue( "g_gametype", GT_SINGLE_PLAYER );
		cvars.SetValue( "g_doWarmup", 0 );
		// may not set sv_maxclients directly, always set latched
		cvars.SetLatched( "sv_maxclients", "8" );
		cmd += 2;
		cheat = qfalse;
		killBots = qtrue;
	}
	else {
		if ( !Q_stricmpn( cmd, "devmap",6 ) || !Q_stricmp( cmd, "spdevmap" ) ) {
			cheat = qtrue;
			killBots = qtrue;
		} else {
			cheat = qfalse;
			killBots = qfalse;
		}
		/*
		if( sv_gametype->integer == GT_SINGLE_PLAYER ) {
			Cvar_SetValue( "g_gametype", GT_FFA );
		}
		*/
	}

	// save the map name here cause on a map restart we reload the jampconfig.cfg
	// and thus nuke the arguments of the map command
	Q_strncpyz(mapname, map, sizeof(mapname));

	ForceReload_e eForceReload = eForceReload_NOTHING;	// default for normal load

//	if ( !Q_stricmp( cmd, "devmapbsp") ) {	// not relevant in MP codebase
//		eForceReload = eForceReload_BSP;
//	}
//	else
	if ( !Q_stricmp( cmd, "devmapmdl") ) {
		eForceReload = eForceReload_MODELS;
	}
	else
	if ( !Q_stricmp( cmd, "devmapall") ) {
		eForceReload = eForceReload_ALL;
	}

	// start up the map
	SV_SpawnServer( mapname, killBots, eForceReload );

	// set the cheat value
	// if the level was started with "map <levelname>", then
	// cheats will not be allowed.  If started with "devmap <levelname>"
	// then cheats will be allowed
	if ( cheat ) {
		cvars.Set( "sv_cheats", "1" );
	} else {
		cvars.Set( "sv_cheats", "0" );
	}
}


/*
================
SV_MapRestart_f

Completely restarts a level, but doesn't send a new gamestate to the clients.
This allows fair starts with variable load times.
================
*/
static void SV_MapRestart_f(CvarSystem& cvars, CommandArgs& args) {
	int			i;
	client_t	*client;
	const char	*denied;
	qboolean	isBot;
	int			delay;

	// make sure we aren't restarting twice in the same frame
	if ( com_frameTime == sv.serverId ) {
		return;
	}

	// make sure server is running
	if ( !com_sv_running->integer() ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( sv.restartTime ) {
		return;
	}

	if (args.Argc() > 1 ) {
		delay = atoi( args.Argv(1) );
	}
	else {
		delay = 5;
	}
	if( delay ) {
		sv.restartTime = svs.time + delay * 1000;
		SV_SetConfigstring( CS_WARMUP, va("%i", sv.restartTime) );
		return;
	}

	// check for changes in variables that can't just be restarted
	// check for maxclients change
	if ( sv_maxclients->modified() || sv_gametype->modified() ) {
		char	mapname[MAX_QPATH];

		Com_Printf( "variable change -- restarting.\n" );
		// restart the map the slow way
		Q_strncpyz( mapname, cvars.VariableString( "mapname" ), sizeof( mapname ) );

		SV_SpawnServer( mapname, qfalse, eForceReload_NOTHING );
		return;
	}

	// toggle the server bit so clients can detect that a
	// map_restart has happened
	svs.snapFlagServerBit ^= SNAPFLAG_SERVERCOUNT;

	// generate a new serverid
	sv.restartedServerId = sv.serverId;
	sv.serverId = com_frameTime;
	cvars.Set( "sv_serverid", va("%i", sv.serverId ) );

	// reset all the vm data in place without changing memory allocation
	// note that we do NOT set sv.state = SS_LOADING, so configstrings that
	// had been changed from their default values will generate broadcast updates
	sv.state = SS_LOADING;
	sv.restarting = qtrue;

	SV_RestartGameProgs();

	// run a few frames to allow everything to settle
	for ( i = 0 ;i < 3 ; i++ ) {
		gvm->call(GAME_RUN_FRAME, svs.time);
		svs.time += 100;
	}

	sv.state = SS_GAME;
	sv.restarting = qfalse;

	// connect and begin all the clients
	for (i=0 ; i<sv_maxclients->integer() ; i++) {
		client = &svs.clients[i];

		// send the new gamestate to all connected clients
		if ( client->state < CS_CONNECTED) {
			continue;
		}

		if ( client->netchan.remoteAddress.type == NA_BOT ) {
			isBot = qtrue;
		} else {
			isBot = qfalse;
		}

		// add the map_restart command
		SV_AddServerCommand( client, "map_restart\n" );

		// connect the client again, without the firstTime flag
		denied = (const char *) gvm->call(GAME_CLIENT_CONNECT, i, qfalse, isBot);
		if ( denied ) {
			// this generally shouldn't happen, because the client
			// was connected before the level change
			SV_DropClient( client, denied );
			Com_Printf( "SV_MapRestart_f(%d): dropped client %i - denied!\n", delay, i ); // bk010125
			continue;
		}

		client->state = CS_ACTIVE;

		SV_ClientEnterWorld( client, &client->lastUsercmd );
	}	

	// run another frame to allow things to look at all the players
	gvm->call(GAME_RUN_FRAME, svs.time);
	svs.time += 100;
}

//===============================================================

/*
==================
SV_GetPlayerByName

Returns the player with name from Cmd_Argv(1)
==================
*/
static client_t *SV_GetPlayerByFedName( const char *name )
{
	client_t	*cl;
	int			i;
	char		cleanName[64];

	// make sure server is running
	if ( !com_sv_running->integer() )
	{
		return NULL;
	}

	// check for a name match
	for ( i=0, cl=svs.clients ; i < sv_maxclients->integer() ; i++,cl++ )
	{
		if ( !cl->state )
		{
			continue;
		}
		if ( !Q_stricmp( cl->name, name ) )
		{
			return cl;
		}

		Q_strncpyz( cleanName, cl->name, sizeof(cleanName) );
		Q_CleanStr( cleanName );
		if ( !Q_stricmp( cleanName, name ) )
		{
			return cl;
		}
	}

	return NULL;
}

static void SV_KickByName( const char *name )
{
	client_t	*cl;
	int			i;

	// make sure server is running
	if ( !com_sv_running->integer() )
	{
		return;
	}

	cl = SV_GetPlayerByFedName(name);
	if ( !cl )
	{
		if ( !Q_stricmp(name, "all") )
		{
			for ( i=0, cl=svs.clients ; i < sv_maxclients->integer() ; i++,cl++ )
			{
				if ( !cl->state )
				{
					continue;
				}
				if( cl->netchan.remoteAddress.type == NA_LOOPBACK )
				{
					continue;
				}
				SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
				cl->lastPacketTime = svs.time;	// in case there is a funny zombie
			}
		}
		else if ( !Q_stricmp(name, "allbots") )
		{
			for ( i=0, cl=svs.clients ; i < sv_maxclients->integer() ; i++,cl++ )
			{
				if ( !cl->state )
				{
					continue;
				}
				if( cl->netchan.remoteAddress.type != NA_BOT )
				{
					continue;
				}
				SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
				cl->lastPacketTime = svs.time;	// in case there is a funny zombie
			}
		}
		return;
	}
	if( cl->netchan.remoteAddress.type == NA_LOOPBACK )
	{
//		SV_SendServerCommand(NULL, "print \"%s\"", "Cannot kick host player\n");
		SV_SendServerCommand(NULL, "print \"%s\"", SV_GetStringEdString("MP_SVGAME","CANNOT_KICK_HOST"));
		return;
	}

	SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
	cl->lastPacketTime = svs.time;	// in case there is a funny zombie
}

/*
==================
SV_Kick_f

Kick a user off of the server  FIXME: move to game
==================
*/
static void SV_Kick_f(CommandArgs& args) {
	client_t	*cl;
	int			i;

	// make sure server is running
	if ( !com_sv_running->integer() ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( args.Argc() != 2 ) {
		Com_Printf ("Usage: kick <player name>\nkick all = kick everyone\nkick allbots = kick all bots\n");
		return;
	}

	if (!Q_stricmp(args.Argv(1), "Padawan"))
	{ //if you try to kick the default name, also try to kick ""
		SV_KickByName("");
	}

	cl = SV_GetPlayerByName(args);
	if ( !cl ) {
		if ( !Q_stricmp(args.Argv(1), "all") ) {
			for ( i=0, cl=svs.clients ; i < sv_maxclients->integer() ; i++,cl++ ) {
				if ( !cl->state ) {
					continue;
				}
				if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
					continue;
				}
				SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
				cl->lastPacketTime = svs.time;	// in case there is a funny zombie
			}
		}
		else if ( !Q_stricmp(args.Argv(1), "allbots") ) {
			for ( i=0, cl=svs.clients ; i < sv_maxclients->integer() ; i++,cl++ ) {
				if ( !cl->state ) {
					continue;
				}
				if( cl->netchan.remoteAddress.type != NA_BOT ) {
					continue;
				}
				SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
				cl->lastPacketTime = svs.time;	// in case there is a funny zombie
			}
		}
		return;
	}
	if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
//		SV_SendServerCommand(NULL, "print \"%s\"", "Cannot kick host player\n");
		SV_SendServerCommand(NULL, "print \"%s\"", SV_GetStringEdString("MP_SVGAME","CANNOT_KICK_HOST"));
		return;
	}

	SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
	cl->lastPacketTime = svs.time;	// in case there is a funny zombie
}

/*
==================
SV_Ban_f

Ban a user from being able to play on this server through the auth
server
==================
*/
#ifdef USE_CD_KEY

static void SV_Ban_f( void ) {
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf ("Usage: banUser <player name>\n");
		return;
	}

	cl = SV_GetPlayerByName();

	if (!cl) {
		return;
	}

	if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
//		SV_SendServerCommand(NULL, "print \"%s\"", "Cannot kick host player\n");
		SV_SendServerCommand(NULL, "print \"%s\"", SV_GetStringEdString("MP_SVGAME","CANNOT_KICK_HOST"));
		return;
	}

	// look up the authorize server's IP
	if ( !svs.authorizeAddress.ip[0] && svs.authorizeAddress.type != NA_BAD ) {
		Com_Printf( "Resolving %s\n", AUTHORIZE_SERVER_NAME );
		if ( !NET_StringToAdr( AUTHORIZE_SERVER_NAME, &svs.authorizeAddress ) ) {
			Com_Printf( "Couldn't resolve address\n" );
			return;
		}
		svs.authorizeAddress.port = BigShort( PORT_AUTHORIZE );
		Com_Printf( "%s resolved to %i.%i.%i.%i:%i\n", AUTHORIZE_SERVER_NAME,
			svs.authorizeAddress.ip[0], svs.authorizeAddress.ip[1],
			svs.authorizeAddress.ip[2], svs.authorizeAddress.ip[3],
			BigShort( svs.authorizeAddress.port ) );
	}

	// otherwise send their ip to the authorize server
	if ( svs.authorizeAddress.type != NA_BAD ) {
		NET_OutOfBandPrint( NS_SERVER, svs.authorizeAddress,
			"banUser %i.%i.%i.%i", cl->netchan.remoteAddress.ip[0], cl->netchan.remoteAddress.ip[1], 
								   cl->netchan.remoteAddress.ip[2], cl->netchan.remoteAddress.ip[3] );
		Com_Printf("%s was banned from coming back\n", cl->name);
	}
}

/*
==================
SV_BanNum_f

Ban a user from being able to play on this server through the auth
server
==================
*/
static void SV_BanNum_f( void ) {
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf ("Usage: banClient <client number>\n");
		return;
	}

	cl = SV_GetPlayerByNum();
	if ( !cl ) {
		return;
	}
	if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
//		SV_SendServerCommand(NULL, "print \"%s\"", "Cannot kick host player\n");
		SV_SendServerCommand(NULL, "print \"%s\"", SV_GetStringEdString("MP_SVGAME","CANNOT_KICK_HOST"));
		return;
	}

	// look up the authorize server's IP
	if ( !svs.authorizeAddress.ip[0] && svs.authorizeAddress.type != NA_BAD ) {
		Com_Printf( "Resolving %s\n", AUTHORIZE_SERVER_NAME );
		if ( !NET_StringToAdr( AUTHORIZE_SERVER_NAME, &svs.authorizeAddress ) ) {
			Com_Printf( "Couldn't resolve address\n" );
			return;
		}
		svs.authorizeAddress.port = BigShort( PORT_AUTHORIZE );
		Com_Printf( "%s resolved to %i.%i.%i.%i:%i\n", AUTHORIZE_SERVER_NAME,
			svs.authorizeAddress.ip[0], svs.authorizeAddress.ip[1],
			svs.authorizeAddress.ip[2], svs.authorizeAddress.ip[3],
			BigShort( svs.authorizeAddress.port ) );
	}

	// otherwise send their ip to the authorize server
	if ( svs.authorizeAddress.type != NA_BAD ) {
		NET_OutOfBandPrint( NS_SERVER, svs.authorizeAddress,
			"banUser %i.%i.%i.%i", cl->netchan.remoteAddress.ip[0], cl->netchan.remoteAddress.ip[1], 
								   cl->netchan.remoteAddress.ip[2], cl->netchan.remoteAddress.ip[3] );
		Com_Printf("%s was banned from coming back\n", cl->name);
	}
}

#endif	// USE_CD_KEY

/*
==================
SV_KickNum_f

Kick a user off of the server  FIXME: move to game
==================
*/
static void SV_KickNum_f(CommandArgs& args) {
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer() ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( args.Argc() != 2 ) {
		Com_Printf ("Usage: kicknum <client number>\n");
		return;
	}

	cl = SV_GetPlayerByNum(args);
	if ( !cl ) {
		return;
	}
	if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
//		SV_SendServerCommand(NULL, "print \"%s\"", "Cannot kick host player\n");
		SV_SendServerCommand(NULL, "print \"%s\"", SV_GetStringEdString("MP_SVGAME","CANNOT_KICK_HOST"));
		return;
	}

	SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","WAS_KICKED"));	// "was kicked" );
	cl->lastPacketTime = svs.time;	// in case there is a funny zombie
}

/*
================
SV_Status_f
================
*/
static void SV_Status_f(CommandArgs& args) 
{
	int				i;
	client_t		*cl;
	playerState_t	*ps;
	const char		*s;
	int				ping;
	char			state[32];
	qboolean		avoidTruncation = qfalse;

	// make sure server is running
	if ( !com_sv_running->integer() ) 
	{
		Com_Printf( SE_GetString("STR_SERVER_SERVER_NOT_RUNNING") );
		return;
	}

	if ( args.Argc() > 1 )
	{
		if (!Q_stricmp("notrunc", args.Argv(1)))
		{
			avoidTruncation = qtrue;
		}
	}

	Com_Printf ("map: %s\n", sv_mapname->string() );

	Com_Printf ("num score ping name            lastmsg address               qport rate\n");
	Com_Printf ("--- ----- ---- --------------- ------- --------------------- ----- -----\n");
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer() ; i++,cl++)
	{
		if (!cl->state)
		{
			continue;
		}

		if (cl->state == CS_CONNECTED)
		{
			strcpy(state, "CNCT ");
		}
		else if (cl->state == CS_ZOMBIE)
		{
			strcpy(state, "ZMBI ");
		}
		else
		{
			ping = cl->ping < 9999 ? cl->ping : 9999;
			sprintf(state, "%4i", ping);
		}

		ps = SV_GameClientNum( i );
		s = NET_AdrToString( cl->netchan.remoteAddress );

		if (!avoidTruncation)
		{
			Com_Printf ("%3i %5i %s %-15.15s %7i %21s %5i %5i\n", 
				i, 
				ps->persistant[PERS_SCORE],
				state,
				cl->name,
				svs.time - cl->lastPacketTime,
				s,
				cl->netchan.qport,
				cl->rate
				);
		}
		else
		{
			Com_Printf ("%3i %5i %s %s %7i %21s %5i %5i\n", 
				i, 
				ps->persistant[PERS_SCORE],
				state,
				cl->name,
				svs.time - cl->lastPacketTime,
				s,
				cl->netchan.qport,
				cl->rate
				);
		}
	}
	Com_Printf ("\n");
}

/*
==================
SV_ConSay_f
==================
*/
static void SV_ConSay_f(CommandArgs& args) {
	char	text[1024];

	if( !com_dedicated->integer() ) {
		Com_Printf( "Server is not dedicated.\n" );
		return;
	}

	// make sure server is running
	if ( !com_sv_running->integer() ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( args.Argc () < 2 ) {
		return;
	}

	strcpy (text, "Server: ");
	auto arg_string = args.Args();
	auto p = arg_string.data();

	if ( *p == '"' ) {
		p++;
		p[strlen(p)-1] = 0;
	}

	strcat(text, p);

	SV_SendServerCommand(NULL, "chat \"%s\n\"", text);
}

static const char *forceToggleNamePrints[] = 
{
	"HEAL",//FP_HEAL
	"JUMP",//FP_LEVITATION
	"SPEED",//FP_SPEED
	"PUSH",//FP_PUSH
	"PULL",//FP_PULL
	"MINDTRICK",//FP_TELEPTAHY
	"GRIP",//FP_GRIP
	"LIGHTNING",//FP_LIGHTNING
	"DARK RAGE",//FP_RAGE
	"PROTECT",//FP_PROTECT
	"ABSORB",//FP_ABSORB
	"TEAM HEAL",//FP_TEAM_HEAL
	"TEAM REPLENISH",//FP_TEAM_FORCE
	"DRAIN",//FP_DRAIN
	"SEEING",//FP_SEE
	"SABER OFFENSE",//FP_SABER_OFFENSE
	"SABER DEFENSE",//FP_SABER_DEFENSE
	"SABER THROW",//FP_SABERTHROW
	NULL
};

/*
==================
SV_ForceToggle_f
==================
*/
void SV_ForceToggle_f(CvarSystem& cvars, CommandArgs& args)
{
	int i = 0;
	int fpDisabled = cvars.VariableValue("g_forcePowerDisable");
	int targetPower = 0;
	const char *powerDisabled = "Enabled";

	if ( args.Argc () < 2 )
	{ //no argument supplied, spit out a list of force powers and their numbers
		while (i < NUM_FORCE_POWERS)
		{
			if (fpDisabled & (1 << i))
			{
				powerDisabled = "Disabled";
			}
			else
			{
				powerDisabled = "Enabled";
			}

			Com_Printf(va("%i - %s - Status: %s\n", i, forceToggleNamePrints[i], powerDisabled));
			i++;
		}

		Com_Printf("Example usage: forcetoggle 3\n(toggles PUSH)\n");
		return;
	}

	targetPower = atoi(args.Argv(1));

	if (targetPower < 0 || targetPower >= NUM_FORCE_POWERS)
	{
		Com_Printf("Specified a power that does not exist.\nExample usage: forcetoggle 3\n(toggles PUSH)\n");
		return;
	}

	if (fpDisabled & (1 << targetPower))
	{
		powerDisabled = "enabled";
		fpDisabled &= ~(1 << targetPower);
	}
	else
	{
		powerDisabled = "disabled";
		fpDisabled |= (1 << targetPower);
	}

	cvars.Set("g_forcePowerDisable", va("%i", fpDisabled));

	Com_Printf("%s has been %s.\n", forceToggleNamePrints[targetPower], powerDisabled);
}

/*
==================
SV_Heartbeat_f

Also called by SV_DropClient, SV_DirectConnect, and SV_SpawnServer
==================
*/
void SV_Heartbeat_f(void) {
	svs.nextHeartbeatTime = -9999999;
}


/*
===========
SV_Serverinfo_f

Examine the serverinfo string
===========
*/
static void SV_Serverinfo_f(CvarSystem& cvars) {
	Com_Printf("Server info settings:\n");
	auto info = cvars.InfoString(CVAR_SERVERINFO);
	Info_Print(info.data());
	if ( !com_sv_running->integer() ) {
		Com_Printf( "Server is not running.\n" );
	}
}


/*
===========
SV_Systeminfo_f

Examine or change the serverinfo string
===========
*/
static void SV_Systeminfo_f(CvarSystem& cvars) {
	Com_Printf("System info settings:\n");
	auto info = cvars.InfoString(CVAR_SYSTEMINFO);
	Info_Print(info.data());
}


/*
===========
SV_DumpUser_f

Examine all a users info strings FIXME: move to game
===========
*/
static void SV_DumpUser_f(CommandArgs& args) {
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer() ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( args.Argc() != 2 ) {
		Com_Printf ("Usage: info <userid>\n");
		return;
	}

	cl = SV_GetPlayerByName(args);
	if ( !cl ) {
		return;
	}

	Com_Printf( "userinfo\n" );
	Com_Printf( "--------\n" );
	Info_Print( cl->userinfo );
}


/*
=================
SV_KillServer
=================
*/
static void SV_KillServer_f( void ) {
	SV_Shutdown( "killserver" );
}

//===========================================================

/*
==================
SV_AddOperatorCommands
==================
*/
void SV_AddOperatorCommands(CvarSystem& cvars, CommandSystem& cmd) {
	static qboolean	initialized;

	if ( initialized ) {
		return;
	}
	initialized = qtrue;

	cmd.AddCommand ("heartbeat", [](auto& args) {
		SV_Heartbeat_f();
	});
	cmd.AddCommand ("kick", SV_Kick_f);
#ifdef USE_CD_KEY
	cmd.AddCommand ("banUser", SV_Ban_f);
	cmd.AddCommand ("banClient", SV_BanNum_f);
#endif	// USE_CD_KEY

	cmd.AddCommand ("clientkick", SV_KickNum_f);
	cmd.AddCommand ("status", SV_Status_f);
	cmd.AddCommand ("serverinfo", [&](auto& args) {
		SV_Serverinfo_f(cvars);
	});
	cmd.AddCommand ("systeminfo", [&](auto& args) {
		SV_Systeminfo_f(cvars);
	});
	cmd.AddCommand ("dumpuser", SV_DumpUser_f);
	cmd.AddCommand ("map_restart", [&](auto& args) {
		SV_MapRestart_f(cvars, args);
	});
	cmd.AddCommand ("sectorlist", [](auto& args) {
		SV_SectorList_f();
	});
	cmd.AddCommand ("map", [&](auto& args) {
		SV_Map_f(cvars, args);
	});
#ifndef PRE_RELEASE_DEMO
	cmd.AddCommand ("devmap", [&](auto& args) {
		SV_Map_f(cvars, args);
	});
	cmd.AddCommand ("spmap", [&](auto& args) {
		SV_Map_f(cvars, args);
	});
	cmd.AddCommand ("spdevmap", [&](auto& args) {
		SV_Map_f(cvars, args);
	});
	cmd.AddCommand ("devmapmdl", [&](auto& args) {
		SV_Map_f(cvars, args);
	});
	cmd.AddCommand ("devmapall", [&](auto& args) {
		SV_Map_f(cvars, args);
	});
//	cmd.AddCommand ("devmapbsp", SV_Map_f);	// not used in MP codebase, no server BSP_cacheing
#endif
	cmd.AddCommand ("killserver", [](auto& args) {
		SV_KillServer_f();
	});
//	if( com_dedicated->integer ) 
	{
		cmd.AddCommand ("svsay", SV_ConSay_f);
	}

	cmd.AddCommand ("forcetoggle", [&](auto& args) {
		SV_ForceToggle_f(cvars, args);
	});
}

/*
==================
SV_RemoveOperatorCommands
==================
*/
void SV_RemoveOperatorCommands( void ) {
#if 0
	// removing these won't let the server start again
	Cmd_RemoveCommand ("heartbeat");
	Cmd_RemoveCommand ("kick");
	Cmd_RemoveCommand ("banUser");
	Cmd_RemoveCommand ("banClient");
	Cmd_RemoveCommand ("status");
	Cmd_RemoveCommand ("serverinfo");
	Cmd_RemoveCommand ("systeminfo");
	Cmd_RemoveCommand ("dumpuser");
	Cmd_RemoveCommand ("map_restart");
	Cmd_RemoveCommand ("sectorlist");
	Cmd_RemoveCommand ("svsay");
#endif
}

