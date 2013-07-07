
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>
#include <syslog.h>

#include <string>
#include <vector>

using std::string;

bool debug = false;

// do a tr "tr1" "tr2" and then a tr -d "badchars"
string fixupstring(string s, const string &tr1, const string &tr2, const string &badchars)
{
	for(int i = 0; i < s.length(); i++)
	{
		int tr = tr1.find( s[i] );
		if ( tr != string::npos ) { s[i] = tr2[tr]; continue; }

		if ( badchars.find( s[i] ) != string::npos ) { s.erase(i, 1); }
	}
	return s;
}

// read folderlist file (1 folder per line, line format=[INBOX.]foldername.foldername.foldername)
bool getfolderlist(const string &filename, std::vector< string > &folders )
{
	folders.clear();

	// "./Maildir/courierimapsubscribed"
	FILE *folderfile = fopen(filename.c_str(), "rt");
	if ( !folderfile ) { return false; }

	char buf[1024];
	char inboxprefix[] = "INBOX.";

	while ( fgets(buf, sizeof(buf), folderfile) != NULL )
	{
		char *p = buf;
		if ( strncmp(p, inboxprefix, strlen(inboxprefix)) == 0 ) p += strlen(inboxprefix);

		int plen = strlen(p);
		if ( plen > 0 && p[plen-1] == '\n' ) p[plen-1] = '\0';

		folders.push_back( p );
	}

	fclose(folderfile);
	return true;
}

void removehidden(std::vector<string> &folders, const std::vector<string> &hiddenfolders)
{
	for(int foldernum = folders.size() - 1; foldernum > 0; foldernum--)
	{
		for(int hid = 0; hid < hiddenfolders.size(); hid++)
		{
			if ( strcasecmp(folders[foldernum].c_str(), hiddenfolders[hid].c_str()) == 0 )
			{
				folders.erase( &folders[foldernum] );
				break;
			}
		}
	}
}

int findfolder_worker(std::vector<string> &folders, const string &foldername, int matchkind, int startfolder=0)
{
	for(int i = startfolder; i < folders.size(); i++)
	{
		switch ( matchkind )
		{
			// exact match
			case 0: if ( strcasecmp(foldername.c_str(), folders[i].c_str()) == 0 ) return i;
					break;

			// prefix match -- no need for this... will always match folder parent
			//case 1: if ( strncasecmp(foldername.c_str(), folders[i].c_str(), foldername.length()) == 0 ) return i;
			//		break;

			// trailing match
			case 2: int startpos = folders[i].length() - foldername.length();
					if ( startpos > 0 &&
						strcasecmp(folders[i].c_str() + startpos, foldername.c_str() ) == 0 ) return i;
					break;
		}
	}
	return -1;
}

void explodefolder(std::vector<string> &foldernames, const string &folder)
{
	foldernames.clear();
	for(int start=0; start < folder.length(); start++)
	{
		int dot = folder.find('.', start);
		if ( dot == string::npos ) dot = folder.length();
		foldernames.push_back( folder.substr(start, dot-start) );
		start = dot;
	}
}

string buildfolderpath(const std::vector<string> &foldernames, int len)
{
	string result;

	for(int i = 0; i < len; i++)
	{
		if ( i != 0 ) result += ".";
		result += foldernames[i];
	}
	return result;
}

int findfolder(std::vector<string> &folders, const string &foldername)
{
	if ( foldername.length() == 0 ) return -1;

	// exact match
	int result;
	if ( (result = findfolder_worker(folders, foldername, 0)) >= 0 ) return result;

	std::vector<string> foldernames;
	explodefolder(foldernames, foldername);

	// for foldername A.B.C, try
	// A.B
	// A
	for( int i = foldernames.size()-1; i > 0; i--)
	{
		string sub = buildfolderpath(foldernames, i);
		if ( (result = findfolder_worker(folders, sub, 0)) >= 0 ) return result;
	}

	// for foldername A.B.C, try
	// *.A.B.C
	// *.A.B
	// *.A
	// fail if there is a duplicate
	for( int i = foldernames.size(); i > 0; i--)
	{
		string sub = ".";
		sub += buildfolderpath(foldernames, i);
		if ( (result = findfolder_worker(folders, sub, 2)) >= 0 ) 
		{
			int dupf = findfolder_worker(folders, sub, 2, result+1);
			if ( debug && dupf != -1 ) syslog(LOG_INFO, "duplicate folder *%s found, giving up", sub.c_str() );
			return dupf == -1 ? result : -1;
		}
	}

	return -1;
}

string getdestdir(const string &maildir)
{
	string destdir = maildir;
	if ( destdir.length() < 2 ) { printf("Maildir path too short ( <2 ): %s\n", maildir.c_str()); exit(111); }
	if ( destdir[destdir.length()-1] != '/' ) destdir += "/";

	char *extcstr = getenv("EXT");
	if ( !extcstr ) return destdir;

	// xlat '_', '.' to spaces, '-' to '.', and strip lots of suspucious(sp?) chars
	string foldername = fixupstring(extcstr, "_-.", " . ", "`~!@#$%^&*()=[]{}\\|;:'\"<>,?/");

	std::vector<string> folders, hiddenfolders;
	if ( !getfolderlist(destdir + string("courierimapsubscribed"), folders) ) return destdir;
	if ( !getfolderlist(destdir + string("hiddenfolders"), hiddenfolders) )
	{
		hiddenfolders.push_back("Trash");
		hiddenfolders.push_back("Sent");
		hiddenfolders.push_back("Drafts");
	}
	removehidden(folders, hiddenfolders);
	int foldernum = findfolder(folders, foldername);
	if ( foldernum < 0 ) return destdir;

	string folderpath = destdir;
	folderpath += string(".");
	folderpath += folders[foldernum];	// could possibly get bad chars here from the courierimapsubscribed file
	folderpath += string("/");

	return folderpath;
}

int main(int argc, char **argv)
{
	if ( argc < 2 ) 
	{
		printf("Missing dest maildir argument\n");
		return 111;
	}

	debug = (argc > 2) && (strcasecmp(argv[2], "-d") == 0);
	if ( debug ) 
	{
		openlog("maildirfolder", LOG_PID, LOG_MAIL);
		syslog(LOG_INFO, "%s %s, EXT=%s", argv[0], argv[1], getenv("EXT"));
	}

	string destdir = getdestdir(argv[1]);	// will always have a trailing '/'
	string tmpdir = destdir; tmpdir += "tmp";
	string newdir = destdir; newdir += "new";

	if ( debug ) syslog(LOG_INFO, "destdir: %s", destdir.c_str());

	struct stat statbuf;
	if ( stat(tmpdir.c_str(), &statbuf) != 0 || !S_ISDIR(statbuf.st_mode) )
	{
		printf("Bad or missing tmp maildir dir: %s\n", tmpdir.c_str());
		return 111;
	}
	if ( stat(newdir.c_str(), &statbuf) != 0 || !S_ISDIR(statbuf.st_mode) )
	{
		printf("Bad or missing new maildir dir: %s\n", newdir.c_str());
		return 111;
	}
	
	execlp("safecat", "safecat", tmpdir.c_str(), newdir.c_str(), NULL);
	printf("Bad or missing safecat\n");
	return 111;
}

