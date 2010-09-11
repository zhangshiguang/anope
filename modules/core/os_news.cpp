/* OperServ core functions
 *
 * (C) 2003-2010 Anope Team
 * Contact us at info@anope.org
 *
 * Please read COPYING and README for further details.
 *
 * Based on the original code of Epona by Lara.
 * Based on the original code of Services by Andy Church.
 */

/*************************************************************************/

#include "module.h"

#define lenof(a)        (sizeof(a) / sizeof(*(a)))

/* List of messages for each news type.  This simplifies message sending. */

enum
{
	MSG_SYNTAX,
	MSG_LIST_HEADER,
	MSG_LIST_ENTRY,
	MSG_LIST_NONE,
	MSG_ADD_SYNTAX,
	MSG_ADD_FULL,
	MSG_ADDED,
	MSG_DEL_SYNTAX,
	MSG_DEL_NOT_FOUND,
	MSG_DELETED,
	MSG_DEL_NONE,
	MSG_DELETED_ALL
};

struct newsmsgs msgarray[] = {
	{NEWS_LOGON, "LOGON",
	 {NEWS_LOGON_SYNTAX,
	  NEWS_LOGON_LIST_HEADER,
	  NEWS_LOGON_LIST_ENTRY,
	  NEWS_LOGON_LIST_NONE,
	  NEWS_LOGON_ADD_SYNTAX,
	  NEWS_LOGON_ADD_FULL,
	  NEWS_LOGON_ADDED,
	  NEWS_LOGON_DEL_SYNTAX,
	  NEWS_LOGON_DEL_NOT_FOUND,
	  NEWS_LOGON_DELETED,
	  NEWS_LOGON_DEL_NONE,
	  NEWS_LOGON_DELETED_ALL}
	 },
	{NEWS_OPER, "OPER",
	 {NEWS_OPER_SYNTAX,
	  NEWS_OPER_LIST_HEADER,
	  NEWS_OPER_LIST_ENTRY,
	  NEWS_OPER_LIST_NONE,
	  NEWS_OPER_ADD_SYNTAX,
	  NEWS_OPER_ADD_FULL,
	  NEWS_OPER_ADDED,
	  NEWS_OPER_DEL_SYNTAX,
	  NEWS_OPER_DEL_NOT_FOUND,
	  NEWS_OPER_DELETED,
	  NEWS_OPER_DEL_NONE,
	  NEWS_OPER_DELETED_ALL}
	 },
	{NEWS_RANDOM, "RANDOM",
	 {NEWS_RANDOM_SYNTAX,
	  NEWS_RANDOM_LIST_HEADER,
	  NEWS_RANDOM_LIST_ENTRY,
	  NEWS_RANDOM_LIST_NONE,
	  NEWS_RANDOM_ADD_SYNTAX,
	  NEWS_RANDOM_ADD_FULL,
	  NEWS_RANDOM_ADDED,
	  NEWS_RANDOM_DEL_SYNTAX,
	  NEWS_RANDOM_DEL_NOT_FOUND,
	  NEWS_RANDOM_DELETED,
	  NEWS_RANDOM_DEL_NONE,
	  NEWS_RANDOM_DELETED_ALL}
	 }
};

static void DisplayNews(User *u, NewsType Type)
{
	int msg;
	static unsigned current_news = 0;

	if (Type == NEWS_LOGON)
		msg = NEWS_LOGON_TEXT;
	else if (Type == NEWS_OPER)
		msg = NEWS_OPER_TEXT;
	else if (Type == NEWS_RANDOM)
		msg = NEWS_RANDOM_TEXT;
	else
		throw CoreException("news: Invalid type (" + stringify(Type) + ") to display_news()");

	unsigned displayed = 0;
	bool NewsExists = false;
	for (unsigned i = 0, end = News.size(); i < end; ++i)
	{
		if (News[i]->type == Type)
		{
			tm *tm;
			char timebuf[64];

			NewsExists = true;

			if (Type == NEWS_RANDOM && i == current_news)
				continue;

			tm = localtime(&News[i]->time);
			strftime_lang(timebuf, sizeof(timebuf), u, STRFTIME_SHORT_DATE_FORMAT, tm);
			notice_lang(Config->s_GlobalNoticer, u, msg, timebuf, News[i]->Text.c_str());

			++displayed;

			if (Type == NEWS_RANDOM)
			{
				current_news = i;
				return;
			}
			else if (displayed >= Config->NewsCount)
				return;
		}

		/* Reset to head of list to get first random news value */
		if (i + 1 == News.size() && Type == NEWS_RANDOM && NewsExists)
			i = 0;
	}
}

static int add_newsitem(User *u, const Anope::string &text, NewsType type)
{
	int num = 0;

	for (unsigned i = News.size(); i > 0; --i)
		if (News[i - 1]->type == type)
		{
			num = News[i - 1]->num;
			break;
		}

	NewsItem *news = new NewsItem();
	news->type = type;
	news->num = num + 1;
	news->Text = text;
	news->time = Anope::CurTime;
	news->who = u->nick;

	News.push_back(news);

	return num + 1;
}

