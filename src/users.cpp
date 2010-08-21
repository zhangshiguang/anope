/* Routines to maintain a list of online users.
 *
 * (C) 2003-2010 Anope Team
 * Contact us at team@anope.org
 *
 * Please read COPYING and README for further details.
 *
 * Based on the original code of Epona by Lara.
 * Based on the original code of Services by Andy Church.
 */

#include "services.h"
#include "modules.h"
#include "language.h"

/* Hash maps used for users. Note UserListByUID will not be used on non-TS6 IRCds, and should never
 * be assumed to have users
 */
user_map UserListByNick;
user_uid_map UserListByUID;

int32 opcnt = 0;
uint32 usercnt = 0, maxusercnt = 0;
time_t maxusertime;

/*************************************************************************/
/*************************************************************************/

User::User(const Anope::string &snick, const Anope::string &suid)
{
	if (snick.empty())
		throw CoreException("what the craq, empty nick passed to constructor");

	// XXX: we should also duplicate-check here.

	/* we used to do this by calloc, no more. */
	server = NULL;
	nc = NULL;
	invalid_pw_count = timestamp = my_signon = invalid_pw_time = lastmemosend = lastnickreg = lastmail = 0;
	OnAccess = false;

	this->nick = snick;
	this->uid = suid;

	UserListByNick[snick] = this;
	if (!suid.empty())
		UserListByUID[suid] = this;

	this->nc = NULL;

	++usercnt;

	if (usercnt > maxusercnt)
	{
		maxusercnt = usercnt;
		maxusertime = time(NULL);
		if (Config->LogMaxUsers)
			Alog() << "user: New maximum user count: "<< maxusercnt;
	}

	this->isSuperAdmin = 0;	 /* always set SuperAdmin to 0 for new users */
}

void User::SetNewNick(const Anope::string &newnick)
{
	/* Sanity check to make sure we don't segfault */
	if (newnick.empty())
		throw CoreException("User::SetNewNick() got a bad argument");

	Alog(LOG_DEBUG) << this->nick << " changed nick to " << newnick;

	UserListByNick.erase(this->nick);

	this->nick = newnick;

	UserListByNick[this->nick] = this;

	OnAccess = false;
	NickAlias *na = findnick(this->nick);
	if (na)
		OnAccess = is_on_access(this, na->nc);
}

void User::SetDisplayedHost(const Anope::string &shost)
{
	if (shost.empty())
		throw CoreException("empty host? in MY services? it seems it's more likely than I thought.");

	this->vhost = shost;

	Alog(LOG_DEBUG) << this->nick << " changed vhost to " << shost;

	this->UpdateHost();
}

/** Get the displayed vhost of a user record.
 * @return The displayed vhost of the user, where ircd-supported, or the user's real host.
 */
const Anope::string &User::GetDisplayedHost() const
{
	if (ircd->vhost && !this->vhost.empty())
		return this->vhost;
	else if (this->HasMode(UMODE_CLOAK) && !this->GetCloakedHost().empty())
		return this->GetCloakedHost();
	else
		return this->host;
}

/** Update the cloaked host of a user
 * @param host The cloaked host
 */
void User::SetCloakedHost(const Anope::string &newhost)
{
	if (newhost.empty())
		throw "empty host in User::SetCloakedHost";

	chost = newhost;

	Alog(LOG_DEBUG) << this->nick << " changed cloaked host to " << newhost;

	this->UpdateHost();
}

/** Get the cloaked host of a user
 * @return The cloaked host
 */
const Anope::string &User::GetCloakedHost() const
{
	return chost;
}

const Anope::string &User::GetUID() const
{
	return this->uid;
}

void User::SetVIdent(const Anope::string &sident)
{
	this->vident = sident;

	Alog(LOG_DEBUG) << this->nick << " changed vident to " << sident;

	this->UpdateHost();
}