static int del_newsitem(unsigned num, NewsType type)
{
	int count = 0;

	for (unsigned i = News.size(); i > 0; --i)
		if (News[i - 1]->type == type && (num == 0 || News[i - 1]->num == num))
		{
			delete News[i - 1];
			News.erase(News.begin() + i - 1);
			++count;
		}

	return count;
}

static int *findmsgs(NewsType type, Anope::string &type_name)
{
	for (unsigned i = 0; i < lenof(msgarray); ++i)
		if (msgarray[i].type == type)
		{
			type_name = msgarray[i].name;
			return msgarray[i].msgs;
		}
	return NULL;
}

class NewsBase : public Command
{
 protected:
	CommandReturn DoList(User *u, NewsType type, int *msgs)
	{
		int count = 0;
		char timebuf[64];
		struct tm *tm;

		for (unsigned i = 0, end = News.size(); i < end; ++i)
			if (News[i]->type == type)
			{
				if (!count)
					notice_lang(Config->s_OperServ, u, msgs[MSG_LIST_HEADER]);
				tm = localtime(&News[i]->time);
				strftime_lang(timebuf, sizeof(timebuf), u, STRFTIME_DATE_TIME_FORMAT, tm);
				notice_lang(Config->s_OperServ, u, msgs[MSG_LIST_ENTRY], News[i]->num, timebuf, !News[i]->who.empty() ? News[i]->who.c_str() : "<unknown>", News[i]->Text.c_str());
				++count;
			}
		if (!count)
			notice_lang(Config->s_OperServ, u, msgs[MSG_LIST_NONE]);
		else
			notice_lang(Config->s_OperServ, u, END_OF_ANY_LIST, "News");

		return MOD_CONT;
	}

	CommandReturn DoAdd(User *u, const std::vector<Anope::string> &params, NewsType type, int *msgs)
	{
		Anope::string text = params.size() > 1 ? params[1] : "";
		int n;

		if (text.empty())
			this->OnSyntaxError(u, "ADD");
		else
		{
			if (readonly)
			{
				notice_lang(Config->s_OperServ, u, READ_ONLY_MODE);
				return MOD_CONT;
			}
			n = add_newsitem(u, text, type);
			if (n < 0)
				notice_lang(Config->s_OperServ, u, msgs[MSG_ADD_FULL]);
			else
				notice_lang(Config->s_OperServ, u, msgs[MSG_ADDED], n);
		}

		return MOD_CONT;
	}

	CommandReturn DoDel(User *u, const std::vector<Anope::string> &params, NewsType type, int *msgs)
	{
		Anope::string text = params.size() > 1 ? params[1] : "";
		unsigned num;

		if (text.empty())
			this->OnSyntaxError(u, "DEL");
		else
		{
			if (readonly)
			{
				notice_lang(Config->s_OperServ, u, READ_ONLY_MODE);
				return MOD_CONT;
			}
			if (!text.equals_ci("ALL"))
			{
				num = text.is_pos_number_only() ? convertTo<unsigned>(text) : 0;
				if (num > 0 && del_newsitem(num, type))
				{
					notice_lang(Config->s_OperServ, u, msgs[MSG_DELETED], num);
					for (unsigned i = 0, end = News.size(); i < end; ++i)
						if (News[i]->type == type && News[i]->num > num)
							--News[i]->num;
				}
				else
					notice_lang(Config->s_OperServ, u, msgs[MSG_DEL_NOT_FOUND], num);
			}
			else
			{
				if (del_newsitem(0, type))
					notice_lang(Config->s_OperServ, u, msgs[MSG_DELETED_ALL]);
				else
					notice_lang(Config->s_OperServ, u, msgs[MSG_DEL_NONE]);
			}
		}

		return MOD_CONT;
	}

	CommandReturn DoNews(User *u, const std::vector<Anope::string> &params, NewsType type)
	{
		Anope::string cmd = params[0];
		Anope::string type_name;
		int *msgs;

		msgs = findmsgs(type, type_name);
		if (!msgs)
			throw CoreException("news: Invalid type to do_news()");

		if (cmd.equals_ci("LIST"))
			return this->DoList(u, type, msgs);
		else if (cmd.equals_ci("ADD"))
			return this->DoAdd(u, params, type, msgs);
		else if (cmd.equals_ci("DEL"))
			return this->DoDel(u, params, type, msgs);
		else
			this->OnSyntaxError(u, "");

		return MOD_CONT;
	}
 public:
	NewsBase(const Anope::string &newstype) : Command(newstype, 1, 2, "operserv/news")
	{
	}

	virtual ~NewsBase()
	{
	}

	virtual CommandReturn Execute(User *u, const std::vector<Anope::string> &params) = 0;

	virtual bool OnHelp(User *u, const Anope::string &subcommand) = 0;

	virtual void OnSyntaxError(User *u, const Anope::string &subcommand) = 0;
};

class CommandOSLogonNews : public NewsBase
{
 public:
	CommandOSLogonNews() : NewsBase("LOGONNEWS")
	{
	}

	CommandReturn Execute(User *u, const std::vector<Anope::string> &params)
	{
		return this->DoNews(u, params, NEWS_LOGON);
	}