const Anope::string &User::GetVIdent() const
{
	if (this->HasMode(UMODE_CLOAK) || (ircd->vident && !this->vident.empty()))
		return this->vident;
	else
		return this->ident;
}

void User::SetIdent(const Anope::string &sident)
{
	this->ident = sident;

	Alog(LOG_DEBUG) << this->nick << " changed real ident to " << sident;

	this->UpdateHost();
}

const Anope::string &User::GetIdent() const
{
	return this->ident;
}

Anope::string User::GetMask() const
{
	return this->nick + "!" + this->ident + "@" + this->host;
}

void User::SetRealname(const Anope::string &srealname)
{
	if (srealname.empty())
		throw CoreException("realname empty in SetRealname");

	this->realname = srealname;
	NickAlias *na = findnick(this->nick);

	if (na && (this->IsIdentified(true) || this->IsRecognized(true)))
		na->last_realname = srealname;

	Alog(LOG_DEBUG) << this->nick << " changed realname to " << srealname;
}

User::~User()
{
	Alog(LOG_DEBUG_2) << "User::~User() called";

	this->Logout();

	if (Config->LogUsers)
	{
		Anope::string srealname = normalizeBuffer(this->realname);

		Alog() << "LOGUSERS: " << this->nick << " (" << this->GetIdent() << "@" << this->host << (ircd->vhost ? " => " : "") << (ircd->vhost ? this->GetDisplayedHost() : "") << ") (" << srealname << ") left the network (" << this->server->GetName() << ").";
	}

	FOREACH_MOD(I_OnUserLogoff, OnUserLogoff(this));

	--usercnt;

	if (is_oper(this))
		--opcnt;

	while (!this->chans.empty())
		this->chans.front()->chan->DeleteUser(this);

	if (Config->LimitSessions && !this->server->IsULined())
		del_session(this->host);

	UserListByNick.erase(this->nick);
	if (!this->uid.empty())
		UserListByUID.erase(this->uid);

	NickAlias *na = findnick(this->nick);
	if (na)
		na->OnCancel(this);

	Alog(LOG_DEBUG_2) << "User::~User() done";
}

void User::SendMessage(const Anope::string &source, const char *fmt, ...) const
{
	va_list args;
	char buf[BUFSIZE] = "";

	if (fmt)
	{
		va_start(args, fmt);
		vsnprintf(buf, BUFSIZE - 1, fmt, args);

		this->SendMessage(source, Anope::string(buf));

		va_end(args);
	}
}

void User::SendMessage(const Anope::string &source, const Anope::string &msg) const
{
	/* Send privmsg instead of notice if:
	* - UsePrivmsg is enabled
	* - The user is not registered and NSDefMsg is enabled
	* - The user is registered and has set /ns set msg on
	*/
	if (Config->UsePrivmsg && ((!this->nc && Config->NSDefFlags.HasFlag(NI_MSG)) || (this->nc && this->nc->HasFlag(NI_MSG))))
		ircdproto->SendPrivmsg(findbot(source), this->nick, "%s", msg.c_str());
	else
		ircdproto->SendNotice(findbot(source), this->nick, "%s", msg.c_str());
}

/** Collides a nick.
 *
 * First, it marks the nick (if the user is on a registered nick, we don't use it without but it could be)
 * as COLLIDED, this is checked in NickAlias::OnCancel.
 *
 * Then it does one of two things.
 *
 * 1. This will force change the users nick to the guest nick. This gets processed by the IRCd and comes
 *    back to call do_nick. do_nick changes the nick of the use to the new one, then calls NickAlias::OnCancel
 *    with the users old nick's nickalias (if there is one).
 *
 * 2. Calls kill_user, which will either delete the user immediatly or kill them, wait for the QUIT,
 *    then delete the user then. Users destructor then calls NickAlias::OnCancel
 *
 * NickAlias::OnCancel checks for NS_COLLIDED, it then does one of two things.
 *
 * 1. If supported, we send a SVSHold for the user. We are done here, the IRCds expires this at the time we give it.
 *
 * 2. We create a new client with SendClientIntroduction(). Note that is it important that this is called either after the
 *    user has been removed from our internal list of user or after the users nick has been updated completely internally.
 *    This is beacuse SendClientIntroduction will destroy any users we think are currently on the nickname (which causes a
 *    lot of problems, eg, deleting the user which recalls OnCancel), whether they really are or not. We then create a
 *    release timer for this new client that waits and later on sends a QUIT for the client. Release timers are never used
 *    for SVSHolds. Ever.
 *
 *
 *  Note that now for the timers we only store the users name, not the NickAlias* pointer. We never remove timers when
 *  a user changes nick or a nick is deleted, the timers must assume that either of these may have happend.
 *
 *  Storing NickAlias* pointers caused quite a problem, some of which are:
 *
 *  Having a valid timer alive that calls User::Collide would either:
 *
 *  1. Kill the user, causing users destructor to cancel all timers for the nick (as it should, it has no way of knowing
 *     if we are in a timer or not) which would delete the currently active timer while it was running, causing TimerManager
 *     to explode.
 *
 *  2. Force a user off of their nick, this would call NickAlias::Cancel before updating the user internally (to cancel the
 *     current nicks timers, granted we could have easially saved this and called it after) which could possibly try to
 *     introduce an enforcer nick. We would then check to see if the nick is already in use (it is, internally) and send
 *     a kill for that nick. That may in turn delete the user immediatly, calling users destructor, which would attempt to
 *     delete the timer, causing TimerManager to explode.
 *
 *     Additionally, if we marked the timer as "in use" so that calling the ClearTimer function wouldn't delete them, users
 *     destructor would then call NickAlias::OnCancel, which would (at this point, it was unsetting GUESTED after introducing
 *     the new client) introduce the same new client again, without actually deleting the originial user, causing an infinite
 *     loop.
 *
 *     This is why we remove NS_GUESTED first in NickAlias::OnCancel before introducing a new client, although this should
 *     not happen anymore. If I must emphasize this again, users need to be GONE from the internal list before calling
 *     NickAlias::OnCancel. NickAlias::OnCancel intentionally reffers to this->nick, not the user passed to it. They *can*
 *     (but not always) be different, depending if the user changed nicks or disconnected.
 *
 *
 *  Adam
 */
void User::Collide(NickAlias *na)
{
	if (na)
		na->SetFlag(NS_COLLIDED);

	if (ircd->svsnick)
	{
		Anope::string guestnick;

		do
		{
			guestnick = Config->NSGuestNickPrefix + stringify(getrandom16());
		} while (finduser(guestnick));

		notice_lang(Config->s_NickServ, this, FORCENICKCHANGE_CHANGING, guestnick.c_str());
		ircdproto->SendForceNickChange(this, guestnick, time(NULL));
	}
	else
		kill_user(Config->s_NickServ, this->nick, "Services nickname-enforcer kill");
}

/** Login the user to a NickCore
 * @param core The account the user is useing
 */
void User::Login(NickCore *core)
{
	this->Logout();
	this->nc = core;
	core->Users.push_back(this);

	this->UpdateHost();
	check_memos(this);
}

/** Logout the user
 */
void User::Logout()
{
	if (!this->nc)
		return;

	std::list<User *>::iterator it = std::find(this->nc->Users.begin(), this->nc->Users.end(), this);
	if (it != this->nc->Users.end())
		this->nc->Users.erase(it);

	this->nc = NULL;
}

/** Get the account the user is logged in using
 * @reurn The account or NULL
 */
NickCore *User::Account()
{
	return this->nc;
}

const NickCore *User::Account() const
{
	return this->nc;
}

/** Check if the user is identified for their nick
 * @param CheckNick True to check if the user is identified to the nickname they are on too
 * @return true or false
 */
bool User::IsIdentified(bool CheckNick) const
{
	if (CheckNick && this->nc)
	{
		NickAlias *na = findnick(this->nc->display);

		if (na && na->nc == this->nc)
			return true;

		return false;
	}

	return this->nc ? true : false;
}