	bool OnHelp(User *u, const Anope::string &subcommand)
	{
		notice_help(Config->s_OperServ, u, NEWS_HELP_LOGON, Config->NewsCount);
		return true;
	}

	void OnSyntaxError(User *u, const Anope::string &subcommand)
	{
		syntax_error(Config->s_OperServ, u, "LOGONNEWS", NEWS_LOGON_SYNTAX);
	}

	void OnServHelp(User *u)
	{
		notice_lang(Config->s_OperServ, u, OPER_HELP_CMD_LOGONNEWS);
	}
};

class CommandOSOperNews : public NewsBase
{
 public:
	CommandOSOperNews() : NewsBase("OPERNEWS")
	{
	}

	CommandReturn Execute(User *u, const std::vector<Anope::string> &params)
	{
		return this->DoNews(u, params, NEWS_OPER);
	}

	bool OnHelp(User *u, const Anope::string &subcommand)
	{
		notice_help(Config->s_OperServ, u, NEWS_HELP_OPER, Config->NewsCount);
		return true;
	}

	void OnSyntaxError(User *u, const Anope::string &subcommand)
	{
		syntax_error(Config->s_OperServ, u, "OPERNEWS", NEWS_OPER_SYNTAX);
	}

	void OnServHelp(User *u)
	{
		notice_lang(Config->s_OperServ, u, OPER_HELP_CMD_OPERNEWS);
	}
};

class CommandOSRandomNews : public NewsBase
{
 public:
	CommandOSRandomNews() : NewsBase("RANDOMNEWS")
	{
	}

	CommandReturn Execute(User *u, const std::vector<Anope::string> &params)
	{
		return this->DoNews(u, params, NEWS_RANDOM);
	}

	bool OnHelp(User *u, const Anope::string &subcommand)
	{
		notice_help(Config->s_OperServ, u, NEWS_HELP_RANDOM);
		return true;
	}

	void OnSyntaxError(User *u, const Anope::string &subcommand)
	{
		syntax_error(Config->s_OperServ, u, "RANDOMNEWS", NEWS_RANDOM_SYNTAX);
	}

	void OnServHelp(User *u)
	{
		notice_lang(Config->s_OperServ, u, OPER_HELP_CMD_RANDOMNEWS);
	}
};

class OSNews : public Module
{
	CommandOSLogonNews commandoslogonnews;
	CommandOSOperNews commandosopernews;
	CommandOSRandomNews commandosrandomnews;

 public:
	OSNews(const Anope::string &modname, const Anope::string &creator) : Module(modname, creator)
	{
		this->SetAuthor("Anope");
		this->SetType(CORE);

		this->AddCommand(OperServ, &commandoslogonnews);
		this->AddCommand(OperServ, &commandosopernews);
		this->AddCommand(OperServ, &commandosrandomnews);

		Implementation i[] = { I_OnUserModeSet, I_OnUserConnect, I_OnDatabaseRead, I_OnDatabaseWrite };
		ModuleManager::Attach(i, this, 4);
	}

	~OSNews()
	{
		for (std::vector<NewsItem *>::iterator it = News.begin(), it_end = News.end(); it != it_end; ++it)
			delete *it;
		News.clear();
	}

	void OnUserModeSet(User *u, UserModeName Name)
	{
		if (Name == UMODE_OPER)
			DisplayNews(u, NEWS_OPER);
	}

	void OnUserConnect(User *u)
	{
		DisplayNews(u, NEWS_LOGON);
		DisplayNews(u, NEWS_RANDOM);
	}

	EventReturn OnDatabaseRead(const std::vector<Anope::string> &params)
	{
		if (params[0].equals_ci("OS") && params.size() >= 7 && params[1].equals_ci("NEWS"))
		{
			NewsItem *n = new NewsItem();
			n->num = params[2].is_pos_number_only() ? convertTo<unsigned>(params[2]) : 0;
			n->time = params[3].is_number_only() ? convertTo<time_t>(params[3]) : 0;
			n->who = params[4];
			if (params[5].equals_ci("LOGON"))
				n->type = NEWS_LOGON;
			else if (params[5].equals_ci("RANDOM"))
				n->type = NEWS_RANDOM;
			else if (params[5].equals_ci("OPER"))
				n->type = NEWS_OPER;
			n->Text = params[6];
			News.push_back(n);

			return EVENT_STOP;
		}

		return EVENT_CONTINUE;
	}

	void OnDatabaseWrite(void (*Write)(const Anope::string &))
	{
		for (std::vector<NewsItem *>::iterator it = News.begin(); it != News.end(); ++it)
		{
			NewsItem *n = *it;
			Anope::string ntype;
			if (n->type == NEWS_LOGON)
				ntype = "LOGON";
			else if (n->type == NEWS_RANDOM)
				ntype = "RANDOM";
			else if (n->type == NEWS_OPER)
				ntype = "OPER";
			Anope::string buf = "OS NEWS " + stringify(n->num) + " " + stringify(n->time) + " " + n->who + " " + ntype + " :" + n->Text;
			Write(buf);
		}
	}
};

MODULE_INIT(OSNews)