/** Check if the user is recognized for their nick (on the nicks access list)
 * @param CheckSecure Only returns true if the user has secure off
 * @return true or false
 */
bool User::IsRecognized(bool CheckSecure) const
{
	if (CheckSecure && OnAccess)
	{
		NickAlias *na = findnick(this->nick);

		if (!na || !na->nc->HasFlag(NI_SECURE))
			return false;
	}

	return OnAccess;
}

/** Update the last usermask stored for a user, and check to see if they are recognized
 */
void User::UpdateHost()
{
	if (this->host.empty())
		return;

	NickAlias *na = findnick(this->nick);
	OnAccess = false;
	if (na)
		OnAccess = is_on_access(this, na->nc);

	if (na && (this->IsIdentified(true) || this->IsRecognized(true)))
	{
		Anope::string last_usermask = this->GetIdent() + "@" + this->GetDisplayedHost();
		na->last_usermask = last_usermask;
	}
}

/** Check if the user has a mode
 * @param Name Mode name
 * @return true or false
 */
bool User::HasMode(UserModeName Name) const
{
	return this->modes.HasFlag(Name);
}

/** Set a mode internally on the user, the IRCd is not informed
 * @param um The user mode
 * @param Param The param, if there is one
 */
void User::SetModeInternal(UserMode *um, const Anope::string &Param)
{
	if (!um)
		return;

	this->modes.SetFlag(um->Name);
	if (!Param.empty())
		Params.insert(std::make_pair(um->Name, Param));

	FOREACH_MOD(I_OnUserModeSet, OnUserModeSet(this, um->Name));
}

/** Remove a mode internally on the user, the IRCd is not informed
 * @param um The user mode
 */
void User::RemoveModeInternal(UserMode *um)
{
	if (!um)
		return;

	this->modes.UnsetFlag(um->Name);
	std::map<UserModeName, Anope::string>::iterator it = Params.find(um->Name);
	if (it != Params.end())
		Params.erase(it);

	FOREACH_MOD(I_OnUserModeUnset, OnUserModeUnset(this, um->Name));
}

/** Set a mode on the user
 * @param bi The client setting the mode
 * @param um The user mode
 * @param Param Optional param for the mode
 */
void User::SetMode(BotInfo *bi, UserMode *um, const Anope::string &Param)
{
	if (!um || HasMode(um->Name))
		return;

	ModeManager::StackerAdd(bi, this, um, true, Param);
	SetModeInternal(um, Param);
}

/** Set a mode on the user
 * @param bi The client setting the mode
 * @param Name The mode name
 * @param param Optional param for the mode
 */
void User::SetMode(BotInfo *bi, UserModeName Name, const Anope::string &Param)
{
	SetMode(bi, ModeManager::FindUserModeByName(Name), Param);
}

/* Set a mode on the user
 * @param bi The client setting the mode
 * @param ModeChar The mode char
 * @param param Optional param for the mode
 */
void User::SetMode(BotInfo *bi, char ModeChar, const Anope::string &Param)
{
	SetMode(bi, ModeManager::FindUserModeByChar(ModeChar), Param);
}

/** Remove a mode on the user
 * @param bi The client setting the mode
 * @param um The user mode
 */
void User::RemoveMode(BotInfo *bi, UserMode *um)
{
	if (!um || !HasMode(um->Name))
		return;

	ModeManager::StackerAdd(bi, this, um, false);
	RemoveModeInternal(um);
}

/** Remove a mode from the user
 * @param bi The client setting the mode
 * @param Name The mode name
 */
void User::RemoveMode(BotInfo *bi, UserModeName Name)
{
	RemoveMode(bi, ModeManager::FindUserModeByName(Name));
}

/** Remove a mode from the user
 * @param bi The client setting the mode
 * @param ModeChar The mode char
 */
void User::RemoveMode(BotInfo *bi, char ModeChar)
{
	RemoveMode(bi, ModeManager::FindUserModeByChar(ModeChar));
}

/** Set a string of modes on a user
 * @param bi The client setting the mode
 * @param umodes The modes
 */
void User::SetModes(BotInfo *bi, const char *umodes, ...)
{
	char buf[BUFSIZE] = "";
	va_list args;
	Anope::string modebuf, sbuf;
	int add = -1;
	va_start(args, umodes);
	vsnprintf(buf, BUFSIZE - 1, umodes, args);
	va_end(args);

	spacesepstream sep(buf);
	sep.GetToken(modebuf);
	for (unsigned i = 0, end = modebuf.length(); i < end; ++i)
	{
		UserMode *um;

		switch (modebuf[i])
		{
			case '+':
				add = 1;
				continue;
			case '-':
				add = 0;
				continue;
			default:
				if (add == -1)
					continue;
				um = ModeManager::FindUserModeByChar(modebuf[i]);
				if (!um)
					continue;
		}

		if (add)
		{
			if (um->Type == MODE_PARAM && sep.GetToken(sbuf))
				this->SetMode(bi, um, sbuf);
			else
				this->SetMode(bi, um);
		}
		else
			this->RemoveMode(bi, um);
	}
}

/** Find the channel container for Channel c that the user is on
 * This is preferred over using FindUser in Channel, as there are usually more users in a channel
 * than channels a user is in
 * @param c The channel
 * @return The channel container, or NULL
 */
ChannelContainer *User::FindChannel(const Channel *c)
{
	for (UChannelList::const_iterator it = this->chans.begin(), it_end = this->chans.end(); it != it_end; ++it)
		if ((*it)->chan == c)
			return *it;
	return NULL;
}

/** Check if the user is protected from kicks and negative mode changes
 * @return true or false
 */
bool User::IsProtected() const
{
	if (this->HasMode(UMODE_PROTECTED) || this->HasMode(UMODE_GOD))
		return true;

	return false;
}

/*************************************************************************/

void get_user_stats(long &count, long &mem)
{
	count = mem = 0;

	for (user_map::const_iterator it = UserListByNick.begin(), it_end = UserListByNick.end(); it != it_end; ++it)
	{
		User *user = it->second;

		++count;
		mem += sizeof(*user);
		if (!user->host.empty())
			mem += user->host.length() + 1;
		if (ircd->vhost)
		{
			if (!user->vhost.empty())
				mem += user->vhost.length() + 1;
		}
		if (!user->realname.empty())
			mem += user->realname.length() + 1;
		mem += user->server->GetName().length() + 1;
		mem += (sizeof(ChannelContainer) * user->chans.size());
	}
}

User *finduser(const Anope::string &nick)
{
	if (isdigit(nick[0]) && ircd->ts6)
	{
		user_uid_map::const_iterator it = UserListByUID.find(nick);

		if (it != UserListByUID.end())
			return it->second;
		return NULL;
	}

	user_map::const_iterator it = UserListByNick.find(nick);

	if (it != UserListByNick.end())
		return it->second;
	return NULL;
}

/*************************************************************************/

/* Handle a server NICK command. */

User *do_nick(const Anope::string &source, const Anope::string &nick, const Anope::string &username, const Anope::string &host, const Anope::string &server, const Anope::string &realname, time_t ts, uint32 ip, const Anope::string &vhost, const Anope::string &uid)
{
	User *user = NULL;
	Anope::string vhost2 = vhost;

	if (source.empty())
	{
		char ipbuf[16];
		struct in_addr addr;

		if (ircd->nickvhost)
		{
			if (!vhost2.empty())
			{
				if (vhost2.equals_cs("*"))
				{
					vhost2.clear();
					Alog(LOG_DEBUG) << "new user with no vhost in NICK command: " << nick;
				}
			}
		}

		/* This is a new user; create a User structure for it. */
		Alog(LOG_DEBUG) << "new user: " << nick;

		if (ircd->nickip)
		{
			addr.s_addr = htonl(ip);
			ntoa(addr, ipbuf, sizeof(ipbuf));
		}

		Server *serv = Server::Find(server);

		if (Config->LogUsers)
		{
			/**
			 * Ugly swap routine for Flop's bug :)   XXX
			 **/
			Anope::string logrealname = realname;
			if (!logrealname.empty())
			{
				size_t tmp;
				while ((tmp = logrealname.find('%')) != Anope::string::npos)
					logrealname[tmp] = '-';
			}
			logrealname = normalizeBuffer(logrealname);

			/**
			 * End of ugly swap
			 **/
			Alog() << "LOGUSERS: " << nick << " (" << username << "@" << host << (ircd->nickvhost && !vhost2.empty() ? " => " : "") << (ircd->nickvhost && !vhost2.empty() ? vhost2 : "") << ") (" << logrealname << ") "
				<< (ircd->nickip ? "[" : "") << (ircd->nickip ? ipbuf : "") << (ircd->nickip ? "]" : "") << " connected to the network (" << serv->GetName() << ").";
		}

		/* Allocate User structure and fill it in. */
		user = new User(nick, !uid.empty() ? uid : "");
		user->SetIdent(username);
		user->host = host;
		user->server = serv;
		user->realname = realname;
		user->timestamp = ts;
		user->my_signon = time(NULL);
		if (!vhost2.empty())
			user->SetCloakedHost(vhost2);
		user->SetVIdent(username);
		/* We now store the user's ip in the user_ struct,
		 * because we will use it in serveral places -- DrStein */
		if (ircd->nickip)
			user->hostip = ipbuf;
		else
			user->hostip = "";

		EventReturn MOD_RESULT;
		FOREACH_RESULT(I_OnPreUserConnect, OnPreUserConnect(user));
		if (MOD_RESULT == EVENT_STOP)
			return finduser(nick);

		if (Config->LimitSessions && !serv->IsULined())
			add_session(nick, host, ipbuf);

		XLineManager::CheckAll(user);

		/* User is no longer connected, return */
		if (!finduser(nick))
			return NULL;

		FOREACH_MOD(I_OnUserConnect, OnUserConnect(user));
	}
	else
	{
		/* An old user changing nicks. */
		user = finduser(source);

		if (!user)
		{
			Alog() << "user: NICK from nonexistent nick " << source;
			return NULL;
		}
		user->isSuperAdmin = 0; /* Dont let people nick change and stay SuperAdmins */
		Alog(LOG_DEBUG) << source << " changes nick to " <<  nick;

		if (Config->LogUsers)
		{
			Anope::string logrealname = normalizeBuffer(user->realname);
			Alog() << "LOGUSERS: " << user->nick << " (" << user->GetIdent() << "@" << user->host << (ircd->vhost ? " => " : "") << (ircd->vhost ? user->GetDisplayedHost() : "") << ") (" << logrealname << ") changed nick to "
				<< nick << " (" << user->server->GetName() << ").";
		}

		user->timestamp = ts;

		if (user->nick.equals_ci(nick))
			/* No need to redo things */
			user->SetNewNick(nick);
		else
		{
			/* Update this only if nicks aren't the same */
			user->my_signon = time(NULL);

			NickAlias *old_na = findnick(user->nick);
			if (old_na && (old_na->nc == user->Account() || user->IsRecognized()))
				old_na->last_seen = time(NULL);

			Anope::string oldnick = user->nick;
			user->SetNewNick(nick);
			FOREACH_MOD(I_OnUserNickChange, OnUserNickChange(user, oldnick));

			if (old_na)
				old_na->OnCancel(user);

			NickAlias *na = findnick(user->nick);
			/* If the new nick isnt registerd or its registerd and not yours */
			if (!na || na->nc != user->Account())
			{
				user->RemoveMode(NickServ, UMODE_REGISTERED);
				ircdproto->SendUnregisteredNick(user);

				validate_user(user);
			}
			else
			{
				na->last_seen = time(NULL);
				user->UpdateHost();
				do_on_id(user);
				ircdproto->SetAutoIdentificationToken(user);
				Alog() << Config->s_NickServ << ": " << user->GetMask() << " automatically identified for group " << user->Account()->display;
			}

			if (ircd->sqline)
			{
				if (!is_oper(user) && SQLine->Check(user))
					return NULL;
			}
		}
	}

	return user;
}

/*************************************************************************/

/* Handle a MODE command for a user.
 *  av[0] = nick to change mode for
 *  av[1] = modes
 */

void do_umode(const Anope::string &source, int ac, const char **av)
{
	User *user = finduser(av[0]);
	if (!user)
	{
		Alog() << "user: MODE "<< av[1] << " for nonexistent nick "<< av[0] << ":" << merge_args(ac, av);
		return;
	}

	UserSetInternalModes(user, ac - 1, &av[1]);
}

/*************************************************************************/

/* Handle a QUIT command.
 *  av[0] = reason
 */

void do_quit(const Anope::string &source, int ac, const char **av)
{
	User *user = finduser(source);
	if (!user)
	{
		Alog() << "user: QUIT from nonexistent user " << source << ":" << merge_args(ac, av);
		return;
	}
	Alog(LOG_DEBUG) << source << " quits";

	NickAlias *na = findnick(user->nick);
	if (na && !na->HasFlag(NS_FORBIDDEN) && !na->nc->HasFlag(NI_SUSPENDED) && (user->IsRecognized() || user->IsIdentified(true)))
	{
		na->last_seen = time(NULL);
		na->last_quit = *av[0] ? av[0] : "";
	}
	FOREACH_MOD(I_OnUserQuit, OnUserQuit(user, *av[0] ? av[0] : ""));
	delete user;
}

/*************************************************************************/

/* Handle a KILL command.
 *  av[0] = nick being killed
 *  av[1] = reason
 */

void do_kill(const Anope::string &nick, const Anope::string &msg)
{
	User *user = finduser(nick);
	if (!user)
	{
		Alog(LOG_DEBUG) << "KILL of nonexistent nick: " <<  nick;
		return;
	}
	Alog(LOG_DEBUG) << nick << " killed";

	NickAlias *na = findnick(user->nick);
	if (na && !na->HasFlag(NS_FORBIDDEN) && !na->nc->HasFlag(NI_SUSPENDED) && (user->IsRecognized() || user->IsIdentified(true)))
	{
		na->last_seen = time(NULL);
		na->last_quit = msg;
	}
	delete user;
}

/*************************************************************************/
/*************************************************************************/

/* Is the given nick an oper? */

bool is_oper(User *user)
{
	return user && user->HasMode(UMODE_OPER);
}

/*************************************************************************/
/*************************************************************************/

/* Is the given user ban-excepted? */
bool is_excepted(ChannelInfo *ci, User *user)
{
	if (!ci->c || !ModeManager::FindChannelModeByName(CMODE_EXCEPT))
		return false;

	return elist_match_user(ci->c->excepts, user);
}

/*************************************************************************/

/* Is the given MASK ban-excepted? */
bool is_excepted_mask(ChannelInfo *ci, const Anope::string &mask)
{
	if (!ci->c || !ModeManager::FindChannelModeByName(CMODE_EXCEPT))
		return false;

	return elist_match_mask(ci->c->excepts, mask, 0);
}

/*************************************************************************/

/* Does the user's usermask match the given mask (either nick!user@host or
 * just user@host)?
 */

bool match_usermask(const Anope::string &mask, User *user)
{
	if (mask.empty())
		return false;

	Anope::string mask2 = mask, nick, username, host;
	size_t ex = mask2.find('!');
	if (ex != Anope::string::npos)
	{
		nick = mask2.substr(0, ex);
		mask2 = mask2.substr(ex + 1);
	}
	size_t at = mask2.find('@');
	if (at != Anope::string::npos)
	{
		username = mask2.substr(0, at);
		host = mask2.substr(at + 1);
	}
	if (username.empty() || host.empty())
		return 0;

	return (nick.empty() ? true : Anope::Match(user->nick, nick)) && Anope::Match(user->GetIdent(), username) && (Anope::Match(user->host, host) || Anope::Match(user->GetDisplayedHost(), host));
}

/*************************************************************************/

/* Given a user, return a mask that will most likely match any address the
 * user will have from that location.  For IP addresses, wildcards the
 * appropriate subnet mask (e.g. 35.1.1.1 -> 35.*; 128.2.1.1 -> 128.2.*);
 * for named addresses, wildcards the leftmost part of the name unless the
 * name only contains two parts.  If the username begins with a ~, delete
 * it.
 */

Anope::string create_mask(User *u)
{
	Anope::string mask;
	Anope::string mident = u->GetIdent();
	Anope::string mhost = u->GetDisplayedHost();

	/* Get us a buffer the size of the username plus hostname.  The result
	 * will never be longer than this (and will often be shorter), thus we
	 * can use strcpy() and sprintf() safely.
	 */
	if (mident[0] == '~')
		mask = "*" + mident + "@";
	else
		mask = mident + "@";

	size_t dot;
	/* To make sure this is an IP, make sure the host contains only numbers and dots, and check to make sure it only contains 3 dots */
	if (mhost.find_first_not_of("0123456789.") == Anope::string::npos && (dot = mhost.find('.')) != Anope::string::npos && (dot = mhost.find('.', dot + 1)) != Anope::string::npos && (dot = mhost.find('.', dot + 1)) != Anope::string::npos && mhost.find('.', dot + 1) == Anope::string::npos)
	{ /* IP addr */
		dot = mhost.find('.');
		mask += mhost.substr(0, dot) + ".*";
	}
	else
	{
		if ((dot = mhost.find('.')) != Anope::string::npos && mhost.find('.', dot + 1) != Anope::string::npos)
			mask += "*" + mhost.substr(dot);
		else
			mask += mhost;
	}
	return mask;
}

/*************************************************************************/

/** Set modes internally on a user
 * @param user The user
 * @param ac Number of args
 * @param av Args
 */
void UserSetInternalModes(User *user, int ac, const char **av)
{
	int add = -1, j = 0;
	const char *modes = av[0];
	if (!user || !modes)
		return;

	Alog(LOG_DEBUG) << "Changing user modes for " << user->nick << " to " << merge_args(ac, av);

	for (; *modes; ++modes)
	{
		UserMode *um;

		switch (*modes)
		{
			case '+':
				add = 1;
				continue;
			case '-':
				add = 0;
				continue;
			default:
				if (add == -1)
					continue;
				um = ModeManager::FindUserModeByChar(*modes);
				if (!um)
					continue;
		}

		if (um->Type == MODE_REGULAR)
		{
			if (add)
				user->SetModeInternal(um);
			else
				user->RemoveModeInternal(um);
		}
		else if (++j < ac)
		{
			if (add)
				user->SetModeInternal(um, av[j]);
			else
				user->RemoveModeInternal(um);
		}

		switch (um->Name)
		{
			case UMODE_OPER:
				if (add)
				{
					++opcnt;
					if (Config->WallOper)
						ircdproto->SendGlobops(OperServ, "\2%s\2 is now an IRC operator.", user->nick.c_str());
				}
				else
					--opcnt;
				break;
			case UMODE_REGISTERED:
				if (add && !user->IsIdentified())
					user->RemoveMode(NickServ, UMODE_REGISTERED);
				break;
			case UMODE_CLOAK:
			case UMODE_VHOST:
				if (!add && !user->vhost.empty())
					user->vhost.clear();
				user->UpdateHost();
			default:
				break;
		}
	}
}