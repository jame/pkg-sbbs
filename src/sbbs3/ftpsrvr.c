/* ftpsrvr.c */

/* Synchronet FTP server */

/* $Id: ftpsrvr.c,v 1.313 2006/12/29 08:42:56 rswindell Exp $ */

/****************************************************************************
 * @format.tab-size 4		(Plain Text/Source Code File Header)			*
 * @format.use-tabs true	(see http://www.synchro.net/ptsc_hdr.html)		*
 *																			*
 * Copyright 2006 Rob Swindell - http://www.synchro.net/copyright.html		*
 *																			*
 * This program is free software; you can redistribute it and/or			*
 * modify it under the terms of the GNU General Public License				*
 * as published by the Free Software Foundation; either version 2			*
 * of the License, or (at your option) any later version.					*
 * See the GNU General Public License for more details: gpl.txt or			*
 * http://www.fsf.org/copyleft/gpl.html										*
 *																			*
 * Anonymous FTP access to the most recent released source is available at	*
 * ftp://vert.synchro.net, ftp://cvs.synchro.net and ftp://ftp.synchro.net	*
 *																			*
 * Anonymous CVS access to the development source and modification history	*
 * is available at cvs.synchro.net:/cvsroot/sbbs, example:					*
 * cvs -d :pserver:anonymous@cvs.synchro.net:/cvsroot/sbbs login			*
 *     (just hit return, no password is necessary)							*
 * cvs -d :pserver:anonymous@cvs.synchro.net:/cvsroot/sbbs checkout src		*
 *																			*
 * For Synchronet coding style and modification guidelines, see				*
 * http://www.synchro.net/source.html										*
 *																			*
 * You are encouraged to submit any modifications (preferably in Unix diff	*
 * format) via e-mail to mods@synchro.net									*
 *																			*
 * Note: If this box doesn't appear square, then you need to fix your tabs.	*
 ****************************************************************************/

/* ANSI C Library headers */
#include <stdio.h>
#include <stdlib.h>			/* ltoa in GNU C lib */
#include <stdarg.h>			/* va_list, varargs */
#include <string.h>			/* strrchr */
#include <fcntl.h>			/* O_WRONLY, O_RDONLY, etc. */
#include <errno.h>			/* EACCES */
#include <ctype.h>			/* toupper */
#include <sys/stat.h>		/* S_IWRITE */

/* Synchronet-specific headers */
#undef SBBS	/* this shouldn't be defined unless building sbbs.dll/libsbbs.so */
#include "sbbs.h"
#include "text.h"			/* TOTAL_TEXT */
#include "ftpsrvr.h"
#include "telnet.h"

/* Constants */

#define FTP_SERVER				"Synchronet FTP Server"

#define STATUS_WFC				"Listening"
#define ANONYMOUS				"anonymous"

#define BBS_VIRTUAL_PATH		"bbs:/""/"	/* this is actually bbs:<slash><slash> */
#define LOCAL_FSYS_DIR			"local:"
#define BBS_FSYS_DIR			"bbs:"
#define BBS_HIDDEN_ALIAS		"hidden"

#define TIMEOUT_THREAD_WAIT		60		/* Seconds */

#define TIMEOUT_SOCKET_LISTEN	30		/* Seconds */

#define XFER_REPORT_INTERVAL	60		/* Seconds */

#define INDEX_FNAME_LEN			15

#define	NAME_LEN				15		/* User name length for listings */

static ftp_startup_t*	startup=NULL;
static scfg_t	scfg;
static SOCKET	server_socket=INVALID_SOCKET;
static DWORD	active_clients=0;
static DWORD	sockets=0;
static DWORD	thread_count=0;
static time_t	uptime=0;
static DWORD	served=0;
static BOOL		terminate_server=FALSE;
static char		revision[16];
static char 	*text[TOTAL_TEXT];
static str_list_t recycle_semfiles;
static str_list_t shutdown_semfiles;

#ifdef _DEBUG
	static BYTE 	socket_debug[0x10000]={0};

	#define	SOCKET_DEBUG_CTRL		(1<<0)	/* 0x01 */
	#define SOCKET_DEBUG_SEND		(1<<1)	/* 0x02 */
	#define SOCKET_DEBUG_READLINE	(1<<2)	/* 0x04 */
	#define SOCKET_DEBUG_ACCEPT		(1<<3)	/* 0x08 */
	#define SOCKET_DEBUG_SENDTHREAD	(1<<4)	/* 0x10 */
	#define SOCKET_DEBUG_TERMINATE	(1<<5)	/* 0x20 */
	#define SOCKET_DEBUG_RECV_CHAR	(1<<6)	/* 0x40 */
	#define SOCKET_DEBUG_FILEXFER	(1<<7)	/* 0x80 */
#endif


typedef struct {
	SOCKET			socket;
	SOCKADDR_IN		client_addr;
} ftp_t;


static const char *ftp_mon[]={"Jan","Feb","Mar","Apr","May","Jun"
            ,"Jul","Aug","Sep","Oct","Nov","Dec"};

BOOL direxist(char *dir)
{
	if(access(dir,0)==0)
		return(TRUE);
	else
		return(FALSE);
}

BOOL dir_op(scfg_t* cfg, user_t* user, uint dirnum)
{
	return(user->level>=SYSOP_LEVEL || user->exempt&FLAG('R')
		|| (cfg->dir[dirnum]->op_ar[0] && chk_ar(cfg,cfg->dir[dirnum]->op_ar,user)));
}

static int lprintf(int level, char *fmt, ...)
{
	int		result;
	va_list argptr;
	char sbuf[1024];

    if(startup==NULL || startup->lputs==NULL)
        return(0);

    va_start(argptr,fmt);
    vsnprintf(sbuf,sizeof(sbuf),fmt,argptr);
	sbuf[sizeof(sbuf)-1]=0;
    va_end(argptr);
    result=startup->lputs(startup->cbdata,level,sbuf);

	return(result);
}

#ifdef _WINSOCKAPI_

static WSADATA WSAData;
#define SOCKLIB_DESC WSAData.szDescription

static BOOL WSAInitialized=FALSE;

static BOOL winsock_startup(void)
{
	int		status;             /* Status Code */

    if((status = WSAStartup(MAKEWORD(1,1), &WSAData))==0) {
		lprintf(LOG_INFO,"%s %s",WSAData.szDescription, WSAData.szSystemStatus);
		WSAInitialized=TRUE;
		return (TRUE);
	}

    lprintf(LOG_ERR,"!WinSock startup ERROR %d", status);
	return (FALSE);
}

#else /* No WINSOCK */

#define winsock_startup()	(TRUE)
#define SOCKLIB_DESC		NULL

#endif

static void status(char* str)
{
	if(startup!=NULL && startup->status!=NULL)
	    startup->status(startup->cbdata,str);
}

static void update_clients(void)
{
	if(startup!=NULL && startup->clients!=NULL)
		startup->clients(startup->cbdata,active_clients);
}

static void client_on(SOCKET sock, client_t* client, BOOL update)
{
	if(startup!=NULL && startup->client_on!=NULL)
		startup->client_on(startup->cbdata,TRUE,sock,client,update);
}

static void client_off(SOCKET sock)
{
	if(startup!=NULL && startup->client_on!=NULL)
		startup->client_on(startup->cbdata,FALSE,sock,NULL,FALSE);
}

static void thread_up(BOOL setuid)
{
	thread_count++;
	if(startup!=NULL && startup->thread_up!=NULL)
		startup->thread_up(startup->cbdata,TRUE, setuid);
}

static void thread_down(void)
{
	if(thread_count>0)
		thread_count--;
	if(startup!=NULL && startup->thread_up!=NULL)
		startup->thread_up(startup->cbdata,FALSE, FALSE);
}

static SOCKET ftp_open_socket(int type)
{
	SOCKET	sock;
	char	error[256];

	sock=socket(AF_INET, type, IPPROTO_IP);
	if(sock!=INVALID_SOCKET && startup!=NULL && startup->socket_open!=NULL) 
		startup->socket_open(startup->cbdata,TRUE);
	if(sock!=INVALID_SOCKET) {
		if(set_socket_options(&scfg, sock, "FTP", error, sizeof(error)))
			lprintf(LOG_ERR,"%04d !ERROR %s",sock, error);
		sockets++;
#ifdef _DEBUG
		lprintf(LOG_DEBUG,"%04d Socket opened (%u sockets in use)",sock,sockets);
#endif
	}
	return(sock);
}

#ifdef __BORLANDC__
#pragma argsused
#endif
static int ftp_close_socket(SOCKET* sock, int line)
{
	int		result;

	if((*sock)==INVALID_SOCKET) {
		lprintf(LOG_WARNING,"0000 !INVALID_SOCKET in close_socket from line %u",line);
		return(-1);
	}

	shutdown(*sock,SHUT_RDWR);	/* required on Unix */

	result=closesocket(*sock);
	if(startup!=NULL && startup->socket_open!=NULL) 
		startup->socket_open(startup->cbdata,FALSE);

	sockets--;

	if(result!=0) {
		if(ERROR_VALUE!=ENOTSOCK)
			lprintf(LOG_WARNING,"%04d !ERROR %d closing socket from line %u",*sock,ERROR_VALUE,line);
	} else if(sock==&server_socket || *sock==server_socket)
		lprintf(LOG_DEBUG,"%04d Server socket closed (%u sockets in use) from line %u",*sock,sockets,line);
#ifdef _DEBUG
	else 
		lprintf(LOG_DEBUG,"%04d Socket closed (%u sockets in use) from line %u",*sock,sockets,line);
#endif
	*sock=INVALID_SOCKET;

	return(result);
}

static int sockprintf(SOCKET sock, char *fmt, ...)
{
	int		len;
	int		maxlen;
	int		result;
	va_list argptr;
	char	sbuf[1024];
	fd_set	socket_set;
	struct timeval tv;

    va_start(argptr,fmt);
    len=vsnprintf(sbuf,maxlen=sizeof(sbuf)-2,fmt,argptr);
    va_end(argptr);

	if(len<0 || len>maxlen) /* format error or output truncated */
		len=maxlen;
	if(startup!=NULL && startup->options&FTP_OPT_DEBUG_TX)
		lprintf(LOG_DEBUG,"%04d TX: %.*s", sock, len, sbuf);
	memcpy(sbuf+len,"\r\n",2);
	len+=2;

	if(sock==INVALID_SOCKET) {
		lprintf(LOG_WARNING,"!INVALID SOCKET in call to sockprintf");
		return(0);
	}

	/* Check socket for writability (using select) */
	tv.tv_sec=300;
	tv.tv_usec=0;

	FD_ZERO(&socket_set);
	FD_SET(sock,&socket_set);

	if((result=select(sock+1,NULL,&socket_set,NULL,&tv))<1) {
		if(result==0)
			lprintf(LOG_WARNING,"%04d !TIMEOUT selecting socket for send"
				,sock);
		else
			lprintf(LOG_WARNING,"%04d !ERROR %d selecting socket for send"
				,sock, ERROR_VALUE);
		return(0);
	}
	while((result=sendsocket(sock,sbuf,len))!=len) {
		if(result==SOCKET_ERROR) {
			if(ERROR_VALUE==EWOULDBLOCK) {
				YIELD();
				continue;
			}
			if(ERROR_VALUE==ECONNRESET) 
				lprintf(LOG_WARNING,"%04d Connection reset by peer on send",sock);
			else if(ERROR_VALUE==ECONNABORTED)
				lprintf(LOG_WARNING,"%04d Connection aborted by peer on send",sock);
			else
				lprintf(LOG_WARNING,"%04d !ERROR %d sending",sock,ERROR_VALUE);
			return(0);
		}
		lprintf(LOG_WARNING,"%04d !ERROR: short send: %u instead of %u",sock,result,len);
	}
	return(len);
}


/* Returns the directory index of a virtual lib/dir path (e.g. main/games/filename) */
int getdir(char* p, user_t* user)
{
	char*	tp;
	char	path[MAX_PATH+1];
	int		dir;
	int		lib;

	SAFECOPY(path,p);
	p=path;

	if(*p=='/') 
		p++;
	else if(!strncmp(p,"./",2))
		p+=2;

	tp=strchr(p,'/');
	if(tp) *tp=0;
	for(lib=0;lib<scfg.total_libs;lib++) {
		if(!chk_ar(&scfg,scfg.lib[lib]->ar,user))
			continue;
		if(!stricmp(scfg.lib[lib]->sname,p))
			break;
	}
	if(lib>=scfg.total_libs) 
		return(-1);

	if(tp!=NULL)
		p=tp+1;

	tp=strchr(p,'/');
	if(tp) *tp=0;
	for(dir=0;dir<scfg.total_dirs;dir++) {
		if(scfg.dir[dir]->lib!=lib)
			continue;
		if(dir!=scfg.sysop_dir && dir!=scfg.upload_dir 
			&& !chk_ar(&scfg,scfg.dir[dir]->ar,user))
			continue;
		if(!stricmp(scfg.dir[dir]->code_suffix,p))
			break;
	}
	if(dir>=scfg.total_dirs) 
		return(-1);

	return(dir);
}

/*********************************/
/* JavaScript Data and Functions */
/*********************************/
#ifdef JAVASCRIPT

js_server_props_t js_server_props;

static JSBool
js_write(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    uintN		i;
    JSString*	str=NULL;
	FILE*	fp;

	if((fp=(FILE*)JS_GetContextPrivate(cx))==NULL)
		return(JS_FALSE);

    for (i = 0; i < argc; i++) {
		str = JS_ValueToString(cx, argv[i]);
		if (!str)
		    return JS_FALSE;
		fprintf(fp,"%s",JS_GetStringBytes(str));
	}

	if(str==NULL)
		*rval = JSVAL_VOID;
	else
		*rval = STRING_TO_JSVAL(str);
	return(JS_TRUE);
}

static JSBool
js_writeln(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
	FILE*	fp;

	if((fp=(FILE*)JS_GetContextPrivate(cx))==NULL)
		return(JS_FALSE);

	js_write(cx,obj,argc,argv,rval);
	fprintf(fp,"\r\n");
	return(JS_TRUE);
}

static JSFunctionSpec js_global_functions[] = {
	{"write",           js_write,           1},		/* write to HTML file */
	{"writeln",         js_writeln,         1},		/* write to HTML file */
	{"print",			js_writeln,         1},		/* alias for writeln */
	{0}
};

static void
js_ErrorReporter(JSContext *cx, const char *message, JSErrorReport *report)
{
	char	line[64];
	char	file[MAX_PATH+1];
	char*	warning;
	FILE*	fp;

	fp=(FILE*)JS_GetContextPrivate(cx);
	
	if(report==NULL) {
		lprintf(LOG_ERR,"!JavaScript: %s", message);
		if(fp!=NULL)
			fprintf(fp,"!JavaScript: %s", message);
		return;
    }

	if(report->filename)
		sprintf(file," %s",report->filename);
	else
		file[0]=0;

	if(report->lineno)
		sprintf(line," line %u",report->lineno);
	else
		line[0]=0;

	if(JSREPORT_IS_WARNING(report->flags)) {
		if(JSREPORT_IS_STRICT(report->flags))
			warning="strict warning";
		else
			warning="warning";
	} else
		warning="";

	lprintf(LOG_ERR,"!JavaScript %s%s%s: %s",warning,file,line,message);
	if(fp!=NULL)
		fprintf(fp,"!JavaScript %s%s%s: %s",warning,file,line,message);
}

static JSContext* 
js_initcx(JSRuntime* runtime, SOCKET sock, JSObject** glob, JSObject** ftp)
{
	JSContext*	js_cx;
	JSObject*	js_glob;
	BOOL		success=FALSE;

	lprintf(LOG_DEBUG,"%04d JavaScript: Initializing context (stack: %lu bytes)"
		,sock,startup->js.cx_stack);

    if((js_cx = JS_NewContext(runtime, startup->js.cx_stack))==NULL)
		return(NULL);

	lprintf(LOG_DEBUG,"%04d JavaScript: Context created",sock);

    JS_SetErrorReporter(js_cx, js_ErrorReporter);

	do {

		lprintf(LOG_DEBUG,"%04d JavaScript: Initializing Global object",sock);
		if((js_glob=js_CreateGlobalObject(js_cx, &scfg, NULL))==NULL) 
			break;

		if (!JS_DefineFunctions(js_cx, js_glob, js_global_functions)) 
			break;

		lprintf(LOG_DEBUG,"%04d JavaScript: Initializing System object",sock);
		if(js_CreateSystemObject(js_cx, js_glob, &scfg, uptime, startup->host_name, SOCKLIB_DESC)==NULL) 
			break;

		if((*ftp=JS_DefineObject(js_cx, js_glob, "ftp", NULL
			,NULL,JSPROP_ENUMERATE|JSPROP_READONLY))==NULL)
			break;

		if(js_CreateServerObject(js_cx,js_glob,&js_server_props)==NULL)
			break;

		if(glob!=NULL)
			*glob=js_glob;

		success=TRUE;

	} while(0);

	if(!success) {
		JS_DestroyContext(js_cx);
		return(NULL);
	}

	return(js_cx);
}

BOOL js_add_file(JSContext* js_cx, JSObject* array, 
				 char* name, char* desc, char* ext_desc,
				 ulong size, ulong credits, 
				 time_t time, time_t uploaded, time_t last_downloaded, 
				 ulong times_downloaded, ulong misc, 
				 char* uploader, char* link)
{
	JSObject*	file;
	JSString*	js_str;
	jsval		val;
	jsuint		index;

	if((file=JS_NewObject(js_cx, NULL, NULL, NULL))==NULL)
		return(FALSE);

	if((js_str=JS_NewStringCopyZ(js_cx, name))==NULL)
		return(FALSE);
	val=STRING_TO_JSVAL(js_str);
	if(!JS_SetProperty(js_cx, file, "name", &val))
		return(FALSE);

	if((js_str=JS_NewStringCopyZ(js_cx, desc))==NULL)
		return(FALSE);
	val=STRING_TO_JSVAL(js_str);
	if(!JS_SetProperty(js_cx, file, "description", &val))
		return(FALSE);

	if((js_str=JS_NewStringCopyZ(js_cx, ext_desc))==NULL)
		return(FALSE);
	val=STRING_TO_JSVAL(js_str);
	if(!JS_SetProperty(js_cx, file, "extended_description", &val))
		return(FALSE);

	val=INT_TO_JSVAL(size);
	if(!JS_SetProperty(js_cx, file, "size", &val))
		return(FALSE);

	val=INT_TO_JSVAL(credits);
	if(!JS_SetProperty(js_cx, file, "credits", &val))
		return(FALSE);

	JS_NewNumberValue(js_cx,time,&val);
	if(!JS_SetProperty(js_cx, file, "time", &val))
		return(FALSE);

	val=INT_TO_JSVAL(uploaded);
	if(!JS_SetProperty(js_cx, file, "uploaded", &val))
		return(FALSE);

	val=INT_TO_JSVAL(last_downloaded);
	if(!JS_SetProperty(js_cx, file, "last_downloaded", &val))
		return(FALSE);

	val=INT_TO_JSVAL(times_downloaded);
	if(!JS_SetProperty(js_cx, file, "times_downloaded", &val))
		return(FALSE);

	if((js_str=JS_NewStringCopyZ(js_cx, uploader))==NULL)
		return(FALSE);
	val=STRING_TO_JSVAL(js_str);
	if(!JS_SetProperty(js_cx, file, "uploader", &val))
		return(FALSE);

	val=INT_TO_JSVAL(misc);
	if(!JS_SetProperty(js_cx, file, "settings", &val))
		return(FALSE);

	if((js_str=JS_NewStringCopyZ(js_cx, link))==NULL)
		return(FALSE);
	val=STRING_TO_JSVAL(js_str);
	if(!JS_SetProperty(js_cx, file, "link", &val))
		return(FALSE);

	if(!JS_GetArrayLength(js_cx, array, &index))
		return(FALSE);

	val=OBJECT_TO_JSVAL(file);
	return(JS_SetElement(js_cx, array, index, &val));
}

BOOL js_generate_index(JSContext* js_cx, JSObject* parent, 
					   SOCKET sock, FILE* fp, int lib, int dir, user_t* user)
{
	char		str[256];
	char		path[MAX_PATH+1];
	char		spath[MAX_PATH+1];	/* script path */
	char		vpath[MAX_PATH+1];	/* virtual path */
	char		aliasfile[MAX_PATH+1];
	char		extdesc[513];
	char*		p;
	char*		tp;
	char*		np;
	char*		dp;
	char		aliasline[512];
	BOOL		alias_dir;
	BOOL		success=FALSE;
	FILE*		alias_fp;
	uint		i;
	file_t		f;
	glob_t		g;
	jsval		val;
	jsval		rval;
	JSObject*	lib_obj=NULL;
	JSObject*	dir_obj=NULL;
	JSObject*	file_array=NULL;
	JSObject*	dir_array=NULL;
	JSScript*	js_script=NULL;
	JSString*	js_str;

	JS_SetContextPrivate(js_cx, fp);

	do {	/* pseudo try/catch */

		if((file_array=JS_NewArrayObject(js_cx, 0, NULL))==NULL) {
			lprintf(LOG_ERR,"%04d !JavaScript FAILED to create file_array",sock);
			break;
		}

		/* file[] */
		val=OBJECT_TO_JSVAL(file_array);
		if(!JS_SetProperty(js_cx, parent, "file_list", &val)) {
			lprintf(LOG_ERR,"%04d !JavaScript FAILED to set file property",sock);
			break;
		}

		if((dir_array=JS_NewArrayObject(js_cx, 0, NULL))==NULL) {
			lprintf(LOG_ERR,"%04d !JavaScript FAILED to create dir_array",sock);
			break;
		}

		/* dir[] */
		val=OBJECT_TO_JSVAL(dir_array);
		if(!JS_SetProperty(js_cx, parent, "dir_list", &val)) {
			lprintf(LOG_ERR,"%04d !JavaScript FAILED to set dir property",sock);
			break;
		}

		if(strcspn(startup->html_index_script,"/\\")==strlen(startup->html_index_script)) {
			sprintf(spath,"%s%s",scfg.mods_dir,startup->html_index_script);
			if(scfg.mods_dir[0]==0 || !fexist(spath))
				sprintf(spath,"%s%s",scfg.exec_dir,startup->html_index_script);
		} else
			sprintf(spath,"%.*s",(int)sizeof(spath)-4,startup->html_index_script);
		/* Add extension if not specified */
		if(!strchr(spath,'.'))
			strcat(spath,".js");

		if(!fexist(spath)) {
			lprintf(LOG_ERR,"%04d !HTML JavaScript (%s) doesn't exist",sock,spath);
			break;
		}

		if((js_str=JS_NewStringCopyZ(js_cx, startup->html_index_file))==NULL)
			break;
		val=STRING_TO_JSVAL(js_str);
		if(!JS_SetProperty(js_cx, parent, "html_index_file", &val)) {
			lprintf(LOG_ERR,"%04d !JavaScript FAILED to set html_index_file property",sock);
			break;
		}

		/* curlib */
		if((lib_obj=JS_NewObject(js_cx, NULL, 0, NULL))==NULL) {
			lprintf(LOG_ERR,"%04d !JavaScript FAILED to create lib_obj",sock);
			break;
		}

		val=OBJECT_TO_JSVAL(lib_obj);
		if(!JS_SetProperty(js_cx, parent, "curlib", &val)) {
			lprintf(LOG_ERR,"%04d !JavaScript FAILED to set curlib property",sock);
			break;
		}

		/* curdir */
		if((dir_obj=JS_NewObject(js_cx, NULL, 0, NULL))==NULL) {
			lprintf(LOG_ERR,"%04d !JavaScript FAILED to create dir_obj",sock);
			break;
		}

		val=OBJECT_TO_JSVAL(dir_obj);
		if(!JS_SetProperty(js_cx, parent, "curdir", &val)) {
			lprintf(LOG_ERR,"%04d !JavaScript FAILED to set curdir property",sock);
			break;
		}

		SAFECOPY(vpath,"/");

		if(lib>=0) { /* root */

			strcat(vpath,scfg.lib[lib]->sname);
			strcat(vpath,"/");

			if((js_str=JS_NewStringCopyZ(js_cx, scfg.lib[lib]->sname))==NULL)
				break;
			val=STRING_TO_JSVAL(js_str);
			if(!JS_SetProperty(js_cx, lib_obj, "name", &val)) {
				lprintf(LOG_ERR,"%04d !JavaScript FAILED to set curlib.name property",sock);
				break;
			}

			if((js_str=JS_NewStringCopyZ(js_cx, scfg.lib[lib]->lname))==NULL)
				break;
			val=STRING_TO_JSVAL(js_str);
			if(!JS_SetProperty(js_cx, lib_obj, "description", &val)) {
				lprintf(LOG_ERR,"%04d !JavaScript FAILED to set curlib.desc property",sock);
				break;
			}
		}

		if(dir>=0) { /* 1st level */
			strcat(vpath,scfg.dir[dir]->code_suffix);
			strcat(vpath,"/");

			if((js_str=JS_NewStringCopyZ(js_cx, scfg.dir[dir]->code))==NULL)
				break;
			val=STRING_TO_JSVAL(js_str);
			if(!JS_SetProperty(js_cx, dir_obj, "code", &val)) {
				lprintf(LOG_ERR,"%04d !JavaScript FAILED to set curdir.code property",sock);
				break;
			}

			if((js_str=JS_NewStringCopyZ(js_cx, scfg.dir[dir]->sname))==NULL)
				break;
			val=STRING_TO_JSVAL(js_str);
			if(!JS_SetProperty(js_cx, dir_obj, "name", &val)) {
				lprintf(LOG_ERR,"%04d !JavaScript FAILED to set curdir.name property",sock);
				break;
			}

			if((js_str=JS_NewStringCopyZ(js_cx, scfg.dir[dir]->lname))==NULL)
				break;
			val=STRING_TO_JSVAL(js_str);
			if(!JS_SetProperty(js_cx, dir_obj, "description", &val)) {
				lprintf(LOG_ERR,"%04d !JavaScript FAILED to set curdir.desc property",sock);
				break;
			}

			val=INT_TO_JSVAL(scfg.dir[dir]->misc);
			if(!JS_SetProperty(js_cx, dir_obj, "settings", &val)) {
				lprintf(LOG_ERR,"%04d !JavaScript FAILED to set curdir.misc property",sock);
				break;
			}
		}

		if((js_str=JS_NewStringCopyZ(js_cx, vpath))==NULL)
			break;
		val=STRING_TO_JSVAL(js_str);
		if(!JS_SetProperty(js_cx, parent, "path", &val)) {
			lprintf(LOG_ERR,"%04d !JavaScript FAILED to set curdir property",sock);
			break;
		}

		if(lib<0) {	/* root dir */

			/* File Aliases */
			sprintf(path,"%sftpalias.cfg",scfg.ctrl_dir);
			if((alias_fp=fopen(path,"r"))!=NULL) {

				while(!feof(alias_fp)) {
					if(!fgets(aliasline,sizeof(aliasline),alias_fp))
						break;

					p=aliasline;	/* alias pointer */
					while(*p && *p<=' ') p++;

					if(*p==';')	/* comment */
						continue;

					tp=p;		/* terminator pointer */
					while(*tp && *tp>' ') tp++;
					if(*tp) *tp=0;

					np=tp+1;	/* filename pointer */
					while(*np && *np<=' ') np++;

					tp=np;		/* terminator pointer */
					while(*tp && *tp>' ') tp++;
					if(*tp) *tp=0;

					dp=tp+1;	/* description pointer */
					while(*dp && *dp<=' ') dp++;
					truncsp(dp);

					if(stricmp(dp,BBS_HIDDEN_ALIAS)==0)
						continue;

					alias_dir=FALSE;

					/* Virtual Path? */
					if(!strnicmp(np,BBS_VIRTUAL_PATH,strlen(BBS_VIRTUAL_PATH))) {
						if((dir=getdir(np+strlen(BBS_VIRTUAL_PATH),user))<0)
							continue; /* No access or invalid virtual path */
						tp=strrchr(np,'/');
						if(tp==NULL) 
							continue;
						tp++;
						if(*tp) {
							SAFEPRINTF2(aliasfile,"%s%s",scfg.dir[dir]->path,tp);
							np=aliasfile;
						}
						else 
							alias_dir=TRUE;
					}

					if(!alias_dir && !fexist(np))
						continue;

					if(alias_dir) {
						if(!chk_ar(&scfg,scfg.dir[dir]->ar,user))
							continue;
						SAFEPRINTF2(vpath,"/%s/%s",p,startup->html_index_file);
					} else
						SAFECOPY(vpath,p);
					js_add_file(js_cx
						,alias_dir ? dir_array : file_array
						,p				/* filename */
						,dp				/* description */
						,NULL			/* extdesc */
						,flength(np)	/* size */
						,0				/* credits */
						,fdate(np)		/* time */
						,fdate(np)		/* uploaded */
						,0				/* last downloaded */
						,0				/* times downloaded */
						,0				/* misc */
						,scfg.sys_id	/* uploader */
						,vpath			/* link */
						);
				}

				fclose(alias_fp);
			}

			/* QWK Packet */
			if(startup->options&FTP_OPT_ALLOW_QWK /* && fexist(qwkfile) */) {
				sprintf(str,"%s.qwk",scfg.sys_id);
				SAFEPRINTF(vpath,"/%s",str);
				js_add_file(js_cx
					,file_array 
					,str						/* filename */
					,"QWK Message Packet"		/* description */
					,NULL						/* extdesc */
					,10240						/* size */
					,0							/* credits */
					,time(NULL)					/* time */
					,0							/* uploaded */
					,0							/* last downloaded */
					,0							/* times downloaded */
					,0							/* misc */
					,scfg.sys_id				/* uploader */
					,str						/* link */
					);
			}

			/* Library Folders */
			for(i=0;i<scfg.total_libs;i++) {
				if(!chk_ar(&scfg,scfg.lib[i]->ar,user))
					continue;
				SAFEPRINTF2(vpath,"/%s/%s",scfg.lib[i]->sname,startup->html_index_file);
				js_add_file(js_cx
					,dir_array 
					,scfg.lib[i]->sname			/* filename */
					,scfg.lib[i]->lname			/* description */
					,NULL						/* extdesc */
					,0,0,0,0,0,0,0				/* unused */
					,scfg.sys_id				/* uploader */
					,vpath						/* link */
					);
			}
		} else if(dir<0) {
			/* Directories */
			for(i=0;i<scfg.total_dirs;i++) {
				if(scfg.dir[i]->lib!=lib)
					continue;
				if(/* i!=scfg.sysop_dir && i!=scfg.upload_dir && */
					!chk_ar(&scfg,scfg.dir[i]->ar,user))
					continue;
				SAFEPRINTF3(vpath,"/%s/%s/%s"
					,scfg.lib[scfg.dir[i]->lib]->sname
					,scfg.dir[i]->code_suffix
					,startup->html_index_file);
				js_add_file(js_cx
					,dir_array 
					,scfg.dir[i]->sname			/* filename */
					,scfg.dir[i]->lname			/* description */
					,NULL						/* extdesc */
					,getfiles(&scfg,i)			/* size */
					,0,0,0,0,0					/* unused */
					,scfg.dir[i]->misc			/* misc */
					,scfg.sys_id				/* uploader */
					,vpath						/* link */
					);

			}
		} else if(chk_ar(&scfg,scfg.dir[dir]->ar,user)){
			SAFEPRINTF(path,"%s*",scfg.dir[dir]->path);
			glob(path,0,NULL,&g);
			for(i=0;i<(int)g.gl_pathc;i++) {
				if(isdir(g.gl_pathv[i]))
					continue;
	#ifdef _WIN32
				GetShortPathName(g.gl_pathv[i], str, sizeof(str));
	#else
				SAFECOPY(str,g.gl_pathv[i]);
	#endif
				padfname(getfname(str),f.name);
				f.dir=dir;
				if(getfileixb(&scfg,&f)) {
					f.size=0; /* flength(g.gl_pathv[i]); */
					getfiledat(&scfg,&f);
					if(f.misc&FM_EXTDESC) {
						extdesc[0]=0;
						getextdesc(&scfg, dir, f.datoffset, extdesc);
						/* Remove Ctrl-A Codes and Ex-ASCII code */
						remove_ctrl_a(extdesc,NULL);
					}
					SAFEPRINTF3(vpath,"/%s/%s/%s"
						,scfg.lib[scfg.dir[dir]->lib]->sname
						,scfg.dir[dir]->code_suffix
						,getfname(g.gl_pathv[i]));
					js_add_file(js_cx
						,file_array 
						,getfname(g.gl_pathv[i])	/* filename */
						,f.desc						/* description */
						,f.misc&FM_EXTDESC ? extdesc : NULL
						,f.size						/* size */
						,f.cdt						/* credits */
						,f.date						/* time */
						,f.dateuled					/* uploaded */
						,f.datedled					/* last downloaded */
						,f.timesdled				/* times downloaded */
						,f.misc						/* misc */
						,f.uler						/* uploader */
						,getfname(g.gl_pathv[i])	/* link */
						);
				}
			}
			globfree(&g);
		}


		/* RUN SCRIPT */
		JS_ClearPendingException(js_cx);

		if((js_script=JS_CompileFile(js_cx, parent, spath))==NULL) {
			lprintf(LOG_ERR,"%04d !JavaScript FAILED to compile script (%s)",sock,spath);
			break;
		}

		if((success=JS_ExecuteScript(js_cx, parent, js_script, &rval))!=TRUE) {
			lprintf(LOG_ERR,"%04d !JavaScript FAILED to execute script (%s)",sock,spath);
			break;
		}

	} while(0);


	if(js_script!=NULL) 
		JS_DestroyScript(js_cx, js_script);

	JS_DeleteProperty(js_cx, parent, "path");
	JS_DeleteProperty(js_cx, parent, "sort");
	JS_DeleteProperty(js_cx, parent, "reverse");
	JS_DeleteProperty(js_cx, parent, "file_list");
	JS_DeleteProperty(js_cx, parent, "dir_list");
	JS_DeleteProperty(js_cx, parent, "curlib");
	JS_DeleteProperty(js_cx, parent, "curdir");
	JS_DeleteProperty(js_cx, parent, "html_index_file");

	return(success);
}


#endif	/* ifdef JAVASCRIPT */


time_t gettimeleft(scfg_t* cfg, user_t* user, time_t starttime)
{
	time_t	now;
    long    tleft;
	time_t	timeleft;

	now=time(NULL);

	if(user->exempt&FLAG('T')) {   /* Time online exemption */
		timeleft=cfg->level_timepercall[user->level]*60;
		if(timeleft<10)             /* never get below 10 for exempt users */
			timeleft=10; }
	else {
		tleft=(((long)cfg->level_timeperday[user->level]-user->ttoday)
			+user->textra)*60L;
		if(tleft<0) tleft=0;
		if(tleft>cfg->level_timepercall[user->level]*60)
			tleft=cfg->level_timepercall[user->level]*60;
		tleft+=user->min*60L;
		tleft-=now-starttime;
		if(tleft>0x7fffL)
			timeleft=0x7fff;
		else
			timeleft=tleft; }

	return(timeleft);
}

static time_t checktime(void)
{
	struct tm tm;

    memset(&tm,0,sizeof(tm));
    tm.tm_year=94;
    tm.tm_mday=1;
    return(mktime(&tm)-0x2D24BD00L);
}

BOOL upload_stats(ulong bytes)
{
	char	str[MAX_PATH+1];
	int		file;
	ulong	val;

	sprintf(str,"%sdsts.dab",scfg.ctrl_dir);
	if((file=nopen(str,O_RDWR))==-1) 
		return(FALSE);

	lseek(file,20L,SEEK_SET);   /* Skip timestamp, logons and logons today */
	read(file,&val,4);        /* Uploads today         */
	val++;
	lseek(file,-4L,SEEK_CUR);
	write(file,&val,4);
	read(file,&val,4);        /* Upload bytes today    */
	val+=bytes;
	lseek(file,-4L,SEEK_CUR);
	write(file,&val,4);
	close(file);
	return(TRUE);
}

BOOL download_stats(ulong bytes)
{
	char	str[MAX_PATH+1];
	int		file;
	ulong	val;

	sprintf(str,"%sdsts.dab",scfg.ctrl_dir);
	if((file=nopen(str,O_RDWR))==-1) 
		return(FALSE);

	lseek(file,28L,SEEK_SET);   /* Skip timestamp, logons and logons today */
	read(file,&val,4);        /* Downloads today         */
	val++;
	lseek(file,-4L,SEEK_CUR);
	write(file,&val,4);
	read(file,&val,4);        /* Download bytes today    */
	val+=bytes;
	lseek(file,-4L,SEEK_CUR);
	write(file,&val,4);
	close(file);
	return(TRUE);
}

void recverror(SOCKET socket, int rd, int line)
{
	if(rd==0) 
		lprintf(LOG_NOTICE,"%04d Socket closed by peer on receive (line %u)"
			,socket, line);
	else if(rd==SOCKET_ERROR) {
		if(ERROR_VALUE==ECONNRESET) 
			lprintf(LOG_NOTICE,"%04d Connection reset by peer on receive (line %u)"
				,socket, line);
		else if(ERROR_VALUE==ECONNABORTED) 
			lprintf(LOG_NOTICE,"%04d Connection aborted by peer on receive (line %u)"
				,socket, line);
		else
			lprintf(LOG_NOTICE,"%04d !ERROR %d receiving on socket (line %u)"
				,socket, ERROR_VALUE, line);
	} else
		lprintf(LOG_WARNING,"%04d !ERROR: recv on socket returned unexpected value: %d (line %u)"
			,socket, rd, line);
}

int sockreadline(SOCKET socket, char* buf, int len, time_t* lastactive)
{
	char	ch;
	int		i,rd=0;
	fd_set	socket_set;
	struct timeval	tv;

	buf[0]=0;

	if(socket==INVALID_SOCKET) {
		lprintf(LOG_WARNING,"INVALID SOCKET in call to sockreadline");
		return(0);
	}
	
	while(rd<len-1) {

		tv.tv_sec=startup->max_inactivity;
		tv.tv_usec=0;

		FD_ZERO(&socket_set);
		FD_SET(socket,&socket_set);

		i=select(socket+1,&socket_set,NULL,NULL,&tv);

		if(server_socket==INVALID_SOCKET || terminate_server) {
			sockprintf(socket,"421 Server downed, aborting.");
			lprintf(LOG_WARNING,"%04d Server downed, aborting",socket);
			return(0);
		}
		if(i<1) {
			if(i==0) {
				if((time(NULL)-(*lastactive))>startup->max_inactivity) {
					lprintf(LOG_WARNING,"%04d Disconnecting due to to inactivity",socket);
					sockprintf(socket,"421 Disconnecting due to inactivity (%u seconds)."
						,startup->max_inactivity);
					return(0);
				}
				continue;
			}
			recverror(socket,i,__LINE__);
			return(i);
		}
#ifdef SOCKET_DEBUG_RECV_CHAR
		socket_debug[socket]|=SOCKET_DEBUG_RECV_CHAR;
#endif
		i=recv(socket, &ch, 1, 0);
#ifdef SOCKET_DEBUG_RECV_CHAR
		socket_debug[socket]&=~SOCKET_DEBUG_RECV_CHAR;
#endif
		if(i<1) {
			recverror(socket,i,__LINE__);
			return(i);
		}
		if(ch=='\n' /* && rd>=1 */) { /* Mar-9-2003: terminate on sole LF */
			break;
		}	
		buf[rd++]=ch;
	}
	if(rd>0 && buf[rd-1]=='\r')
		buf[rd-1]=0;
	else
		buf[rd]=0;
	
	return(rd);
}

#if 0	/* now exported from in xtrn.cpp */
/*****************************************************************************/
/* Returns command line generated from instr with %c replacments             */
/*****************************************************************************/
static char* cmdstr(user_t* user, char *instr, char *fpath, char *fspec, char *cmd)
{
	char	str[256];
    int		i,j,len;
#ifdef _WIN32
	char sfpath[MAX_PATH+1];
#endif

    len=strlen(instr);
    for(i=j=0;i<len;i++) {
        if(instr[i]=='%') {
            i++;
            cmd[j]=0;
            switch(toupper(instr[i])) {
                case 'A':   /* User alias */
                    strcat(cmd,user->alias);
                    break;
                case 'B':   /* Baud (DTE) Rate */
                case 'C':   /* Connect Description */
                case 'D':   /* Connect (DCE) Rate */
                case 'E':   /* Estimated Rate */
                case 'H':   /* Port Handle or Hardware Flow Control */
                case 'P':   /* COM Port */
                case 'R':   /* Rows */
                case 'T':   /* Time left in seconds */
                case '&':   /* Address of msr */
                case 'Y':	/* COMSPEC */
					/* UNSUPPORTED */
					break;
                case 'F':   /* File path */
                    strcat(cmd,fpath);
                    break;
                case 'G':   /* Temp directory */
                    strcat(cmd,scfg.temp_dir);
                    break;
                case 'I':   /* UART IRQ Line */
                    strcat(cmd,ultoa(scfg.com_irq,str,10));
                    break;
                case 'J':
                    strcat(cmd,scfg.data_dir);
                    break;
                case 'K':
                    strcat(cmd,scfg.ctrl_dir);
                    break;
                case 'L':   /* Lines per message */
                    strcat(cmd,ultoa(scfg.level_linespermsg[user->level],str,10));
                    break;
                case 'M':   /* Minutes (credits) for user */
                    strcat(cmd,ultoa(user->min,str,10));
                    break;
                case 'N':   /* Node Directory (same as SBBSNODE environment var) */
                    strcat(cmd,scfg.node_dir);
                    break;
                case 'O':   /* SysOp */
                    strcat(cmd,scfg.sys_op);
                    break;
                case 'Q':   /* QWK ID */
                    strcat(cmd,scfg.sys_id);
                    break;
                case 'S':   /* File Spec */
                    strcat(cmd,fspec);
                    break;
                case 'U':   /* UART I/O Address (in hex) */
                    strcat(cmd,ultoa(scfg.com_base,str,16));
                    break;
                case 'V':   /* Synchronet Version */
                    sprintf(str,"%s%c",VERSION,REVISION);
                    break;
                case 'W':   /* Time-slice API type (mswtype) */
                    break;
                case 'X':
                    strcat(cmd,scfg.shell[user->shell]->code);
                    break;
                case 'Z':
                    strcat(cmd,scfg.text_dir);
                    break;
				case '~':	/* DOS-compatible (8.3) filename */
#ifdef _WIN32
					SAFECOPY(sfpath,fpath);
					GetShortPathName(fpath,sfpath,sizeof(sfpath));
					strcat(cmd,sfpath);
#else
                    strcat(cmd,fpath);
#endif			
					break;
                case '!':   /* EXEC Directory */
                    strcat(cmd,scfg.exec_dir);
                    break;
                case '@':   /* EXEC Directory for DOS/OS2/Win32, blank for Unix */
#ifndef __unix__
                    strcat(cmd,scfg.exec_dir);
#endif
                    break;
                case '#':   /* Node number (same as SBBSNNUM environment var) */
                    sprintf(str,"%u",scfg.node_num);
                    strcat(cmd,str);
                    break;
                case '*':
                    sprintf(str,"%03u",scfg.node_num);
                    strcat(cmd,str);
                    break;
                case '$':   /* Credits */
                    strcat(cmd,ultoa(user->cdt+user->freecdt,str,10));
                    break;
                case '%':   /* %% for percent sign */
                    strcat(cmd,"%");
                    break;
				case '?':	/* Platform */
#ifdef __OS2__
					SAFECOPY(str,"OS2");
#else
					SAFECOPY(str,PLATFORM_DESC);
#endif
					strlwr(str);
					strcat(cmd,str);
					break;
                default:    /* unknown specification */
                    if(isdigit(instr[i])) {
                        sprintf(str,"%0*d",instr[i]&0xf,user->number);
                        strcat(cmd,str); }
                    break; }
            j=strlen(cmd); }
        else
            cmd[j++]=instr[i]; }
    cmd[j]=0;

    return(cmd);
}
#endif

void DLLCALL ftp_terminate(void)
{
   	lprintf(LOG_DEBUG,"%04d FTP Server terminate",server_socket);
	terminate_server=TRUE;
}


typedef struct {
	SOCKET	ctrl_sock;
	SOCKET*	data_sock;
	BOOL*	inprogress;
	BOOL*	aborted;
	BOOL	delfile;
	BOOL	tmpfile;
	BOOL	credits;
	BOOL	append;
	long	filepos;
	char	filename[MAX_PATH+1];
	time_t*	lastactive;
	user_t*	user;
	int		dir;
	char*	desc;
} xfer_t;

static void send_thread(void* arg)
{
	char		buf[8192];
	char		fname[MAX_PATH+1];
	char		str[128];
	char		tmp[128];
	char		username[128];
	int			i;
	int			rd;
	int			wr;
	long		mod;
	ulong		l;
	ulong		total=0;
	ulong		last_total=0;
	ulong		dur;
	ulong		cps;
	ulong		length;
	BOOL		error=FALSE;
	FILE*		fp;
	file_t		f;
	xfer_t		xfer;
	time_t		now;
	time_t		start;
	time_t		last_report;
	user_t		uploader;
	SOCKADDR_IN	addr;
	socklen_t	addr_len;
	fd_set		socket_set;
	struct timeval tv;

	xfer=*(xfer_t*)arg;
	free(arg);

	thread_up(TRUE /* setuid */);

	length=flength(xfer.filename);

	if((fp=fnopen(NULL,xfer.filename,O_RDONLY|O_BINARY))==NULL	/* non-shareable open failed */
		&& (fp=fopen(xfer.filename,"rb"))==NULL) {				/* shareable open failed */
		lprintf(LOG_ERR,"%04d !DATA ERROR %d opening %s",xfer.ctrl_sock,errno,xfer.filename);
		sockprintf(xfer.ctrl_sock,"450 ERROR %d opening %s.",errno,xfer.filename);
		if(xfer.tmpfile && !(startup->options&FTP_OPT_KEEP_TEMP_FILES))
			remove(xfer.filename);
		ftp_close_socket(xfer.data_sock,__LINE__);
		*xfer.inprogress=FALSE;
		thread_down();
		return;
	}

#ifdef SOCKET_DEBUG_SENDTHREAD
			socket_debug[xfer.ctrl_sock]|=SOCKET_DEBUG_SENDTHREAD;
#endif

	*xfer.aborted=FALSE;
	if(startup->options&FTP_OPT_DEBUG_DATA || xfer.filepos)
		lprintf(LOG_DEBUG,"%04d DATA socket %d sending %s from offset %lu"
			,xfer.ctrl_sock,*xfer.data_sock,xfer.filename,xfer.filepos);

	fseek(fp,xfer.filepos,SEEK_SET);
	last_report=start=time(NULL);
	while((xfer.filepos+total)<length) {

		now=time(NULL);

		/* Periodic progress report */
		if(total && now>=last_report+XFER_REPORT_INTERVAL) {
			if(xfer.filepos)
				sprintf(str," from offset %lu",xfer.filepos);
			else
				str[0]=0;
			lprintf(LOG_INFO,"%04d Sent %lu bytes (%lu total) of %s (%lu cps)%s"
				,xfer.ctrl_sock,total,length,xfer.filename
				,(total-last_total)/(now-last_report)
				,str);
			last_total=total;
			last_report=now;
		}

		if(*xfer.aborted==TRUE) {
			lprintf(LOG_WARNING,"%04d !DATA Transfer aborted",xfer.ctrl_sock);
			sockprintf(xfer.ctrl_sock,"426 Transfer aborted.");
			error=TRUE;
			break;
		}
		if(server_socket==INVALID_SOCKET || terminate_server) {
			lprintf(LOG_WARNING,"%04d !DATA Transfer locally aborted",xfer.ctrl_sock);
			sockprintf(xfer.ctrl_sock,"426 Transfer locally aborted.");
			error=TRUE;
			break;
		}

		/* Check socket for writability (using select) */
		tv.tv_sec=1;
		tv.tv_usec=0;

		FD_ZERO(&socket_set);
		FD_SET(*xfer.data_sock,&socket_set);

		i=select((*xfer.data_sock)+1,NULL,&socket_set,NULL,&tv);
		if(i==SOCKET_ERROR) {
			lprintf(LOG_WARNING,"%04d !DATA ERROR %d selecting socket %d for send"
				,xfer.ctrl_sock, ERROR_VALUE, *xfer.data_sock);
			sockprintf(xfer.ctrl_sock,"426 Transfer error.");
			error=TRUE;
			break;
		}
		if(i<1)
			continue;

		fseek(fp,xfer.filepos+total,SEEK_SET);
		rd=fread(buf,sizeof(char),sizeof(buf),fp);
		if(rd<1) /* EOF or READ error */
			break;

#ifdef SOCKET_DEBUG_SEND
		socket_debug[xfer.ctrl_sock]|=SOCKET_DEBUG_SEND;
#endif
		wr=sendsocket(*xfer.data_sock,buf,rd);
#ifdef SOCKET_DEBUG_SEND
		socket_debug[xfer.ctrl_sock]&=~SOCKET_DEBUG_SEND;
#endif
		if(wr<1) {
			if(wr==SOCKET_ERROR) {
				if(ERROR_VALUE==EWOULDBLOCK) {
					/*lprintf(LOG_WARNING,"%04d DATA send would block, retrying",xfer.ctrl_sock);*/
					YIELD();
					continue;
				}
				else if(ERROR_VALUE==ECONNRESET) 
					lprintf(LOG_WARNING,"%04d DATA Connection reset by peer, sending on socket %d"
						,xfer.ctrl_sock,*xfer.data_sock);
				else if(ERROR_VALUE==ECONNABORTED) 
					lprintf(LOG_WARNING,"%04d DATA Connection aborted by peer, sending on socket %d"
						,xfer.ctrl_sock,*xfer.data_sock);
				else
					lprintf(LOG_WARNING,"%04d !DATA ERROR %d sending on data socket %d"
						,xfer.ctrl_sock,ERROR_VALUE,*xfer.data_sock);
				/* Send NAK */
				sockprintf(xfer.ctrl_sock,"426 Error %d sending on DATA channel"
					,ERROR_VALUE);
				error=TRUE;
				break;
			}
			if(wr==0) {
				lprintf(LOG_WARNING,"%04d !DATA socket %d disconnected",xfer.ctrl_sock, *xfer.data_sock);
				sockprintf(xfer.ctrl_sock,"426 DATA channel disconnected");
				error=TRUE;
				break;
			}
			lprintf(LOG_ERR,"%04d !DATA SEND ERROR %d (%d) on socket %d"
				,xfer.ctrl_sock, wr, ERROR_VALUE, *xfer.data_sock);
			sockprintf(xfer.ctrl_sock,"451 DATA send error");
			error=TRUE;
			break;
		}
		total+=wr;
		*xfer.lastactive=time(NULL);
		YIELD();
	}

	if((i=ferror(fp))!=0) 
		lprintf(LOG_ERR,"%04d !FILE ERROR %d (%d)",xfer.ctrl_sock,i,errno);

	ftp_close_socket(xfer.data_sock,__LINE__);	/* Signal end of file */
	if(startup->options&FTP_OPT_DEBUG_DATA)
		lprintf(LOG_DEBUG,"%04d DATA socket closed",xfer.ctrl_sock);
	
	if(!error) {
		dur=time(NULL)-start;
		cps=dur ? total/dur : total*2;
		lprintf(LOG_INFO,"%04d Transfer successful: %lu bytes sent in %lu seconds (%lu cps)"
			,xfer.ctrl_sock
			,total,dur,cps);
		sockprintf(xfer.ctrl_sock,"226 Download complete (%lu cps).",cps);

		if(xfer.dir>=0) {
			memset(&f,0,sizeof(f));
#ifdef _WIN32
			GetShortPathName(xfer.filename,fname,sizeof(fname));
#else
			SAFECOPY(fname,xfer.filename);
#endif
			padfname(getfname(fname),f.name);
			f.dir=xfer.dir;
			f.size=total;
			if(getfileixb(&scfg,&f)==TRUE && getfiledat(&scfg,&f)==TRUE) {
				f.timesdled++;
				putfiledat(&scfg,&f);
				f.datedled=time(NULL);
				putfileixb(&scfg,&f);

				lprintf(LOG_INFO,"%04d %s downloaded: %s (%lu times total)"
					,xfer.ctrl_sock
					,xfer.user->alias
					,xfer.filename
					,f.timesdled);
				/**************************/
				/* Update Uploader's Info */
				/**************************/
				uploader.number=matchuser(&scfg,f.uler,TRUE /*sysop_alias*/);
				if(uploader.number
					&& uploader.number!=xfer.user->number 
					&& getuserdat(&scfg,&uploader)==0
					&& uploader.firston<f.dateuled) {
					l=f.cdt;
					if(!(scfg.dir[f.dir]->misc&DIR_CDTDL))	/* Don't give credits on d/l */
						l=0;
					if(scfg.dir[f.dir]->misc&DIR_CDTMIN && cps) { /* Give min instead of cdt */
						mod=((ulong)(l*(scfg.dir[f.dir]->dn_pct/100.0))/cps)/60;
						adjustuserrec(&scfg,uploader.number,U_MIN,10,mod);
						sprintf(tmp,"%lu minute",mod);
					} else {
						mod=(ulong)(l*(scfg.dir[f.dir]->dn_pct/100.0));
						adjustuserrec(&scfg,uploader.number,U_CDT,10,mod);
						ultoac(mod,tmp);
					}
					if(!(scfg.dir[f.dir]->misc&DIR_QUIET)) {
						addr_len = sizeof(addr);
						if(uploader.level>=SYSOP_LEVEL
							&& getpeername(xfer.ctrl_sock,(struct sockaddr *)&addr,&addr_len)==0)
							SAFEPRINTF2(username,"%s [%s]",xfer.user->alias,inet_ntoa(addr.sin_addr));
						else
							SAFECOPY(username,xfer.user->alias);
						/* Inform uploader of downloaded file */
						safe_snprintf(str,sizeof(str),text[DownloadUserMsg]
							,getfname(xfer.filename)
							,xfer.filepos ? "partially FTP-" : "FTP-"
							,username,tmp); 
						putsmsg(&scfg,uploader.number,str); 
					}
				}
			}
			if(!xfer.tmpfile && !xfer.delfile && !(scfg.dir[f.dir]->misc&DIR_NOSTAT))
				download_stats(total);
		}	

		if(xfer.credits) {
			user_downloaded(&scfg, xfer.user, 1, total);
			if(xfer.dir>=0 && !is_download_free(&scfg,xfer.dir,xfer.user))
				subtract_cdt(&scfg, xfer.user, xfer.credits);
		}
	}

	fclose(fp);
	if(server_socket!=INVALID_SOCKET && !terminate_server)
		*xfer.inprogress=FALSE;
	if(xfer.tmpfile) {
		if(!(startup->options&FTP_OPT_KEEP_TEMP_FILES))
			remove(xfer.filename);
	} 
	else if(xfer.delfile && !error)
		remove(xfer.filename);

#if defined(SOCKET_DEBUG_SENDTHREAD)
			socket_debug[xfer.ctrl_sock]&=~SOCKET_DEBUG_SENDTHREAD;
#endif

	thread_down();
}

static void receive_thread(void* arg)
{
	char*		p;
	char		str[128];
	char		buf[8192];
	char		ext[F_EXBSIZE+1];
	char		desc[F_EXBSIZE+1];
	char		cmd[MAX_PATH*2];
	char		tmp[MAX_PATH+1];
	char		fname[MAX_PATH+1];
	int			i;
	int			rd;
	int			file;
	ulong		total=0;
	ulong		last_total=0;
	ulong		dur;
	ulong		cps;
	BOOL		error=FALSE;
	BOOL		filedat;
	FILE*		fp;
	file_t		f;
	xfer_t		xfer;
	time_t		now;
	time_t		start;
	time_t		last_report;
	fd_set		socket_set;
	struct timeval tv;

	xfer=*(xfer_t*)arg;
	free(arg);

	thread_up(TRUE /* setuid */);

	if((fp=fopen(xfer.filename,xfer.append ? "ab" : "wb"))==NULL) {
		lprintf(LOG_ERR,"%04d !DATA ERROR %d opening %s",xfer.ctrl_sock,errno,xfer.filename);
		sockprintf(xfer.ctrl_sock,"450 ERROR %d opening %s.",errno,xfer.filename);
		ftp_close_socket(xfer.data_sock,__LINE__);
		*xfer.inprogress=FALSE;
		thread_down();
		return;
	}

	if(xfer.append)
		xfer.filepos=filelength(fileno(fp));

	*xfer.aborted=FALSE;
	if(xfer.filepos || startup->options&FTP_OPT_DEBUG_DATA)
		lprintf(LOG_DEBUG,"%04d DATA socket %d receiving %s from offset %lu"
			,xfer.ctrl_sock,*xfer.data_sock,xfer.filename,xfer.filepos);

	fseek(fp,xfer.filepos,SEEK_SET);
	last_report=start=time(NULL);
	while(1) {

		now=time(NULL);

		/* Periodic progress report */
		if(total && now>=last_report+XFER_REPORT_INTERVAL) {
			if(xfer.filepos)
				sprintf(str," from offset %lu",xfer.filepos);
			else
				str[0]=0;
			lprintf(LOG_INFO,"%04d Received %lu bytes of %s (%lu cps)%s"
				,xfer.ctrl_sock,total,xfer.filename
				,(total-last_total)/(now-last_report)
				,str);
			last_total=total;
			last_report=now;
		}
		if(*xfer.aborted==TRUE) {
			lprintf(LOG_WARNING,"%04d !DATA Transfer aborted",xfer.ctrl_sock);
			/* Send NAK */
			sockprintf(xfer.ctrl_sock,"426 Transfer aborted.");
			error=TRUE;
			break;
		}
		if(server_socket==INVALID_SOCKET || terminate_server) {
			lprintf(LOG_WARNING,"%04d !DATA Transfer locally aborted",xfer.ctrl_sock);
			/* Send NAK */
			sockprintf(xfer.ctrl_sock,"426 Transfer locally aborted.");
			error=TRUE;
			break;
		}

		/* Check socket for readability (using select) */
		tv.tv_sec=1;
		tv.tv_usec=0;

		FD_ZERO(&socket_set);
		FD_SET(*xfer.data_sock,&socket_set);

		i=select((*xfer.data_sock)+1,&socket_set,NULL,NULL,&tv);
		if(i==SOCKET_ERROR) {
			lprintf(LOG_WARNING,"%04d !DATA ERROR %d selecting socket %d for receive"
				,xfer.ctrl_sock, ERROR_VALUE, *xfer.data_sock);
			sockprintf(xfer.ctrl_sock,"426 Transfer error.");
			error=TRUE;
			break;
		}
		if(i<1)
			continue;

#if defined(SOCKET_DEBUG_RECV_BUF)
		socket_debug[xfer.ctrl_sock]|=SOCKET_DEBUG_RECV_BUF;
#endif
		rd=recv(*xfer.data_sock,buf,sizeof(buf),0);
#if defined(SOCKET_DEBUG_RECV_BUF)
		socket_debug[xfer.ctrl_sock]&=~SOCKET_DEBUG_RECV_BUF;
#endif
		if(rd<1) {
			if(rd==0) { /* Socket closed */
				if(startup->options&FTP_OPT_DEBUG_DATA)
					lprintf(LOG_DEBUG,"%04d DATA socket %d closed by client"
						,xfer.ctrl_sock,*xfer.data_sock);
				break;
			}
			if(rd==SOCKET_ERROR) {
				if(ERROR_VALUE==EWOULDBLOCK) {
					/*lprintf(LOG_WARNING,"%04d DATA recv would block, retrying",xfer.ctrl_sock);*/
					YIELD();
					continue;
				}
				else if(ERROR_VALUE==ECONNRESET) 
					lprintf(LOG_WARNING,"%04d DATA Connection reset by peer, receiving on socket %d"
						,xfer.ctrl_sock,*xfer.data_sock);
				else if(ERROR_VALUE==ECONNABORTED) 
					lprintf(LOG_WARNING,"%04d DATA Connection aborted by peer, receiving on socket %d"
						,xfer.ctrl_sock,*xfer.data_sock);
				else
					lprintf(LOG_WARNING,"%04d !DATA ERROR %d receiving on data socket %d"
						,xfer.ctrl_sock,ERROR_VALUE,*xfer.data_sock);
				/* Send NAK */
				sockprintf(xfer.ctrl_sock,"426 Error %d receiving on DATA channel"
					,ERROR_VALUE);
				error=TRUE;
				break;
			}
			lprintf(LOG_ERR,"%04d !DATA ERROR recv returned %d on socket %d"
				,xfer.ctrl_sock,rd,*xfer.data_sock);
			/* Send NAK */
			sockprintf(xfer.ctrl_sock,"451 Unexpected socket error: %d",rd);
			error=TRUE;
			break;
		}
		fwrite(buf,1,rd,fp);
		total+=rd;
		*xfer.lastactive=time(NULL);
		YIELD();
	}

	if(server_socket!=INVALID_SOCKET && !terminate_server)
		*xfer.inprogress=FALSE;
	fclose(fp);

	ftp_close_socket(xfer.data_sock,__LINE__);
	if(error && startup->options&FTP_OPT_DEBUG_DATA)
		lprintf(LOG_DEBUG,"%04d DATA socket %d closed",xfer.ctrl_sock,*xfer.data_sock);
	
	if(!error) {
		dur=time(NULL)-start;
		cps=dur ? total/dur : total*2;
		lprintf(LOG_INFO,"%04d Transfer successful: %lu bytes received in %lu seconds (%lu cps)"
			,xfer.ctrl_sock
			,total,dur,cps);

		if(xfer.dir>=0) {
			memset(&f,0,sizeof(f));
#ifdef _WIN32
			GetShortPathName(xfer.filename,fname,sizeof(fname));
#else
			SAFECOPY(fname,xfer.filename);
#endif
			padfname(getfname(fname),f.name);
			f.dir=xfer.dir;
			filedat=getfileixb(&scfg,&f);
			if(scfg.dir[f.dir]->misc&DIR_AONLY)  /* Forced anonymous */
				f.misc|=FM_ANON;
			f.cdt=flength(xfer.filename);
			f.dateuled=time(NULL);

			/* Desciption specified with DESC command? */
			if(xfer.desc!=NULL && *xfer.desc!=0)	
				SAFECOPY(f.desc,xfer.desc);

			/* Necessary for DIR and LIB ARS keyword support in subsequent chk_ar()'s */
			SAFECOPY(xfer.user->curdir, scfg.dir[f.dir]->code);

			/* FILE_ID.DIZ support */
			p=strrchr(f.name,'.');
			if(p!=NULL && scfg.dir[f.dir]->misc&DIR_DIZ) {
				for(i=0;i<scfg.total_fextrs;i++)
					if(!stricmp(scfg.fextr[i]->ext,p+1) 
						&& chk_ar(&scfg,scfg.fextr[i]->ar,xfer.user))
						break;
				if(i<scfg.total_fextrs) {
					sprintf(tmp,"%sFILE_ID.DIZ",scfg.temp_dir);
					if(fexistcase(tmp))
						remove(tmp);
					system(cmdstr(&scfg,xfer.user,scfg.fextr[i]->cmd,fname,"FILE_ID.DIZ",cmd));
					if(!fexistcase(tmp)) {
						sprintf(tmp,"%sDESC.SDI",scfg.temp_dir);
						if(fexistcase(tmp))
							remove(tmp);
						system(cmdstr(&scfg,xfer.user,scfg.fextr[i]->cmd,fname,"DESC.SDI",cmd)); 
						fexistcase(tmp);	/* fixes filename case */
					}
					if((file=nopen(tmp,O_RDONLY))!=-1) {
						memset(ext,0,sizeof(ext));
						read(file,ext,sizeof(ext)-1);
						for(i=sizeof(ext)-1;i;i--)	/* trim trailing spaces */
							if(ext[i-1]>' ')
								break;
						ext[i]=0;
						if(!f.desc[0]) {			/* use for normal description */
							SAFECOPY(desc,ext);
							strip_exascii(desc);	/* strip extended ASCII chars */
							prep_file_desc(desc);	/* strip control chars and dupe chars */
							for(i=0;desc[i];i++)	/* find approprate first char */
								if(isalnum(desc[i]))
									break;
							SAFECOPY(f.desc,desc+i); 
						}
						close(file);
						remove(tmp);
						f.misc|=FM_EXTDESC; 
					} 
				} 
			} /* FILE_ID.DIZ support */

			if(f.desc[0]==0) 	/* no description given, use (long) filename */
				SAFECOPY(f.desc,getfname(xfer.filename));

			SAFECOPY(f.uler,xfer.user->alias);	/* exception here, Aug-27-2002 */
			if(filedat) {
				if(!putfiledat(&scfg,&f))
					lprintf(LOG_ERR,"%04d !ERROR updating file (%s) in database",xfer.ctrl_sock,f.name);
				/* need to update the index here */
			} else {
				if(!addfiledat(&scfg,&f))
					lprintf(LOG_ERR,"%04d !ERROR adding file (%s) to database",xfer.ctrl_sock,f.name);
			}

			if(f.misc&FM_EXTDESC)
				putextdesc(&scfg,f.dir,f.datoffset,ext);

			if(scfg.dir[f.dir]->upload_sem[0])
				ftouch(scfg.dir[f.dir]->upload_sem);
			/**************************/
			/* Update Uploader's Info */
			/**************************/
			user_uploaded(&scfg, xfer.user, (!xfer.append && xfer.filepos==0) ? 1:0, total);
			if(scfg.dir[f.dir]->up_pct && scfg.dir[f.dir]->misc&DIR_CDTUL) { /* credit for upload */
				if(scfg.dir[f.dir]->misc&DIR_CDTMIN && cps)    /* Give min instead of cdt */
					xfer.user->min=adjustuserrec(&scfg,xfer.user->number,U_MIN,10
						,((ulong)(total*(scfg.dir[f.dir]->up_pct/100.0))/cps)/60);
				else
					xfer.user->cdt=adjustuserrec(&scfg,xfer.user->number,U_CDT,10
						,(ulong)(f.cdt*(scfg.dir[f.dir]->up_pct/100.0))); 
			}
			if(!(scfg.dir[f.dir]->misc&DIR_NOSTAT))
				upload_stats(total);
		}
		/* Send ACK */
		sockprintf(xfer.ctrl_sock,"226 Upload complete (%lu cps).",cps);
	}

	thread_down();
}



static void filexfer(SOCKADDR_IN* addr, SOCKET ctrl_sock, SOCKET pasv_sock, SOCKET* data_sock
					,char* filename, long filepos, BOOL* inprogress, BOOL* aborted
					,BOOL delfile, BOOL tmpfile
					,time_t* lastactive
					,user_t* user
					,int dir
					,BOOL receiving
					,BOOL credits
					,BOOL append
					,char* desc)
{
	int			result;
	ulong		l;
	socklen_t	addr_len;
	SOCKADDR_IN	server_addr;
	BOOL		reuseaddr;
	xfer_t*		xfer;
	struct timeval	tv;
	fd_set			socket_set;

	if((*inprogress)==TRUE) {
		lprintf(LOG_WARNING,"%04d !TRANSFER already in progress",ctrl_sock);
		sockprintf(ctrl_sock,"425 Transfer already in progress.");
		return;
	}
	*inprogress=TRUE;

	if(*data_sock!=INVALID_SOCKET)
		ftp_close_socket(data_sock,__LINE__);

	if(pasv_sock==INVALID_SOCKET) {	/* !PASV */

		if((*data_sock=socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) == INVALID_SOCKET) {
			lprintf(LOG_ERR,"%04d !DATA ERROR %d opening socket", ctrl_sock, ERROR_VALUE);
			sockprintf(ctrl_sock,"425 Error %d opening socket",ERROR_VALUE);
			if(tmpfile)
				remove(filename);
			*inprogress=FALSE;
			return;
		}
		if(startup->socket_open!=NULL)
			startup->socket_open(startup->cbdata,TRUE);
		sockets++;
		if(startup->options&FTP_OPT_DEBUG_DATA)
			lprintf(LOG_DEBUG,"%04d DATA socket %d opened",ctrl_sock,*data_sock);

		/* Use port-1 for all data connections */
		reuseaddr=TRUE;
		setsockopt(*data_sock,SOL_SOCKET,SO_REUSEADDR,(char*)&reuseaddr,sizeof(reuseaddr));

		memset(&server_addr, 0, sizeof(server_addr));

		server_addr.sin_addr.s_addr = htonl(startup->interface_addr);
		server_addr.sin_family = AF_INET;
		server_addr.sin_port   = htons((WORD)(startup->port-1));	/* 20? */

		result=bind(*data_sock, (struct sockaddr *) &server_addr,sizeof(server_addr));
		if(result!=0) {
			server_addr.sin_port = 0;	/* any user port */
			result=bind(*data_sock, (struct sockaddr *) &server_addr,sizeof(server_addr));
		}
		if(result!=0) {
			lprintf(LOG_ERR,"%04d !DATA ERROR %d (%d) binding socket %d"
				,ctrl_sock, result, ERROR_VALUE, *data_sock);
			sockprintf(ctrl_sock,"425 Error %d binding socket",ERROR_VALUE);
			if(tmpfile)
				remove(filename);
			*inprogress=FALSE;
			ftp_close_socket(data_sock,__LINE__);
			return;
		}

		result=connect(*data_sock, (struct sockaddr *)addr,sizeof(struct sockaddr));
		if(result!=0) {
			lprintf(LOG_WARNING,"%04d !DATA ERROR %d (%d) connecting to client %s port %u on socket %d"
					,ctrl_sock,result,ERROR_VALUE
					,inet_ntoa(addr->sin_addr),ntohs(addr->sin_port),*data_sock);
			sockprintf(ctrl_sock,"425 Error %d connecting to socket",ERROR_VALUE);
			if(tmpfile)
				remove(filename);
			*inprogress=FALSE;
			ftp_close_socket(data_sock,__LINE__);
			return;
		}
		if(startup->options&FTP_OPT_DEBUG_DATA)
			lprintf(LOG_DEBUG,"%04d DATA socket %d connected to %s port %u"
				,ctrl_sock,*data_sock,inet_ntoa(addr->sin_addr),ntohs(addr->sin_port));

	} else {	/* PASV */

		if(startup->options&FTP_OPT_DEBUG_DATA) {
			addr_len=sizeof(SOCKADDR_IN);
			if((result=getsockname(pasv_sock, (struct sockaddr *)addr,&addr_len))!=0)
				lprintf(LOG_ERR,"%04d !ERROR %d (%d) getting address/port of passive socket (%u)"
					,ctrl_sock,result,ERROR_VALUE,pasv_sock);
			else
				lprintf(LOG_DEBUG,"%04d PASV DATA socket %d listening on %s port %u"
					,ctrl_sock,pasv_sock,inet_ntoa(addr->sin_addr),ntohs(addr->sin_port));
		}

		/* Setup for select() */
		tv.tv_sec=TIMEOUT_SOCKET_LISTEN;
		tv.tv_usec=0;

		FD_ZERO(&socket_set);
		FD_SET(pasv_sock,&socket_set);

#if defined(SOCKET_DEBUG_SELECT)
		socket_debug[ctrl_sock]|=SOCKET_DEBUG_SELECT;
#endif
		result=select(pasv_sock+1,&socket_set,NULL,NULL,&tv);
#if defined(SOCKET_DEBUG_SELECT)
		socket_debug[ctrl_sock]&=~SOCKET_DEBUG_SELECT;
#endif
		if(result<1) {
			lprintf(LOG_WARNING,"%04d !PASV select returned %d (error: %d)",ctrl_sock,result,ERROR_VALUE);
			sockprintf(ctrl_sock,"425 Error %d selecting socket for connection",ERROR_VALUE);
			if(tmpfile)
				remove(filename);
			*inprogress=FALSE;
			return;
		}
			
		addr_len=sizeof(SOCKADDR_IN);
#ifdef SOCKET_DEBUG_ACCEPT
		socket_debug[ctrl_sock]|=SOCKET_DEBUG_ACCEPT;
#endif
		*data_sock=accept(pasv_sock,(struct sockaddr*)addr,&addr_len);
#ifdef SOCKET_DEBUG_ACCEPT
		socket_debug[ctrl_sock]&=~SOCKET_DEBUG_ACCEPT;
#endif
		if(*data_sock==INVALID_SOCKET) {
			lprintf(LOG_WARNING,"%04d !PASV DATA ERROR %d accepting connection on socket %d"
				,ctrl_sock,ERROR_VALUE,pasv_sock);
			sockprintf(ctrl_sock,"425 Error %d accepting connection",ERROR_VALUE);
			if(tmpfile)
				remove(filename);
			*inprogress=FALSE;
			return;
		}
		if(startup->socket_open!=NULL)
			startup->socket_open(startup->cbdata,TRUE);
		sockets++;
		if(startup->options&FTP_OPT_DEBUG_DATA)
			lprintf(LOG_DEBUG,"%04d PASV DATA socket %d connected to %s port %u"
				,ctrl_sock,*data_sock,inet_ntoa(addr->sin_addr),ntohs(addr->sin_port));
	}

	do {

		l=1;

		if(ioctlsocket(*data_sock, FIONBIO, &l)!=0) {
			lprintf(LOG_ERR,"%04d !DATA ERROR %d disabling socket blocking"
				,ctrl_sock, ERROR_VALUE);
			sockprintf(ctrl_sock,"425 Error %d disabling socket blocking"
				,ERROR_VALUE);
			break;
		}

		if((xfer=malloc(sizeof(xfer_t)))==NULL) {
			lprintf(LOG_CRIT,"%04d !MALLOC FAILURE LINE %d",ctrl_sock,__LINE__);
			sockprintf(ctrl_sock,"425 MALLOC FAILURE");
			break;
		}
		memset(xfer,0,sizeof(xfer_t));
		xfer->ctrl_sock=ctrl_sock;
		xfer->data_sock=data_sock;
		xfer->inprogress=inprogress;
		xfer->aborted=aborted;
		xfer->delfile=delfile;
		xfer->tmpfile=tmpfile;
		xfer->append=append;
		xfer->filepos=filepos;
		xfer->credits=credits;
		xfer->lastactive=lastactive;
		xfer->user=user;
		xfer->dir=dir;
		xfer->desc=desc;
		SAFECOPY(xfer->filename,filename);
		if(receiving)
			result=_beginthread(receive_thread,0,(void*)xfer);
		else
			result=_beginthread(send_thread,0,(void*)xfer);

		if(result!=-1)
			return;	/* success */

	} while(0);

	/* failure */
	if(tmpfile)
		remove(filename);
	*inprogress=FALSE;
}

/* convert "user name" to "user.name" or "mr. user" to "mr._user" */
char* dotname(char* in, char* out)
{
	char	ch;
	int		i;

	if(strchr(in,'.')==NULL)
		ch='.';
	else
		ch='_';
	for(i=0;in[i];i++)
		if(in[i]<=' ')
			out[i]=ch;
		else
			out[i]=in[i];
	out[i]=0;
	return(out);
}

void parsepath(char** pp, user_t* user, int* curlib, int* curdir)
{
	char*	p;
	char*	tp;
	char	path[MAX_PATH+1];
	int		dir=*curdir;
	int		lib=*curlib;

	SAFECOPY(path,*pp);
	p=path;

	if(*p=='/') {
		p++;
		lib=-1;
	}
	else if(!strncmp(p,"./",2))
		p+=2;

	if(!strncmp(p,"..",2)) {
		p+=2;
		if(dir>=0)
			dir=-1;
		else if(lib>=0)
			lib=-1;
		if(*p=='/')
			p++;
	}

	if(*p==0) {
		*curlib=lib;
		*curdir=dir;
		return;
	}

	if(lib<0) { /* root */
		tp=strchr(p,'/');
		if(tp) *tp=0;
		for(lib=0;lib<scfg.total_libs;lib++) {
			if(!chk_ar(&scfg,scfg.lib[lib]->ar,user))
				continue;
			if(!stricmp(scfg.lib[lib]->sname,p))
				break;
		}
		if(lib>=scfg.total_libs) { /* not found */
			*curlib=-1;
			return;
		}
		*curlib=lib;

		if(tp==NULL) {
			*curdir=-1;
			return;
		}

		p=tp+1;
	}

	tp=strchr(p,'/');
	if(tp!=NULL) {
		*tp=0;
		tp++;
	} else 
		tp=p+strlen(p);

	for(dir=0;dir<scfg.total_dirs;dir++) {
		if(scfg.dir[dir]->lib!=lib)
			continue;
		if(dir!=scfg.sysop_dir && dir!=scfg.upload_dir 
			&& !chk_ar(&scfg,scfg.dir[dir]->ar,user))
			continue;
		if(!stricmp(scfg.dir[dir]->code_suffix,p))
			break;
	}

	if(dir>=scfg.total_dirs)  /* not found */
		return;

	*curdir=dir;

	*pp+=tp-path;	/* skip "lib/dir/" */
}

static BOOL ftpalias(char* fullalias, char* filename, user_t* user, int* curdir)
{
	char*	p;
	char*	tp;
	char*	fname="";
	char	line[512];
	char	alias[512];
	char	aliasfile[MAX_PATH+1];
	int		dir=-1;
	FILE*	fp;
	BOOL	result=FALSE;

	sprintf(aliasfile,"%sftpalias.cfg",scfg.ctrl_dir);
	if((fp=fopen(aliasfile,"r"))==NULL) 
		return(FALSE);

	SAFECOPY(alias,fullalias);
	p=strrchr(alias+1,'/');
	if(p) {
		*p=0;
		fname=p+1;
	}

	if(filename==NULL /* directory */ && *fname /* filename specified */) {
		fclose(fp);
		return(FALSE);
	}

	while(!feof(fp)) {
		if(!fgets(line,sizeof(line),fp))
			break;

		p=line;	/* alias */
		while(*p && *p<=' ') p++;
		if(*p==';')	/* comment */
			continue;

		tp=p;		/* terminator */
		while(*tp && *tp>' ') tp++;
		if(*tp) *tp=0;

		if(stricmp(p,alias))	/* Not a match */
			continue;

		p=tp+1;		/* filename */
		while(*p && *p<=' ') p++;

		tp=p;		/* terminator */
		while(*tp && *tp>' ') tp++;
		if(*tp) *tp=0;

		if(!strnicmp(p,BBS_VIRTUAL_PATH,strlen(BBS_VIRTUAL_PATH))) {
			if((dir=getdir(p+strlen(BBS_VIRTUAL_PATH),user))<0)	{
				lprintf(LOG_WARNING,"0000 !Invalid virtual path (%s) for %s",p,user->alias);
				/* invalid or no access */
				continue;
			}
			p=strrchr(p,'/');
			if(p!=NULL) p++;
			if(p!=NULL && filename!=NULL) {
				if(*p)
					sprintf(filename,"%s%s",scfg.dir[dir]->path,p);
				else
					sprintf(filename,"%s%s",scfg.dir[dir]->path,fname);
			}
		} else if(filename!=NULL)
			strcpy(filename,p);

		result=TRUE;	/* success */
		break;
	}
	fclose(fp);
	if(curdir!=NULL)
		*curdir=dir;
	return(result);
}

char* root_dir(char* path)
{
	char*	p;
	static char	root[MAX_PATH+1];

	SAFECOPY(root,path);

	if(!strncmp(root,"\\\\",2)) {	/* network path */
		p=strchr(root+2,'\\');
		if(p) p=strchr(p+1,'\\');
		if(p) *(p+1)=0;				/* truncate at \\computer\sharename\ */
	} 
	else if(!strncmp(root+1,":/",2) || !strncmp(root+1,":\\",2))
		root[3]=0;
	else if(*root=='/' || *root=='\\')
		root[1]=0;

	return(root);
}

char* vpath(int lib, int dir, char* str)
{
	strcpy(str,"/");
	if(lib<0)
		return(str);
	strcat(str,scfg.lib[lib]->sname);
	strcat(str,"/");
	if(dir<0)
		return(str);
	strcat(str,scfg.dir[dir]->code_suffix);
	strcat(str,"/");
	return(str);
}

void ftp_printfile(SOCKET sock, const char* name, unsigned code)
{
	char	path[MAX_PATH+1];
	char	buf[512];
	FILE*	fp;
	unsigned i;

	SAFEPRINTF2(path,"%sftp%s.txt",scfg.text_dir,name);
	if((fp=fopen(path,"rb"))!=NULL) {
		i=0;
		while(!feof(fp)) {
			if(!fgets(buf,sizeof(buf),fp))
				break;
			truncsp(buf);
			if(!i)
				sockprintf(sock,"%u-%s",code,buf);
			else
				sockprintf(sock," %s",buf);
			i++;
		}
		fclose(fp);
	}
}

static BOOL badlogin(SOCKET sock, ulong* login_attempts)
{
	mswait(5000);	/* As recommended by RFC2577 */
	if(++(*login_attempts)>=3) {
		sockprintf(sock,"421 Too many failed login attempts.");
		return(TRUE);
	}
	ftp_printfile(sock,"badlogin",530);
	sockprintf(sock,"530 Invalid login.");
	return(FALSE);
}

static char* ftp_tmpfname(char* str, SOCKET sock)
{
	safe_snprintf(str,MAX_PATH,"%sSBBS_FTP.%u.%u.tx",scfg.temp_dir,getpid(),sock);
	return(str);
}

static void ctrl_thread(void* arg)
{
	char		buf[512];
	char		str[128];
	char*		cmd;
	char*		p;
	char*		np;
	char*		tp;
	char*		dp;
	char*		mode="active";
	char		password[64];
	char		fname[MAX_PATH+1];
	char		qwkfile[MAX_PATH+1];
	char		aliasfile[MAX_PATH+1];
	char		aliasline[512];
	char		desc[501]="";
	char		sys_pass[128];
	char*		host_name;
	char		host_ip[64];
	char		path[MAX_PATH+1];
	char		local_dir[MAX_PATH+1];
	char		ren_from[MAX_PATH+1]="";
	char		html_index_ext[MAX_PATH+1];
	WORD		port;
	ulong		ip_addr;
	socklen_t	addr_len;
	DWORD		h1,h2,h3,h4;
	u_short		p1,p2;	/* For PORT command */
	int			i;
	int			rd;
	int			result;
	int			lib;
	int			dir;
	int			curlib=-1;
	int			curdir=-1;
	int			orglib;
	int			orgdir;
	long		filepos=0L;
	long		timeleft;
	ulong		l;
	ulong		login_attempts=0;
	ulong		avail;	/* disk space */
	ulong		count;
	BOOL		detail;
	BOOL		success;
	BOOL		getdate;
	BOOL		getsize;
	BOOL		delecmd;
	BOOL		delfile;
	BOOL		tmpfile;
	BOOL		credits;
	BOOL		filedat=FALSE;
	BOOL		transfer_inprogress;
	BOOL		transfer_aborted;
	BOOL		sysop=FALSE;
	BOOL		local_fsys=FALSE;
	BOOL		alias_dir;
	BOOL		append;
	BOOL		reuseaddr;
	FILE*		fp;
	FILE*		alias_fp;
	SOCKET		sock;
	SOCKET		tmp_sock;
	SOCKET		pasv_sock=INVALID_SOCKET;
	SOCKET		data_sock=INVALID_SOCKET;
	HOSTENT*	host;
	SOCKADDR_IN	addr;
	SOCKADDR_IN	data_addr;
	SOCKADDR_IN	pasv_addr;
	ftp_t		ftp=*(ftp_t*)arg;
	user_t		user;
	time_t		t;
	time_t		now;
	time_t		logintime=0;
	time_t		lastactive;
	file_t		f;
	glob_t		g;
	node_t		node;
	client_t	client;
	struct tm	tm;
	struct tm 	cur_tm;
#ifdef JAVASCRIPT
	jsval		js_val;
	JSRuntime*	js_runtime=NULL;
	JSContext*	js_cx=NULL;
	JSObject*	js_glob;
	JSObject*	js_ftp;
	JSString*	js_str;
#endif

	thread_up(TRUE /* setuid */);

	lastactive=time(NULL);

	sock=ftp.socket;
	data_addr=ftp.client_addr;
	/* Default data port is ctrl port-1 */
	data_addr.sin_port=ntohs(data_addr.sin_port)-1;
	data_addr.sin_port=htons(data_addr.sin_port);
	
	lprintf(LOG_DEBUG,"%04d CTRL thread started", sock);

	free(arg);	/* unexplicable assertion here on July 26, 2001 */

#ifdef _WIN32
	if(startup->answer_sound[0] && !(startup->options&FTP_OPT_MUTE)) 
		PlaySound(startup->answer_sound, NULL, SND_ASYNC|SND_FILENAME);
#endif

	transfer_inprogress = FALSE;
	transfer_aborted = FALSE;

	l=1;

	if((i=ioctlsocket(sock, FIONBIO, &l))!=0) {
		lprintf(LOG_ERR,"%04d !ERROR %d (%d) disabling socket blocking"
			,sock, i, ERROR_VALUE);
		sockprintf(sock,"425 Error %d disabling socket blocking"
			,ERROR_VALUE);
		ftp_close_socket(&sock,__LINE__);
		thread_down();
		return;
	}

	memset(&user,0,sizeof(user));

	SAFECOPY(host_ip,inet_ntoa(ftp.client_addr.sin_addr));

	lprintf(LOG_DEBUG,"%04d CTRL connection accepted from: %s port %u"
		,sock, host_ip, ntohs(ftp.client_addr.sin_port));

	if(startup->options&FTP_OPT_NO_HOST_LOOKUP)
		host=NULL;
	else
		host=gethostbyaddr ((char *)&ftp.client_addr.sin_addr
			,sizeof(ftp.client_addr.sin_addr),AF_INET);

	if(host!=NULL && host->h_name!=NULL)
		host_name=host->h_name;
	else
		host_name="<no name>";

	if(!(startup->options&FTP_OPT_NO_HOST_LOOKUP)) {
		lprintf(LOG_INFO,"%04d Hostname: %s", sock, host_name);
		for(i=0;host!=NULL && host->h_aliases!=NULL && host->h_aliases[i]!=NULL;i++)
			lprintf(LOG_INFO,"%04d HostAlias: %s", sock, host->h_aliases[i]);
	}

	if(trashcan(&scfg,host_ip,"ip")) {
		lprintf(LOG_NOTICE,"%04d !CLIENT BLOCKED in ip.can: %s", sock, host_ip);
		sockprintf(sock,"550 Access denied.");
		ftp_close_socket(&sock,__LINE__);
		thread_down();
		return;
	}

	if(trashcan(&scfg,host_name,"host")) {
		lprintf(LOG_NOTICE,"%04d !CLIENT BLOCKED in host.can: %s", sock, host_name);
		sockprintf(sock,"550 Access denied.");
		ftp_close_socket(&sock,__LINE__);
		thread_down();
		return;
	}

	/* For PASV mode */
	addr_len=sizeof(pasv_addr);
	if((result=getsockname(sock, (struct sockaddr *)&pasv_addr,&addr_len))!=0) {
		lprintf(LOG_ERR,"%04d !ERROR %d (%d) getting address/port", sock, result, ERROR_VALUE);
		sockprintf(sock,"425 Error %d getting address/port",ERROR_VALUE);
		ftp_close_socket(&sock,__LINE__);
		thread_down();
		return;
	} 

	active_clients++, update_clients();

	/* Initialize client display */
	client.size=sizeof(client);
	client.time=time(NULL);
	SAFECOPY(client.addr,host_ip);
	SAFECOPY(client.host,host_name);
	client.port=ntohs(ftp.client_addr.sin_port);
	client.protocol="FTP";
	client.user="<unknown>";
	client_on(sock,&client,FALSE /* update */);

	sockprintf(sock,"220-%s (%s)",scfg.sys_name, startup->host_name);
	sockprintf(sock," Synchronet FTP Server %s-%s Ready"
		,revision,PLATFORM_DESC);
	sprintf(str,"%sftplogin.txt",scfg.text_dir);
	if((fp=fopen(str,"rb"))!=NULL) {
		while(!feof(fp)) {
			if(!fgets(buf,sizeof(buf),fp))
				break;
			truncsp(buf);
			sockprintf(sock," %s",buf);
		}
		fclose(fp);
	}
	sockprintf(sock,"220 Please enter your user name.");

#ifdef SOCKET_DEBUG_CTRL
	socket_debug[sock]|=SOCKET_DEBUG_CTRL;
#endif
	while(1) {

#ifdef SOCKET_DEBUG_READLINE
		socket_debug[sock]|=SOCKET_DEBUG_READLINE;
#endif
		rd = sockreadline(sock, buf, sizeof(buf), &lastactive);
#ifdef SOCKET_DEBUG_READLINE
		socket_debug[sock]&=~SOCKET_DEBUG_READLINE;
#endif
		if(rd<1) {
			if(transfer_inprogress==TRUE) {
				lprintf(LOG_WARNING,"%04d Aborting transfer due to receive error",sock);
				transfer_aborted=TRUE;
			}
			break;
		}
		truncsp(buf);
		lastactive=time(NULL);
		cmd=buf;
		while(((BYTE)*cmd)==TELNET_IAC) {
			cmd++;
			lprintf(LOG_DEBUG,"%04d RX: Telnet cmd: %s",sock,telnet_cmd_desc(*cmd));
			cmd++;
		}
		while(*cmd && *cmd<' ') {
			lprintf(LOG_DEBUG,"%04d RX: %d (0x%02X)",sock,(BYTE)*cmd,(BYTE)*cmd);
			cmd++;
		}
		if(!(*cmd))
			continue;
		if(startup->options&FTP_OPT_DEBUG_RX)
			lprintf(LOG_DEBUG,"%04d RX: %s", sock, cmd);
		if(!stricmp(cmd, "NOOP")) {
			sockprintf(sock,"200 NOOP command successful.");
			continue;
		}
		if(!stricmp(cmd, "HELP SITE") || !stricmp(cmd, "SITE HELP")) {
			sockprintf(sock,"214-The following SITE commands are recognized (* => unimplemented):");
			sockprintf(sock," HELP    VER     WHO     UPTIME");
			if(user.level>=SYSOP_LEVEL)
				sockprintf(sock,
							" RECYCLE [ALL]");
			if(sysop)
				sockprintf(sock,
							" EXEC <cmd>");
			sockprintf(sock,"214 Direct comments to sysop@%s.",scfg.sys_inetaddr);
			continue;
		}
		if(!strnicmp(cmd, "HELP",4)) {
			sockprintf(sock,"214-The following commands are recognized (* => unimplemented, # => extension):");
			sockprintf(sock," USER    PASS    CWD     XCWD    CDUP    XCUP    PWD     XPWD");
			sockprintf(sock," QUIT    REIN    PORT    PASV    LIST    NLST    NOOP    HELP");
			sockprintf(sock," SIZE    MDTM    RETR    STOR    REST    ALLO    ABOR    SYST");
			sockprintf(sock," TYPE    STRU    MODE    SITE    RNFR*   RNTO*   DELE*   DESC#");
			sockprintf(sock," FEAT#   OPTS#");
			sockprintf(sock,"214 Direct comments to sysop@%s.",scfg.sys_inetaddr);
			continue;
		}
		if(!stricmp(cmd, "FEAT")) {
			sockprintf(sock,"211-The following additional (post-RFC949) features are supported:");
			sockprintf(sock," DESC");
			sockprintf(sock," MDTM");
			sockprintf(sock," SIZE");
			sockprintf(sock," REST STREAM");
			sockprintf(sock,"211 End");
			continue;
		}
		if(!strnicmp(cmd, "OPTS",4)) {
			sockprintf(sock,"501 No options supported.");
			continue;
		}
		if(!stricmp(cmd, "QUIT")) {
			ftp_printfile(sock,"bye",221);
			sockprintf(sock,"221 Goodbye. Closing control connection.");
			break;
		}
		if(!strnicmp(cmd, "USER ",5)) {
			sysop=FALSE;
			user.number=0;
			p=cmd+5;
			while(*p && *p<=' ') p++;
			truncsp(p);
			SAFECOPY(user.alias,p);
			user.number=matchuser(&scfg,user.alias,FALSE /*sysop_alias*/);
			if(!user.number && !stricmp(user.alias,"anonymous"))	
				user.number=matchuser(&scfg,"guest",FALSE);
			if(user.number && getuserdat(&scfg, &user)==0 && user.pass[0]==0) 
				sockprintf(sock,"331 User name okay, give your full e-mail address as password.");
			else
				sockprintf(sock,"331 User name okay, need password.");
			user.number=0;
			continue;
		}
		if(!strnicmp(cmd, "PASS ",5) && user.alias[0]) {
			user.number=0;
			p=cmd+5;
			while(*p && *p<=' ') p++;

			SAFECOPY(password,p);
			user.number=matchuser(&scfg,user.alias,FALSE /*sysop_alias*/);
			if(!user.number) {
				if(scfg.sys_misc&SM_ECHO_PW)
					lprintf(LOG_WARNING,"%04d !UNKNOWN USER: %s, Password: %s",sock,user.alias,p);
				else
					lprintf(LOG_WARNING,"%04d !UNKNOWN USER: %s",sock,user.alias);
				if(badlogin(sock,&login_attempts))
					break;
				continue;
			}
			if((i=getuserdat(&scfg, &user))!=0) {
				lprintf(LOG_ERR,"%04d !ERROR %d getting data for user #%d (%s)"
					,sock,i,user.number,user.alias);
				sockprintf(sock,"530 Database error %d",i);
				user.number=0;
				continue;
			}
			if(user.misc&(DELETED|INACTIVE)) {
				lprintf(LOG_WARNING,"%04d !DELETED or INACTIVE user #%d (%s)"
					,sock,user.number,user.alias);
				user.number=0;
				if(badlogin(sock,&login_attempts))
					break;
				continue;
			}
			if(user.rest&FLAG('T')) {
				lprintf(LOG_WARNING,"%04d !T RESTRICTED user #%d (%s)"
					,sock,user.number,user.alias);
				user.number=0;
				if(badlogin(sock,&login_attempts))
					break;
				continue;
			}
			if(user.ltoday>scfg.level_callsperday[user.level]
				&& !(user.exempt&FLAG('L'))) {
				lprintf(LOG_WARNING,"%04d !MAXIMUM LOGONS (%d) reached for %s"
					,sock,scfg.level_callsperday[user.level],user.alias);
				sockprintf(sock,"530 Maximum logons reached.");
				user.number=0;
				continue;
			}
			if(user.rest&FLAG('L') && user.ltoday>1) {
				lprintf(LOG_WARNING,"%04d !L RESTRICTED user #%d (%s) already on today"
					,sock,user.number,user.alias);
				sockprintf(sock,"530 Maximum logons reached.");
				user.number=0;
				continue;
			}

			SAFEPRINTF2(sys_pass,"%s:%s",user.pass,scfg.sys_pass);
			if(!user.pass[0]) {	/* Guest/Anonymous */
				if(trashcan(&scfg,password,"email")) {
					lprintf(LOG_NOTICE,"%04d !BLOCKED e-mail address: %s",sock,password);
					user.number=0;
					if(badlogin(sock,&login_attempts))
						break;
					continue;
				}
				lprintf(LOG_INFO,"%04d %s: <%s>",sock,user.alias,password);
				putuserrec(&scfg,user.number,U_NETMAIL,LEN_NETMAIL,password);
			}
			else if(user.level>=SYSOP_LEVEL && !stricmp(password,sys_pass)) {
				lprintf(LOG_INFO,"%04d Sysop access granted to %s", sock, user.alias);
				sysop=TRUE;
			}
			else if(stricmp(password,user.pass)) {
				if(scfg.sys_misc&SM_ECHO_PW)
					lprintf(LOG_WARNING,"%04d !FAILED Password attempt for user %s: '%s' expected '%s'"
						,sock, user.alias, password, user.pass);
				else
					lprintf(LOG_WARNING,"%04d !FAILED Password attempt for user %s"
						,sock, user.alias);
				user.number=0;
				if(badlogin(sock,&login_attempts))
					break;
				continue;
			}

			/* Update client display */
			if(user.pass[0])
				client.user=user.alias;
			else {	/* anonymous */
				sprintf(str,"%s <%.32s>",user.alias,password);
				client.user=str;
			}
			client_on(sock,&client,TRUE /* update */);


			lprintf(LOG_INFO,"%04d %s logged in",sock,user.alias);
			logintime=time(NULL);
			timeleft=gettimeleft(&scfg,&user,logintime);
			ftp_printfile(sock,"hello",230);

#ifdef JAVASCRIPT
#ifdef JS_CX_PER_SESSION
			if(js_cx!=NULL) {

				if(js_CreateUserClass(js_cx, js_glob, &scfg)==NULL) 
					lprintf(LOG_ERR,"%04d !JavaScript ERROR creating user class",sock);

				if(js_CreateUserObject(js_cx, js_glob, &scfg, "user", user.number)==NULL) 
					lprintf(LOG_ERR,"%04d !JavaScript ERROR creating user object",sock);

				if(js_CreateClientObject(js_cx, js_glob, "client", &client, sock)==NULL) 
					lprintf(LOG_ERR,"%04d !JavaScript ERROR creating client object",sock);

				if(js_CreateFileAreaObject(js_cx, js_glob, &scfg, &user
					,startup->html_index_file)==NULL) 
					lprintf(LOG_ERR,"%04d !JavaScript ERROR creating file area object",sock);

			}
#endif
#endif

			if(sysop)
				sockprintf(sock,"230-Sysop access granted.");
			sockprintf(sock,"230-%s logged in.",user.alias);
			if(!(user.exempt&FLAG('D')) && (user.cdt+user.freecdt)>0)
				sockprintf(sock,"230-You have %lu download credits."
					,user.cdt+user.freecdt);
			sockprintf(sock,"230 You are allowed %lu minutes of use for this session."
				,timeleft/60);
			sprintf(qwkfile,"%sfile/%04d.qwk",scfg.data_dir,user.number);

			/* Adjust User Total Logons/Logons Today */
			adjustuserrec(&scfg,user.number,U_LOGONS,5,1);
			putuserrec(&scfg,user.number,U_LTODAY,5,ultoa(user.ltoday+1,str,10));
			putuserrec(&scfg,user.number,U_MODEM,LEN_MODEM,"FTP");
			putuserrec(&scfg,user.number,U_COMP,LEN_COMP,host_name);
			putuserrec(&scfg,user.number,U_NOTE,LEN_NOTE,host_ip);
			putuserrec(&scfg,user.number,U_LOGONTIME,0,ultoa(logintime,str,16));
			getuserdat(&scfg, &user);	/* make user current */

			continue;
		}

		if(!user.number) {
			sockprintf(sock,"530 Please login with USER and PASS.");
			continue;
		}

		if(!(user.rest&FLAG('G')))
			getuserdat(&scfg, &user);	/* get current user data */

		if((timeleft=gettimeleft(&scfg,&user,logintime))<1L) {
			sockprintf(sock,"421 Sorry, you've run out of time.");
			lprintf(LOG_WARNING,"%04d Out of time, disconnecting",sock);
			break;
		}

		/********************************/
		/* These commands require login */
		/********************************/

		if(!stricmp(cmd, "REIN")) {
			lprintf(LOG_INFO,"%04d %s reinitialized control session",sock,user.alias);
			user.number=0;
			sysop=FALSE;
			filepos=0;
			sockprintf(sock,"220 Control session re-initialized. Ready for re-login.");
			continue;
		}

		if(!stricmp(cmd, "SITE WHO")) {
			sockprintf(sock,"211-Active Telnet Nodes:");
			for(i=0;i<scfg.sys_nodes && i<scfg.sys_lastnode;i++) {
				if((result=getnodedat(&scfg, i+1, &node, 0))!=0) {
					sockprintf(sock," Error %d getting data for Telnet Node %d",result,i+1);
					continue;
				}
				if(node.status==NODE_INUSE)
					sockprintf(sock," Node %3d: %s",i+1, username(&scfg,node.useron,str));
			}
			sockprintf(sock,"211 End (%d active FTP clients)", active_clients);
			continue;
		}
		if(!stricmp(cmd, "SITE VER")) {
			sockprintf(sock,"211 %s",ftp_ver());
			continue;
		}
		if(!stricmp(cmd, "SITE UPTIME")) {
			sockprintf(sock,"211 %s (%lu served)",sectostr(time(NULL)-uptime,str),served);
			continue;
		}
		if(!stricmp(cmd, "SITE RECYCLE") && user.level>=SYSOP_LEVEL) {
			startup->recycle_now=TRUE;
			sockprintf(sock,"211 server will recycle when not in-use");
			continue;
		}
		if(!stricmp(cmd, "SITE RECYCLE ALL") && user.level>=SYSOP_LEVEL) {
			refresh_cfg(&scfg);
			sockprintf(sock,"211 ALL servers/nodes will recycle when not in-use");
			continue;
		}
		if(!strnicmp(cmd,"SITE EXEC ",10) && sysop) {
			p=cmd+10;
			while(*p && *p<=' ') p++;
#ifdef __unix__
			fp=popen(p,"r");
			if(fp==NULL)
				sockprintf(sock,"500 Error %d opening pipe to: %s",errno,p);
			else {
				while(!feof(fp)) {
					if(fgets(str,sizeof(str),fp)==NULL)
						break;
					sockprintf(sock,"200-%s",str);
				}
				sockprintf(sock,"200 %s returned %d",p,pclose(fp));
			}
#else
			sockprintf(sock,"200 system(%s) returned %d",p,system(p));
#endif
			continue;
		}


#ifdef SOCKET_DEBUG_CTRL
		if(!stricmp(cmd, "SITE DEBUG")) {
			sockprintf(sock,"211-Debug");
			for(i=0;i<sizeof(socket_debug);i++) 
				if(socket_debug[i]!=0)
					sockprintf(sock,"211-socket %d = 0x%X",i,socket_debug[i]);
			sockprintf(sock,"211 End");
			continue;
		}
#endif

		if(!strnicmp(cmd, "PORT ",5)) {

			if(pasv_sock!=INVALID_SOCKET) 
				ftp_close_socket(&pasv_sock,__LINE__);

			p=cmd+5;
			while(*p && *p<=' ') p++;
			sscanf(p,"%ld,%ld,%ld,%ld,%hd,%hd",&h1,&h2,&h3,&h4,&p1,&p2);
			data_addr.sin_addr.s_addr=htonl((h1<<24)|(h2<<16)|(h3<<8)|h4);
			data_addr.sin_port=(u_short)((p1<<8)|p2);
			if(data_addr.sin_port< IPPORT_RESERVED) {	
				lprintf(LOG_WARNING,"%04d !SUSPECTED BOUNCE ATTACK ATTEMPT by %s to %s port %u"
					,sock,user.alias
					,inet_ntoa(data_addr.sin_addr),data_addr.sin_port);
				hacklog(&scfg, "FTP", user.alias, cmd, host_name, &ftp.client_addr);
				sockprintf(sock,"504 Bad port number.");	
#ifdef _WIN32
				if(startup->hack_sound[0] && !(startup->options&FTP_OPT_MUTE)) 
					PlaySound(startup->hack_sound, NULL, SND_ASYNC|SND_FILENAME);
#endif
				continue; /* As recommended by RFC2577 */
			}
			data_addr.sin_port=htons(data_addr.sin_port);
			sockprintf(sock,"200 PORT Command successful.");
			mode="active";
			continue;
		}

		if(!stricmp(cmd, "PASV") 
			|| !stricmp(cmd, "P@SW")) {	/* Kludge required for SMC Barricade V1.2 */

			if(pasv_sock!=INVALID_SOCKET) 
				ftp_close_socket(&pasv_sock,__LINE__);

			if((pasv_sock=ftp_open_socket(SOCK_STREAM))==INVALID_SOCKET) {
				lprintf(LOG_WARNING,"%04d !PASV ERROR %d opening socket", sock,ERROR_VALUE);
				sockprintf(sock,"425 Error %d opening PASV data socket", ERROR_VALUE);
				continue;
			}

			reuseaddr=FALSE;
			if((result=setsockopt(pasv_sock,SOL_SOCKET,SO_REUSEADDR,(char*)&reuseaddr,sizeof(reuseaddr)))!=0) {
				lprintf(LOG_WARNING,"%04d !PASV ERROR %d disabling REUSEADDR socket option"
					,sock,ERROR_VALUE);
				sockprintf(sock,"425 Error %d disabling REUSEADDR socket option", ERROR_VALUE);
				continue;
			}

			if(startup->options&FTP_OPT_DEBUG_DATA)
				lprintf(LOG_DEBUG,"%04d PASV DATA socket %d opened",sock,pasv_sock);

			for(port=startup->pasv_port_low; port<=startup->pasv_port_high; port++) {

				if(startup->options&FTP_OPT_DEBUG_DATA)
					lprintf(LOG_DEBUG,"%04d PASV DATA trying to bind socket to port %u"
						,sock,port);

				pasv_addr.sin_port = htons(port);

				if((result=bind(pasv_sock, (struct sockaddr *) &pasv_addr,sizeof(pasv_addr)))==0)
					break;
			}
			if(result!= 0) {
				lprintf(LOG_ERR,"%04d !PASV ERROR %d (%d) binding socket to port %u"
					,sock, result, ERROR_VALUE, port);
				sockprintf(sock,"425 Error %d binding data socket",ERROR_VALUE);
				ftp_close_socket(&pasv_sock,__LINE__);
				continue;
			}
			if(startup->options&FTP_OPT_DEBUG_DATA)
				lprintf(LOG_DEBUG,"%04d PASV DATA socket %d bound to port %u",sock,pasv_sock,port);

			addr_len=sizeof(addr);
			if((result=getsockname(pasv_sock, (struct sockaddr *)&addr,&addr_len))!=0) {
				lprintf(LOG_ERR,"%04d !PASV ERROR %d (%d) getting address/port"
					,sock, result, ERROR_VALUE);
				sockprintf(sock,"425 Error %d getting address/port",ERROR_VALUE);
				ftp_close_socket(&pasv_sock,__LINE__);
				continue;
			} 

			if((result=listen(pasv_sock, 1))!= 0) {
				lprintf(LOG_ERR,"%04d !PASV ERROR %d (%d) listening on port %u"
					,sock, result, ERROR_VALUE,port);
				sockprintf(sock,"425 Error %d listening on data socket",ERROR_VALUE);
				ftp_close_socket(&pasv_sock,__LINE__);
				continue;
			}

			/* Choose IP address to use in passive response */
			ip_addr=0;
			if(startup->options&FTP_OPT_LOOKUP_PASV_IP
				&& (host=gethostbyname(startup->host_name))!=NULL) 
				ip_addr=ntohl(*((ulong*)host->h_addr_list[0]));
			if(ip_addr==0 && (ip_addr=startup->pasv_ip_addr)==0)
				ip_addr=ntohl(pasv_addr.sin_addr.s_addr);

			if(startup->options&FTP_OPT_DEBUG_DATA)
				lprintf(LOG_INFO,"%04d PASV DATA IP address in response: %u.%u.%u.%u (subject to NAT)"
					,sock
					,(ip_addr>>24)&0xff
					,(ip_addr>>16)&0xff
					,(ip_addr>>8)&0xff
					,ip_addr&0xff
					);
			port=ntohs(addr.sin_port);
			sockprintf(sock,"227 Entering Passive Mode (%u,%u,%u,%u,%hu,%hu)"
				,(ip_addr>>24)&0xff
				,(ip_addr>>16)&0xff
				,(ip_addr>>8)&0xff
				,ip_addr&0xff
				,(port>>8)&0xff
				,port&0xff
				);
			mode="passive";
			continue;
		}

		if(!strnicmp(cmd, "TYPE ",5)) {
			sockprintf(sock,"200 All files sent in BINARY mode.");
			continue;
		}

		if(!strnicmp(cmd, "ALLO",4)) {
			p=cmd+5;
			while(*p && *p<=' ') p++;
			if(*p)
				l=atol(p);	
			else
				l=0;
			if(local_fsys)
				avail=getfreediskspace(local_dir,0);
			else
				avail=getfreediskspace(scfg.data_dir,0);	/* Change to temp_dir? */
			if(l && l>avail)
				sockprintf(sock,"504 Only %lu bytes available.",avail);
			else
				sockprintf(sock,"200 %lu bytes available.",avail);
			continue;
		}

		if(!strnicmp(cmd, "REST",4)) {
			p=cmd+4;
			while(*p && *p<=' ') p++;
			if(*p)
				filepos=atol(p);
			else
				filepos=0;
			sockprintf(sock,"350 Restarting at %lu. Send STORE or RETRIEVE to initiate transfer."
				,filepos);
			continue;
		}

		if(!strnicmp(cmd, "MODE ",5)) {
			p=cmd+5;
			while(*p && *p<=' ') p++;
			if(toupper(*p)!='S')
				sockprintf(sock,"504 Only STREAM mode supported.");
			else
				sockprintf(sock,"200 STREAM mode.");
			continue;
		}

		if(!strnicmp(cmd, "STRU ",5)) {
			p=cmd+5;
			while(*p && *p<=' ') p++;
			if(toupper(*p)!='F')
				sockprintf(sock,"504 Only FILE structure supported.");
			else
				sockprintf(sock,"200 FILE structure.");
			continue;
		}

		if(!stricmp(cmd, "SYST")) {
			sockprintf(sock,"215 UNIX Type: L8");
			continue;
		}

		if(!stricmp(cmd, "ABOR")) {
			if(!transfer_inprogress)
				sockprintf(sock,"226 No tranfer in progress.");
			else {
				lprintf(LOG_WARNING,"%04d %s aborting transfer"
					,sock,user.alias);
				transfer_aborted=TRUE;
				YIELD(); /* give send thread time to abort */
				sockprintf(sock,"226 Transfer aborted.");
			}
			continue;
		}

		if(!strnicmp(cmd,"SMNT ",5) && sysop && !(startup->options&FTP_OPT_NO_LOCAL_FSYS)) {
			p=cmd+5;
			while(*p && *p<=' ') p++;
			if(!stricmp(p,BBS_FSYS_DIR)) 
				local_fsys=FALSE;
			else {
				if(!direxist(p)) {
					sockprintf(sock,"550 Directory does not exist.");
					lprintf(LOG_WARNING,"%04d !%s attempted to mount invalid directory: %s"
						,sock, user.alias, p);
					continue;
				}
				local_fsys=TRUE;
				SAFECOPY(local_dir,p);
			}
			sockprintf(sock,"250 %s file system mounted."
				,local_fsys ? "Local" : "BBS");
			lprintf(LOG_INFO,"%04d %s mounted %s file system"
				,sock, user.alias, local_fsys ? "local" : "BBS");
			continue;
		}

		/****************************/
		/* Local File System Access */
		/****************************/
		if(sysop && local_fsys && !(startup->options&FTP_OPT_NO_LOCAL_FSYS)) {
			if(local_dir[0] 
				&& local_dir[strlen(local_dir)-1]!='\\'
				&& local_dir[strlen(local_dir)-1]!='/')
				strcat(local_dir,"/");

			if(!strnicmp(cmd, "LIST", 4) || !strnicmp(cmd, "NLST", 4)) {	
				if((fp=fopen(ftp_tmpfname(fname,sock),"w+b"))==NULL) {
					lprintf(LOG_ERR,"%04d !ERROR %d opening %s",sock,errno,fname);
					sockprintf(sock, "451 Insufficient system storage");
					continue;
				}
				if(!strnicmp(cmd, "LIST", 4))
					detail=TRUE;
				else
					detail=FALSE;

				p=cmd+4;
				while(*p && *p<=' ') p++;

				if(*p=='-') {	/* -Letc */
					while(*p && *p>' ') p++;
					while(*p && *p<=' ') p++;
				}

				SAFEPRINTF2(path,"%s%s",local_dir, *p ? p : "*");
				lprintf(LOG_INFO,"%04d %s listing: %s in %s mode", sock, user.alias, path, mode);
				sockprintf(sock, "150 Directory of %s%s", local_dir, p);

				now=time(NULL);
				if(localtime_r(&now,&cur_tm)==NULL) 
					memset(&cur_tm,0,sizeof(cur_tm));
			
				glob(path,0,NULL,&g);
				for(i=0;i<(int)g.gl_pathc;i++) {
					if(detail) {
						f.size=flength(g.gl_pathv[i]);
						t=fdate(g.gl_pathv[i]);
						if(localtime_r(&t,&tm)==NULL)
							memset(&tm,0,sizeof(tm));
						fprintf(fp,"%crw-r--r--   1 %-8s local %9ld %s %2d "
							,isdir(g.gl_pathv[i]) ? 'd':'-'
							,scfg.sys_id
							,f.size
							,ftp_mon[tm.tm_mon],tm.tm_mday);
						if(tm.tm_year==cur_tm.tm_year)
							fprintf(fp,"%02d:%02d %s\r\n"
								,tm.tm_hour,tm.tm_min
								,getfname(g.gl_pathv[i]));
						else
							fprintf(fp,"%5d %s\r\n"
								,1900+tm.tm_year
								,getfname(g.gl_pathv[i]));
					} else
						fprintf(fp,"%s\r\n",getfname(g.gl_pathv[i]));
				}
				globfree(&g);
				fclose(fp);
				filexfer(&data_addr,sock,pasv_sock,&data_sock,fname,0L
					,&transfer_inprogress,&transfer_aborted
					,TRUE	/* delfile */
					,TRUE	/* tmpfile */
					,&lastactive,&user,-1,FALSE,FALSE,FALSE,NULL);
				continue;
			} /* Local LIST/NLST */
				
			if(!strnicmp(cmd, "CWD ", 4) || !strnicmp(cmd,"XCWD ",5)) {
			    if(!strnicmp(cmd,"CWD ",4))
					p=cmd+4;
				else
					p=cmd+5;
				while(*p && *p<=' ') p++;
				tp=p;
				if(*tp=='/' || *tp=='\\') /* /local: and /bbs: are valid */
					tp++;
				if(!strnicmp(tp,BBS_FSYS_DIR,strlen(BBS_FSYS_DIR))) {
					local_fsys=FALSE;
					sockprintf(sock,"250 CWD command successful (BBS file system mounted).");
					lprintf(LOG_INFO,"%04d %s mounted BBS file system", sock, user.alias);
					continue;
				}
				if(!strnicmp(tp,LOCAL_FSYS_DIR,strlen(LOCAL_FSYS_DIR))) {
					tp+=strlen(LOCAL_FSYS_DIR);	/* already mounted */
					p=tp;
				}

				if(p[1]==':' || !strncmp(p,"\\\\",2))
					SAFECOPY(path,p);
				else if(*p=='/' || *p=='\\')
					SAFEPRINTF2(path,"%s%s",root_dir(local_dir),p);
				else {
					SAFEPRINTF2(fname,"%s%s",local_dir,p);
					FULLPATH(path,fname,sizeof(path));
				}

				if(!direxist(path)) {
					sockprintf(sock,"550 Directory does not exist (%s).",path);
					lprintf(LOG_WARNING,"%04d !%s attempted to change to an invalid directory: %s"
						,sock, user.alias, path);
				} else {
					SAFECOPY(local_dir,path);
					sockprintf(sock,"250 CWD command successful (%s).", local_dir);
				}
				continue;
			} /* Local CWD */

			if(!stricmp(cmd,"CDUP") || !stricmp(cmd,"XCUP")) {
				SAFEPRINTF(path,"%s..",local_dir);
				if(FULLPATH(local_dir,path,sizeof(local_dir))==NULL)
					sockprintf(sock,"550 Directory does not exist.");
				else
					sockprintf(sock,"200 CDUP command successful.");
				continue;
			}

			if(!stricmp(cmd, "PWD") || !stricmp(cmd,"XPWD")) {
				if(strlen(local_dir)>3)
					local_dir[strlen(local_dir)-1]=0;	/* truncate '/' */

				sockprintf(sock,"257 \"%s\" is current directory."
					,local_dir);
				continue;
			} /* Local PWD */

			if(!strnicmp(cmd, "MKD ", 4) || !strnicmp(cmd,"XMKD",4)) {
				p=cmd+4;
				while(*p && *p<=' ') p++;
				if(*p=='/')	/* absolute */
					SAFEPRINTF2(fname,"%s%s",root_dir(local_dir),p+1);
				else		/* relative */
					SAFEPRINTF2(fname,"%s%s",local_dir,p);

				if((i=MKDIR(fname))==0) {
					sockprintf(sock,"257 \"%s\" directory created",fname);
					lprintf(LOG_NOTICE,"%04d %s created directory: %s",sock,user.alias,fname);
				} else {
					sockprintf(sock,"521 Error %d creating directory: %s",i,fname);
					lprintf(LOG_WARNING,"%04d !%s attempted to create directory: %s (Error %d)"
						,sock,user.alias,fname,i);
				}
				continue;
			}

			if(!strnicmp(cmd, "RMD ", 4) || !strnicmp(cmd,"XRMD",4)) {
				p=cmd+4;
				while(*p && *p<=' ') p++;
				if(*p=='/')	/* absolute */
					SAFEPRINTF2(fname,"%s%s",root_dir(local_dir),p+1);
				else		/* relative */
					SAFEPRINTF2(fname,"%s%s",local_dir,p);

				if((i=rmdir(fname))==0) {
					sockprintf(sock,"250 \"%s\" directory removed",fname);
					lprintf(LOG_NOTICE,"%04d %s removed directory: %s",sock,user.alias,fname);
				} else {
					sockprintf(sock,"450 Error %d removing directory: %s",i,fname);
					lprintf(LOG_WARNING,"%04d !%s attempted to remove directory: %s (Error %d)"
						,sock,user.alias,fname,i);
				}
				continue;
			}

			if(!strnicmp(cmd, "RNFR ",5)) {
				p=cmd+5;
				while(*p && *p<=' ') p++;
				if(*p=='/')	/* absolute */
					SAFEPRINTF2(ren_from,"%s%s",root_dir(local_dir),p+1);
				else		/* relative */
					SAFEPRINTF2(ren_from,"%s%s",local_dir,p);
				if(!fexist(ren_from)) {
					sockprintf(sock,"550 File not found: %s",ren_from);
					lprintf(LOG_WARNING,"%04d !%s attempted to rename %s (not found)"
						,sock,user.alias,ren_from);
				} else
					sockprintf(sock,"350 File exists, ready for destination name");
				continue;
			}

			if(!strnicmp(cmd, "RNTO ",5)) {
				p=cmd+5;
				while(*p && *p<=' ') p++;
				if(*p=='/')	/* absolute */
					SAFEPRINTF2(fname,"%s%s",root_dir(local_dir),p+1);
				else		/* relative */
					SAFEPRINTF2(fname,"%s%s",local_dir,p);

				if((i=rename(ren_from, fname))==0) {
					sockprintf(sock,"250 \"%s\" renamed to \"%s\"",ren_from,fname);
					lprintf(LOG_NOTICE,"%04d %s renamed %s to %s",sock,user.alias,ren_from,fname);
				} else {
					sockprintf(sock,"450 Error %d renaming file: %s",i,ren_from);
					lprintf(LOG_WARNING,"%04d !%s attempted to rename file: %s (Error %d)"
						,sock,user.alias,ren_from,i);
				}
				continue;
			}


			if(!strnicmp(cmd, "RETR ", 5) || !strnicmp(cmd,"SIZE ",5) 
				|| !strnicmp(cmd, "MDTM ",5) || !strnicmp(cmd, "DELE ",5)) {
				p=cmd+5;
				while(*p && *p<=' ') p++;

				if(!strnicmp(p,LOCAL_FSYS_DIR,strlen(LOCAL_FSYS_DIR))) 
					p+=strlen(LOCAL_FSYS_DIR);	/* already mounted */

				if(p[1]==':')		/* drive specified */
					SAFECOPY(fname,p);
				else if(*p=='/')	/* absolute, current drive */
					SAFEPRINTF2(fname,"%s%s",root_dir(local_dir),p+1);
				else		/* relative */
					SAFEPRINTF2(fname,"%s%s",local_dir,p);
				if(!fexist(fname)) {
					lprintf(LOG_WARNING,"%04d !%s file not found: %s",sock,user.alias,fname);
					sockprintf(sock,"550 File not found: %s",fname);
					continue;
				}
				if(!strnicmp(cmd,"SIZE ",5)) {
					sockprintf(sock,"213 %lu",flength(fname));
					continue;
				}
				if(!strnicmp(cmd,"MDTM ",5)) {
					t=fdate(fname);
					if(gmtime_r(&t,&tm)==NULL) /* specifically use GMT/UTC representation */
						memset(&tm,0,sizeof(tm));
					sockprintf(sock,"213 %u%02u%02u%02u%02u%02u"
						,1900+tm.tm_year,tm.tm_mon+1,tm.tm_mday
						,tm.tm_hour,tm.tm_min,tm.tm_sec);					
					continue;
				}
				if(!strnicmp(cmd,"DELE ",5)) {
					if((i=remove(fname))==0) {
						sockprintf(sock,"250 \"%s\" removed successfully.",fname);
						lprintf(LOG_NOTICE,"%04d %s deleted file: %s",sock,user.alias,fname);
					} else {
						sockprintf(sock,"450 Error %d removing file: %s",i,fname);
						lprintf(LOG_WARNING,"%04d !%s attempted to delete file: %s (Error %d)"
							,sock,user.alias,fname,i);
					}
					continue;
				}
				/* RETR */
				lprintf(LOG_INFO,"%04d %s downloading: %s (%lu bytes) in %s mode"
					,sock,user.alias,fname,flength(fname)
					,mode);
				sockprintf(sock,"150 Opening BINARY mode data connection for file transfer.");
				filexfer(&data_addr,sock,pasv_sock,&data_sock,fname,filepos
					,&transfer_inprogress,&transfer_aborted,FALSE,FALSE
					,&lastactive,&user,-1,FALSE,FALSE,FALSE,NULL);
				continue;
			} /* Local RETR/SIZE/MDTM */

			if(!strnicmp(cmd, "STOR ", 5) || !strnicmp(cmd, "APPE ", 5)) {
				p=cmd+5;
				while(*p && *p<=' ') p++;

				if(!strnicmp(p,LOCAL_FSYS_DIR,strlen(LOCAL_FSYS_DIR))) 
					p+=strlen(LOCAL_FSYS_DIR);	/* already mounted */

				if(p[1]==':')		/* drive specified */
					SAFECOPY(fname,p);
				else if(*p=='/')	/* absolute, current drive */
					SAFEPRINTF2(fname,"%s%s",root_dir(local_dir),p+1);
				else				/* relative */
					SAFEPRINTF2(fname,"%s%s",local_dir,p);

				lprintf(LOG_INFO,"%04d %s uploading: %s in %s mode", sock,user.alias,fname
					,mode);
				sockprintf(sock,"150 Opening BINARY mode data connection for file transfer.");
				filexfer(&data_addr,sock,pasv_sock,&data_sock,fname,filepos
					,&transfer_inprogress,&transfer_aborted,FALSE,FALSE
					,&lastactive
					,&user
					,-1		/* dir */
					,TRUE	/* uploading */
					,FALSE	/* credits */
					,!strnicmp(cmd,"APPE",4) ? TRUE : FALSE	/* append */
					,NULL	/* desc */
					);
				filepos=0;
				continue;
			} /* Local STOR */
		}

		if(!strnicmp(cmd, "LIST", 4) || !strnicmp(cmd, "NLST", 4)) {	
			dir=curdir;
			lib=curlib;

			if(cmd[4]!=0) 
				lprintf(LOG_DEBUG,"%04d LIST/NLST: %s",sock,cmd);

			/* path specified? */
			p=cmd+4;
			while(*p && *p<=' ') p++;

			if(*p=='-') {	/* -Letc */
				while(*p && *p>' ') p++;
				while(*p && *p<=' ') p++;
			}

			parsepath(&p,&user,&lib,&dir);

			if((fp=fopen(ftp_tmpfname(fname,sock),"w+b"))==NULL) {
				lprintf(LOG_ERR,"%04d !ERROR %d opening %s",sock,errno,fname);
				sockprintf(sock, "451 Insufficient system storage");
				continue;
			}
			if(!strnicmp(cmd, "LIST", 4))
				detail=TRUE;
			else
				detail=FALSE;
			sockprintf(sock,"150 Opening ASCII mode data connection for /bin/ls.");
			now=time(NULL);
			if(localtime_r(&now,&cur_tm)==NULL) 
				memset(&cur_tm,0,sizeof(cur_tm));

			/* ASCII Index File */
			if(startup->options&FTP_OPT_INDEX_FILE && startup->index_file_name[0]
				&& (!stricmp(p,startup->index_file_name) || *p==0 || *p=='*')) {
				if(detail)
					fprintf(fp,"-r--r--r--   1 %-*s %-8s %9ld %s %2d %02d:%02d %s\r\n"
						,NAME_LEN
						,scfg.sys_id
						,lib<0 ? scfg.sys_id : dir<0 
							? scfg.lib[lib]->sname : scfg.dir[dir]->code_suffix
						,512L
						,ftp_mon[cur_tm.tm_mon],cur_tm.tm_mday,cur_tm.tm_hour,cur_tm.tm_min
						,startup->index_file_name);
				else
					fprintf(fp,"%s\r\n",startup->index_file_name);
			} 
			/* HTML Index File */
			if(startup->options&FTP_OPT_HTML_INDEX_FILE && startup->html_index_file[0]
				&& (!stricmp(p,startup->html_index_file) || *p==0 || *p=='*')) {
				if(detail)
					fprintf(fp,"-r--r--r--   1 %-*s %-8s %9ld %s %2d %02d:%02d %s\r\n"
						,NAME_LEN
						,scfg.sys_id
						,lib<0 ? scfg.sys_id : dir<0 
							? scfg.lib[lib]->sname : scfg.dir[dir]->code_suffix
						,512L
						,ftp_mon[cur_tm.tm_mon],cur_tm.tm_mday,cur_tm.tm_hour,cur_tm.tm_min
						,startup->html_index_file);
				else
					fprintf(fp,"%s\r\n",startup->html_index_file);
			} 

			if(lib<0) { /* Root dir */
				lprintf(LOG_INFO,"%04d %s listing: root in %s mode",sock,user.alias, mode);

				/* QWK Packet */
				if(startup->options&FTP_OPT_ALLOW_QWK/* && fexist(qwkfile)*/) {
					if(detail) {
						if(fexistcase(qwkfile)) {
							t=fdate(qwkfile);
							l=flength(qwkfile);
						} else {
							t=time(NULL);
							l=10240;
						};
						if(localtime_r(&t,&tm)==NULL) 
							memset(&tm,0,sizeof(tm));
						fprintf(fp,"-r--r--r--   1 %-*s %-8s %9ld %s %2d %02d:%02d %s.qwk\r\n"
							,NAME_LEN
							,scfg.sys_id
							,scfg.sys_id
							,l
							,ftp_mon[tm.tm_mon],tm.tm_mday,tm.tm_hour,tm.tm_min
							,scfg.sys_id);
					} else
						fprintf(fp,"%s.qwk\r\n",scfg.sys_id);
				} 

				/* File Aliases */
				sprintf(aliasfile,"%sftpalias.cfg",scfg.ctrl_dir);
				if((alias_fp=fopen(aliasfile,"r"))!=NULL) {

					while(!feof(alias_fp)) {
						if(!fgets(aliasline,sizeof(aliasline),alias_fp))
							break;

						alias_dir=FALSE;

						p=aliasline;		/* alias pointer */
						while(*p && *p<=' ') p++;

						if(*p==';')	/* comment */
							continue;

						tp=p;		/* terminator pointer */
						while(*tp && *tp>' ') tp++;
						if(*tp) *tp=0;

						np=tp+1;	/* filename pointer */
						while(*np && *np<=' ') np++;

						tp=np;		/* terminator pointer */
						while(*tp && *tp>' ') tp++;
						if(*tp) *tp=0;

						dp=tp+1;	/* description pointer */
						while(*dp && *dp<=' ') dp++;
						truncsp(dp);

						if(stricmp(dp,BBS_HIDDEN_ALIAS)==0)
							continue;

						/* Virtual Path? */
						if(!strnicmp(np,BBS_VIRTUAL_PATH,strlen(BBS_VIRTUAL_PATH))) {
							if((dir=getdir(np+strlen(BBS_VIRTUAL_PATH),&user))<0)
								continue; /* No access or invalid virtual path */
							tp=strrchr(np,'/');
							if(tp==NULL) 
								continue;
							tp++;
							if(*tp) {
								SAFEPRINTF2(aliasfile,"%s%s",scfg.dir[dir]->path,tp);
								np=aliasfile;
							}
							else 
								alias_dir=TRUE;
						}

						if(!alias_dir && !fexist(np))
							continue;

						if(detail) {

							if(alias_dir==TRUE) {
								fprintf(fp,"drwxrwxrwx   1 %-*s %-8s %9ld %s %2d %02d:%02d %s\r\n"
									,NAME_LEN
									,scfg.sys_id
									,scfg.lib[scfg.dir[dir]->lib]->sname
									,512L
									,ftp_mon[cur_tm.tm_mon],cur_tm.tm_mday,cur_tm.tm_hour,cur_tm.tm_min
									,p);
							}
							else {
								t=fdate(np);
								if(localtime_r(&t,&tm)==NULL)
									memset(&tm,0,sizeof(tm));
								fprintf(fp,"-r--r--r--   1 %-*s %-8s %9ld %s %2d %02d:%02d %s\r\n"
									,NAME_LEN
									,scfg.sys_id
									,scfg.sys_id
									,flength(np)
									,ftp_mon[tm.tm_mon],tm.tm_mday,tm.tm_hour,tm.tm_min
									,p);
							}
						} else
							fprintf(fp,"%s\r\n",p);

					}

					fclose(alias_fp);
				}

				/* Library folders */
				for(i=0;i<scfg.total_libs;i++) {
					if(!chk_ar(&scfg,scfg.lib[i]->ar,&user))
						continue;
					if(detail)
						fprintf(fp,"dr-xr-xr-x   1 %-*s %-8s %9ld %s %2d %02d:%02d %s\r\n"
							,NAME_LEN
							,scfg.sys_id
							,scfg.sys_id
							,512L
							,ftp_mon[cur_tm.tm_mon],cur_tm.tm_mday,cur_tm.tm_hour,cur_tm.tm_min
							,scfg.lib[i]->sname);
					else
						fprintf(fp,"%s\r\n",scfg.lib[i]->sname);
				}
			} else if(dir<0) {
				lprintf(LOG_INFO,"%04d %s listing: %s library in %s mode"
					,sock,user.alias,scfg.lib[lib]->sname,mode);
				for(i=0;i<scfg.total_dirs;i++) {
					if(scfg.dir[i]->lib!=lib)
						continue;
					if(i!=scfg.sysop_dir && i!=scfg.upload_dir 
						&& !chk_ar(&scfg,scfg.dir[i]->ar,&user))
						continue;
					if(detail)
						fprintf(fp,"drwxrwxrwx   1 %-*s %-8s %9ld %s %2d %02d:%02d %s\r\n"
							,NAME_LEN
							,scfg.sys_id
							,scfg.lib[lib]->sname
							,512L
							,ftp_mon[cur_tm.tm_mon],cur_tm.tm_mday,cur_tm.tm_hour,cur_tm.tm_min
							,scfg.dir[i]->code_suffix);
					else
						fprintf(fp,"%s\r\n",scfg.dir[i]->code_suffix);
				}
			} else if(chk_ar(&scfg,scfg.dir[dir]->ar,&user)) {
				lprintf(LOG_INFO,"%04d %s listing: %s/%s directory in %s mode"
					,sock,user.alias,scfg.lib[lib]->sname,scfg.dir[dir]->code_suffix,mode);

				SAFEPRINTF2(path,"%s%s",scfg.dir[dir]->path,*p ? p : "*");
				glob(path,0,NULL,&g);
				for(i=0;i<(int)g.gl_pathc;i++) {
					if(isdir(g.gl_pathv[i]))
						continue;
#ifdef _WIN32
					GetShortPathName(g.gl_pathv[i], str, sizeof(str));
#else
					SAFECOPY(str,g.gl_pathv[i]);
#endif
					padfname(getfname(str),f.name);
					f.dir=dir;
					if((filedat=getfileixb(&scfg,&f))==FALSE
						&& !(startup->options&FTP_OPT_DIR_FILES))
						continue;
					if(detail) {
						f.size=flength(g.gl_pathv[i]);
						getfiledat(&scfg,&f);
						t=fdate(g.gl_pathv[i]);
						if(localtime_r(&t,&tm)==NULL)
							memset(&tm,0,sizeof(tm));
						if(filedat) {
							if(f.misc&FM_ANON)
								SAFECOPY(str,ANONYMOUS);
							else
								dotname(f.uler,str);
						} else
							SAFECOPY(str,scfg.sys_id);
						fprintf(fp,"-r--r--r--   1 %-*s %-8s %9ld %s %2d "
							,NAME_LEN
							,str
							,scfg.dir[dir]->code_suffix
							,f.size
							,ftp_mon[tm.tm_mon],tm.tm_mday);
						if(tm.tm_year==cur_tm.tm_year)
							fprintf(fp,"%02d:%02d %s\r\n"
								,tm.tm_hour,tm.tm_min
								,getfname(g.gl_pathv[i]));
						else
							fprintf(fp,"%5d %s\r\n"
								,1900+tm.tm_year
								,getfname(g.gl_pathv[i]));
					} else
						fprintf(fp,"%s\r\n",getfname(g.gl_pathv[i]));
				}
				globfree(&g);
			} else 
				lprintf(LOG_INFO,"%04d %s listing: %s/%s directory in %s mode (empty - no access)"
					,sock,user.alias,scfg.lib[lib]->sname,scfg.dir[dir]->code_suffix,mode);

			fclose(fp);
			filexfer(&data_addr,sock,pasv_sock,&data_sock,fname,0L
				,&transfer_inprogress,&transfer_aborted
				,TRUE /* delfile */
				,TRUE /* tmpfile */
				,&lastactive,&user,dir,FALSE,FALSE,FALSE,NULL);
			continue;
		}

		if(!strnicmp(cmd, "RETR ", 5) 
			|| !strnicmp(cmd, "SIZE ",5) 
			|| !strnicmp(cmd, "MDTM ",5)
			|| !strnicmp(cmd, "DELE ",5)) {
			getdate=FALSE;
			getsize=FALSE;
			delecmd=FALSE;
			if(!strnicmp(cmd,"SIZE ",5))
				getsize=TRUE;
			else if(!strnicmp(cmd,"MDTM ",5))
				getdate=TRUE;
			else if(!strnicmp(cmd,"DELE ",5))
				delecmd=TRUE;

			if(!getsize && !getdate && user.rest&FLAG('D')) {
				sockprintf(sock,"550 Insufficient access.");
				filepos=0;
				continue;
			}
			credits=TRUE;
			success=FALSE;
			delfile=FALSE;
			tmpfile=FALSE;
			lib=curlib;
			dir=curdir;

			p=cmd+5;
			while(*p && *p<=' ') p++;

			if(!strnicmp(p,BBS_FSYS_DIR,strlen(BBS_FSYS_DIR))) 
				p+=strlen(BBS_FSYS_DIR);	/* already mounted */

			if(*p=='/') {
				lib=-1;
				p++;
			}
			else if(!strncmp(p,"./",2))
				p+=2;

			if(lib<0 && ftpalias(p, fname, &user, &dir)==TRUE) {
				success=TRUE;
				credits=TRUE;	/* include in d/l stats */
				tmpfile=FALSE;
				delfile=FALSE;
				lprintf(LOG_INFO,"%04d %s %.4s by alias: %s"
					,sock,user.alias,cmd,p);
				p=getfname(fname);
				if(dir>=0)
					lib=scfg.dir[dir]->lib;
			}
			if(!success && lib<0 && (tp=strchr(p,'/'))!=NULL) {
				dir=-1;
				*tp=0;
				for(i=0;i<scfg.total_libs;i++) {
					if(!chk_ar(&scfg,scfg.lib[i]->ar,&user))
						continue;
					if(!stricmp(scfg.lib[i]->sname,p))
						break;
				}
				if(i<scfg.total_libs) 
					lib=i;
				p=tp+1;
			}
			if(!success && dir<0 && (tp=strchr(p,'/'))!=NULL) {
				*tp=0;
				for(i=0;i<scfg.total_dirs;i++) {
					if(scfg.dir[i]->lib!=lib)
						continue;
					if(!chk_ar(&scfg,scfg.dir[i]->ar,&user))
						continue;
					if(!stricmp(scfg.dir[i]->code_suffix,p))
						break;
				}
				if(i<scfg.total_dirs) 
					dir=i;
				p=tp+1;
			}

			sprintf(html_index_ext,"%s?",startup->html_index_file);

			sprintf(str,"%s.qwk",scfg.sys_id);
			if(lib<0 && startup->options&FTP_OPT_ALLOW_QWK 
				&& !stricmp(p,str) && !delecmd) {
				if(!fexistcase(qwkfile)) {
					lprintf(LOG_INFO,"%04d %s creating QWK packet...",sock,user.alias);
					sprintf(str,"%spack%04u.now",scfg.data_dir,user.number);
					if(!ftouch(str))
						lprintf(LOG_ERR,"%04d !ERROR creating semaphore file: %s"
							,sock, str);
					t=time(NULL);
					while(fexist(str)) {
						if(time(NULL)-t>startup->qwk_timeout)
							break;
						mswait(1000);
					}
					if(fexist(str)) {
						lprintf(LOG_WARNING,"%04d !TIMEOUT waiting for QWK packet creation",sock);
						sockprintf(sock,"451 Time-out waiting for packet creation.");
						remove(str);
						filepos=0;
						continue;
					}
					if(!fexistcase(qwkfile)) {
						lprintf(LOG_INFO,"%04d No QWK Packet created (no new messages)",sock);
						sockprintf(sock,"550 No QWK packet created (no new messages)");
						filepos=0;
						continue;
					}
				}
				SAFECOPY(fname,qwkfile);
				success=TRUE;
				delfile=TRUE;
				credits=FALSE;
				if(!getsize && !getdate)
					lprintf(LOG_INFO,"%04d %s downloading QWK packet (%lu bytes) in %s mode"
						,sock,user.alias,flength(fname)
						,mode);
			/* ASCII Index File */
			} else if(startup->options&FTP_OPT_INDEX_FILE 
				&& !stricmp(p,startup->index_file_name)
				&& !delecmd) {
				if((fp=fopen(ftp_tmpfname(fname,sock),"w+b"))==NULL) {
					lprintf(LOG_ERR,"%04d !ERROR %d opening %s",sock,errno,fname);
					sockprintf(sock, "451 Insufficient system storage");
					filepos=0;
					continue;
				}
				if(!getsize && !getdate)
					lprintf(LOG_INFO,"%04d %s downloading index for %s in %s mode"
						,sock,user.alias,vpath(lib,dir,str)
						,mode);
				success=TRUE;
				credits=FALSE;
				tmpfile=TRUE;
				delfile=TRUE;
				fprintf(fp,"%-*s File/Folder Descriptions\r\n"
					,INDEX_FNAME_LEN,startup->index_file_name);
				if(startup->options&FTP_OPT_HTML_INDEX_FILE)
					fprintf(fp,"%-*s File/Folder Descriptions (HTML)\r\n"
						,INDEX_FNAME_LEN,startup->html_index_file);
				if(lib<0) {

					/* File Aliases */
					sprintf(aliasfile,"%sftpalias.cfg",scfg.ctrl_dir);
					if((alias_fp=fopen(aliasfile,"r"))!=NULL) {

						while(!feof(alias_fp)) {
							if(!fgets(aliasline,sizeof(aliasline),alias_fp))
								break;

							p=aliasline;	/* alias pointer */
							while(*p && *p<=' ') p++;

							if(*p==';')	/* comment */
								continue;

							tp=p;		/* terminator pointer */
							while(*tp && *tp>' ') tp++;
							if(*tp) *tp=0;

							np=tp+1;	/* filename pointer */
							while(*np && *np<=' ') np++;

							np++;		/* description pointer */
							while(*np && *np>' ') np++;

							while(*np && *np<' ') np++;

							truncsp(np);

							fprintf(fp,"%-*s %s\r\n",INDEX_FNAME_LEN,p,np);
						}

						fclose(alias_fp);
					}

					/* QWK Packet */
					if(startup->options&FTP_OPT_ALLOW_QWK /* && fexist(qwkfile) */) {
						sprintf(str,"%s.qwk",scfg.sys_id);
						fprintf(fp,"%-*s QWK Message Packet\r\n"
							,INDEX_FNAME_LEN,str);
					}

					/* Library Folders */
					for(i=0;i<scfg.total_libs;i++) {
						if(!chk_ar(&scfg,scfg.lib[i]->ar,&user))
							continue;
						fprintf(fp,"%-*s %s\r\n"
							,INDEX_FNAME_LEN,scfg.lib[i]->sname,scfg.lib[i]->lname);
					}
				} else if(dir<0) {
					for(i=0;i<scfg.total_dirs;i++) {
						if(scfg.dir[i]->lib!=lib)
							continue;
						if(i!=scfg.sysop_dir && i!=scfg.upload_dir
							&& !chk_ar(&scfg,scfg.dir[i]->ar,&user))
							continue;
						fprintf(fp,"%-*s %s\r\n"
							,INDEX_FNAME_LEN,scfg.dir[i]->code_suffix,scfg.dir[i]->lname);
					}
				} else if(chk_ar(&scfg,scfg.dir[dir]->ar,&user)){
					sprintf(cmd,"%s*",scfg.dir[dir]->path);
					glob(cmd,0,NULL,&g);
					for(i=0;i<(int)g.gl_pathc;i++) {
						if(isdir(g.gl_pathv[i]))
							continue;
#ifdef _WIN32
						GetShortPathName(g.gl_pathv[i], str, sizeof(str));
#else
						SAFECOPY(str,g.gl_pathv[i]);
#endif
						padfname(getfname(str),f.name);
						f.dir=dir;
						if(getfileixb(&scfg,&f)) {
							f.size=flength(g.gl_pathv[i]);
							getfiledat(&scfg,&f);
							fprintf(fp,"%-*s %s\r\n",INDEX_FNAME_LEN
								,getfname(g.gl_pathv[i]),f.desc);
						}
					}
					globfree(&g);
				}
				fclose(fp);
			/* HTML Index File */
			} else if(startup->options&FTP_OPT_HTML_INDEX_FILE 
				&& (!stricmp(p,startup->html_index_file) 
				|| !strnicmp(p,html_index_ext,strlen(html_index_ext)))
				&& !delecmd) {
#ifdef JAVASCRIPT
				if(startup->options&FTP_OPT_NO_JAVASCRIPT) {
					lprintf(LOG_ERR,"%04d !JavaScript disabled, cannot generate %s",sock,fname);
					sockprintf(sock, "451 JavaScript disabled");
					filepos=0;
					continue;
				}
				if(js_runtime == NULL) {
					lprintf(LOG_DEBUG,"%04d JavaScript: Creating runtime: %lu bytes"
						,sock,startup->js.max_bytes);

					if((js_runtime = JS_NewRuntime(startup->js.max_bytes))==NULL) {
						lprintf(LOG_ERR,"%04d !ERROR creating JavaScript runtime",sock);
						sockprintf(sock,"451 Error creating JavaScript runtime");
						filepos=0;
						continue;
					}
				}

				if(js_cx==NULL) {	/* Context not yet created, create it now */
					if(((js_cx=js_initcx(js_runtime, sock,&js_glob,&js_ftp))==NULL)) {
						lprintf(LOG_ERR,"%04d !ERROR initializing JavaScript context",sock);
						sockprintf(sock,"451 Error initializing JavaScript context");
						filepos=0;
						continue;
					}
					if(js_CreateUserClass(js_cx, js_glob, &scfg)==NULL) 
						lprintf(LOG_ERR,"%04d !JavaScript ERROR creating user class",sock);

					if(js_CreateFileClass(js_cx, js_glob)==NULL) 
						lprintf(LOG_ERR,"%04d !JavaScript ERROR creating file class",sock);

					if(js_CreateUserObject(js_cx, js_glob, &scfg, "user", user.number)==NULL) 
						lprintf(LOG_ERR,"%04d !JavaScript ERROR creating user object",sock);

					if(js_CreateClientObject(js_cx, js_glob, "client", &client, sock)==NULL) 
						lprintf(LOG_ERR,"%04d !JavaScript ERROR creating client object",sock);

					if(js_CreateFileAreaObject(js_cx, js_glob, &scfg, &user
						,startup->html_index_file)==NULL) 
						lprintf(LOG_ERR,"%04d !JavaScript ERROR creating file area object",sock);
				}

				if((js_str=JS_NewStringCopyZ(js_cx, "name"))!=NULL) {
					js_val=STRING_TO_JSVAL(js_str);
					JS_SetProperty(js_cx, js_ftp, "sort", &js_val);
				}
				js_val=BOOLEAN_TO_JSVAL(FALSE);
				JS_SetProperty(js_cx, js_ftp, "reverse", &js_val);

				if(!strnicmp(p,html_index_ext,strlen(html_index_ext))) {
					p+=strlen(html_index_ext);
					tp=strrchr(p,'$');
					if(tp!=NULL)
						*tp=0;
					if(!strnicmp(p,"ext=",4)) {
						p+=4;
						if(!strcmp(p,"on"))
							user.misc|=EXTDESC;
						else
							user.misc&=~EXTDESC;
						if(!(user.rest&FLAG('G')))
							putuserrec(&scfg,user.number,U_MISC,8,ultoa(user.misc,str,16));
					} 
					else if(!strnicmp(p,"sort=",5)) {
						p+=5;
						tp=strchr(p,'&');
						if(tp!=NULL) {
							*tp=0;
							tp++;
							if(!stricmp(tp,"reverse")) {
								js_val=BOOLEAN_TO_JSVAL(TRUE);
								JS_SetProperty(js_cx, js_ftp, "reverse", &js_val);
							}
						}
						if((js_str=JS_NewStringCopyZ(js_cx, p))!=NULL) {
							js_val=STRING_TO_JSVAL(js_str);
							JS_SetProperty(js_cx, js_ftp, "sort", &js_val);
						}
					}
				}

				js_val=BOOLEAN_TO_JSVAL(INT_TO_BOOL(user.misc&EXTDESC));
				JS_SetProperty(js_cx, js_ftp, "extended_descriptions", &js_val);

#endif
				if((fp=fopen(ftp_tmpfname(fname,sock),"w+b"))==NULL) {
					lprintf(LOG_ERR,"%04d !ERROR %d opening %s",sock,errno,fname);
					sockprintf(sock, "451 Insufficient system storage");
					filepos=0;
					continue;
				}
				if(!getsize && !getdate)
					lprintf(LOG_INFO,"%04d %s downloading HTML index for %s in %s mode"
						,sock,user.alias,vpath(lib,dir,str)
						,mode);
				success=TRUE;
				credits=FALSE;
				tmpfile=TRUE;
				delfile=TRUE;
#ifdef JAVASCRIPT
				js_val=INT_TO_JSVAL(timeleft);
				if(!JS_SetProperty(js_cx, js_ftp, "time_left", &js_val))
					lprintf(LOG_ERR,"%04d !JavaScript ERROR setting user.time_left",sock);
				js_generate_index(js_cx, js_ftp, sock, fp, lib, dir, &user);
#endif
				fclose(fp);
			} else if(dir>=0) {

				if(!chk_ar(&scfg,scfg.dir[dir]->ar,&user)) {
					lprintf(LOG_WARNING,"%04d !%s has insufficient access to /%s/%s"
						,sock,user.alias
						,scfg.lib[scfg.dir[dir]->lib]->sname
						,scfg.dir[dir]->code_suffix);
					sockprintf(sock,"550 Insufficient access.");
					filepos=0;
					continue;
				}

				if(!getsize && !getdate && !delecmd
					&& !chk_ar(&scfg,scfg.dir[dir]->dl_ar,&user)) {
					lprintf(LOG_WARNING,"%04d !%s has insufficient access to download from /%s/%s"
						,sock,user.alias
						,scfg.lib[scfg.dir[dir]->lib]->sname
						,scfg.dir[dir]->code_suffix);
					sockprintf(sock,"550 Insufficient access.");
					filepos=0;
					continue;
				}

				if(delecmd && !dir_op(&scfg,&user,dir)) {
					lprintf(LOG_WARNING,"%04d !%s has insufficient access to delete files in /%s/%s"
						,sock,user.alias
						,scfg.lib[scfg.dir[dir]->lib]->sname
						,scfg.dir[dir]->code_suffix);
					sockprintf(sock,"550 Insufficient access.");
					filepos=0;
					continue;
				}
				SAFEPRINTF2(fname,"%s%s",scfg.dir[dir]->path,p);
#ifdef _WIN32
				GetShortPathName(fname, str, sizeof(str));
#else
				SAFECOPY(str,fname);
#endif
				padfname(getfname(str),f.name);
				f.dir=dir;
				f.cdt=0;
				f.size=-1;
				filedat=getfileixb(&scfg,&f);
				if(!filedat && !(startup->options&FTP_OPT_DIR_FILES)) {
					sockprintf(sock,"550 File not found: %s",p);
					lprintf(LOG_WARNING,"%04d !%s file (%s%s) not in database for %.4s command"
						,sock,user.alias,vpath(lib,dir,str),p,cmd);
					filepos=0;
					continue;
				}

				/* Verify credits */
				if(!getsize && !getdate && !delecmd
					&& !is_download_free(&scfg,dir,&user)) {
					if(filedat)
						getfiledat(&scfg,&f);
					else
						f.cdt=flength(fname);
					if(f.cdt>(user.cdt+user.freecdt)) {
						lprintf(LOG_WARNING,"%04d !%s has insufficient credit to download /%s/%s/%s (%lu credits)"
							,sock,user.alias,scfg.lib[scfg.dir[dir]->lib]->sname
							,scfg.dir[dir]->code_suffix
							,p
							,f.cdt);
						sockprintf(sock,"550 Insufficient credit (%lu required).",f.cdt);
						filepos=0;
						continue;
					}
				}

				if(strcspn(p,ILLEGAL_FILENAME_CHARS)!=strlen(p)) {
					success=FALSE;
					lprintf(LOG_WARNING,"%04d !ILLEGAL FILENAME ATTEMPT by %s: %s"
						,sock,user.alias,p);
					hacklog(&scfg, "FTP", user.alias, cmd, host_name, &ftp.client_addr);
#ifdef _WIN32
					if(startup->hack_sound[0] && !(startup->options&FTP_OPT_MUTE)) 
						PlaySound(startup->hack_sound, NULL, SND_ASYNC|SND_FILENAME);
#endif
				} else {
					if(fexist(fname)) {
						success=TRUE;
						if(!getsize && !getdate && !delecmd)
							lprintf(LOG_INFO,"%04d %s downloading: %s (%lu bytes) in %s mode"
								,sock,user.alias,fname,flength(fname)
								,mode);
					} 
				}
			}
#if defined(SOCKET_DEBUG_DOWNLOAD)
			socket_debug[sock]|=SOCKET_DEBUG_DOWNLOAD;
#endif

			if(getsize && success) 
				sockprintf(sock,"213 %lu",flength(fname));
			else if(getdate && success) {
				t=fdate(fname);
				if(gmtime_r(&t,&tm)==NULL)	/* specifically use GMT/UTC representation */
					memset(&tm,0,sizeof(tm));
				sockprintf(sock,"213 %u%02u%02u%02u%02u%02u"
					,1900+tm.tm_year,tm.tm_mon+1,tm.tm_mday
					,tm.tm_hour,tm.tm_min,tm.tm_sec);
			} else if(delecmd && success) {
				if(remove(fname)!=0) {
					lprintf(LOG_ERR,"%04d !ERROR %d deleting %s",sock,errno,fname);
					sockprintf(sock,"450 %s could not be deleted (error: %d)"
						,fname,errno);
				} else {
					lprintf(LOG_NOTICE,"%04d %s deleted %s",sock,user.alias,fname);
					if(filedat) 
						removefiledat(&scfg,&f);
					sockprintf(sock,"250 %s deleted.",fname);
				}
			} else if(success) {
				sockprintf(sock,"150 Opening BINARY mode data connection for file transfer.");
				filexfer(&data_addr,sock,pasv_sock,&data_sock,fname,filepos
					,&transfer_inprogress,&transfer_aborted,delfile,tmpfile
					,&lastactive,&user,dir,FALSE,credits,FALSE,NULL);
			}
			else {
				sockprintf(sock,"550 File not found: %s",p);
				lprintf(LOG_WARNING,"%04d !%s file (%s%s) not found for %.4s command"
					,sock,user.alias,vpath(lib,dir,str),p,cmd);
			}
			filepos=0;
#if defined(SOCKET_DEBUG_DOWNLOAD)
			socket_debug[sock]&=~SOCKET_DEBUG_DOWNLOAD;
#endif
			continue;
		}

		if(!strnicmp(cmd, "DESC", 4)) {

			if(user.rest&FLAG('U')) {
				sockprintf(sock,"553 Insufficient access.");
				continue;
			}

			p=cmd+4;
			while(*p && *p<=' ') p++;

			if(*p==0) 
				sockprintf(sock,"501 No file description given.");
			else {
				SAFECOPY(desc,p);
				sockprintf(sock,"200 File description set. Ready to STOR file.");
			}
			continue;
		}

		if(!strnicmp(cmd, "STOR ", 5) || !strnicmp(cmd, "APPE ", 5)) {

			if(user.rest&FLAG('U')) {
				sockprintf(sock,"553 Insufficient access.");
				continue;
			}

			if(transfer_inprogress==TRUE) {
				lprintf(LOG_WARNING,"%04d !TRANSFER already in progress (%s)",sock,cmd);
				sockprintf(sock,"425 Transfer already in progress.");
				continue;
			}

			append=FALSE;
			lib=curlib;
			dir=curdir;
			p=cmd+5;

			while(*p && *p<=' ') p++;

			if(!strnicmp(p,BBS_FSYS_DIR,strlen(BBS_FSYS_DIR))) 
				p+=strlen(BBS_FSYS_DIR);	/* already mounted */

			if(*p=='/') {
				lib=-1;
				p++;
			}
			else if(!strncmp(p,"./",2))
				p+=2;
			/* Need to add support for uploading to aliased directories */
			if(lib<0 && (tp=strchr(p,'/'))!=NULL) {
				dir=-1;
				*tp=0;
				for(i=0;i<scfg.total_libs;i++) {
					if(!chk_ar(&scfg,scfg.lib[i]->ar,&user))
						continue;
					if(!stricmp(scfg.lib[i]->sname,p))
						break;
				}
				if(i<scfg.total_libs) 
					lib=i;
				p=tp+1;
			}
			if(dir<0 && (tp=strchr(p,'/'))!=NULL) {
				*tp=0;
				for(i=0;i<scfg.total_dirs;i++) {
					if(scfg.dir[i]->lib!=lib)
						continue;
					if(i!=scfg.sysop_dir && i!=scfg.upload_dir 
						&& !chk_ar(&scfg,scfg.dir[i]->ar,&user))
						continue;
					if(!stricmp(scfg.dir[i]->code_suffix,p))
						break;
				}
				if(i<scfg.total_dirs) 
					dir=i;
				p=tp+1;
			}
			if(dir<0) {
				sprintf(str,"%s.rep",scfg.sys_id);
				if(!(startup->options&FTP_OPT_ALLOW_QWK)
					|| stricmp(p,str)) {
					lprintf(LOG_WARNING,"%04d !%s attempted to upload to invalid directory"
						,sock,user.alias);
					sockprintf(sock,"553 Invalid directory.");
					continue;
				}
				sprintf(fname,"%sfile/%04d.rep",scfg.data_dir,user.number);
				lprintf(LOG_INFO,"%04d %s uploading: %s in %s mode"
					,sock,user.alias,fname
					,mode);
			} else {

				append=(strnicmp(cmd,"APPE",4)==0);
			
				if(!chk_ar(&scfg,scfg.dir[dir]->ul_ar,&user)) {
					lprintf(LOG_WARNING,"%04d !%s has insufficient access to upload to /%s/%s"
						,sock,user.alias
						,scfg.lib[scfg.dir[dir]->lib]->sname
						,scfg.dir[dir]->code_suffix);
					sockprintf(sock,"553 Insufficient access.");
					continue;
				}
				if(*p=='-'
					|| strcspn(p,ILLEGAL_FILENAME_CHARS)!=strlen(p)
					|| trashcan(&scfg,p,"file")) {
					lprintf(LOG_WARNING,"%04d !ILLEGAL FILENAME ATTEMPT by %s: %s"
						,sock,user.alias,p);
					sockprintf(sock,"553 Illegal filename attempt");
					hacklog(&scfg, "FTP", user.alias, cmd, host_name, &ftp.client_addr);
#ifdef _WIN32
					if(startup->hack_sound[0] && !(startup->options&FTP_OPT_MUTE)) 
						PlaySound(startup->hack_sound, NULL, SND_ASYNC|SND_FILENAME);
#endif
					continue;
				}
				SAFEPRINTF2(fname,"%s%s",scfg.dir[dir]->path,p);
				if((!append && filepos==0 && fexist(fname))
					|| (startup->options&FTP_OPT_INDEX_FILE 
						&& !stricmp(p,startup->index_file_name))
					|| (startup->options&FTP_OPT_HTML_INDEX_FILE 
						&& !stricmp(p,startup->html_index_file))
					) {
					lprintf(LOG_WARNING,"%04d !%s attempted to overwrite existing file: %s"
						,sock,user.alias,fname);
					sockprintf(sock,"553 File already exists.");
					continue;
				}
				if(append || filepos) {	/* RESUME */
#ifdef _WIN32
					GetShortPathName(fname, str, sizeof(str));
#else
					SAFECOPY(str,fname);
#endif
					padfname(getfname(str),f.name);
					f.dir=dir;
					f.cdt=0;
					f.size=-1;
					if(!getfileixb(&scfg,&f) || !getfiledat(&scfg,&f)) {
						if(filepos) {
							lprintf(LOG_WARNING,"%04d !%s file (%s) not in database for %.4s command"
								,sock,user.alias,fname,cmd);
							sockprintf(sock,"550 File not found: %s",p);
							continue;
						}
						append=FALSE;
					}
					/* Verify user is original uploader */
					if((append || filepos) && stricmp(f.uler,user.alias)) {
						lprintf(LOG_WARNING,"%04d !%s cannot resume upload of %s, uploaded by %s"
							,sock,user.alias,fname,f.uler);
						sockprintf(sock,"553 Insufficient access (can't resume upload from different user).");
						continue;
					}
				}
				lprintf(LOG_INFO,"%04d %s uploading: %s to %s (%s) in %s mode"
					,sock,user.alias
					,p						/* filename */
					,vpath(lib,dir,str)		/* virtual path */
					,scfg.dir[dir]->path	/* actual path */
					,mode);
			}
			sockprintf(sock,"150 Opening BINARY mode data connection for file transfer.");
			filexfer(&data_addr,sock,pasv_sock,&data_sock,fname,filepos
				,&transfer_inprogress,&transfer_aborted,FALSE,FALSE
				,&lastactive
				,&user
				,dir
				,TRUE	/* uploading */
				,TRUE	/* credits */
				,append
				,desc
				);
			filepos=0;
			continue;
		}

		if(!stricmp(cmd,"CDUP") || !stricmp(cmd,"XCUP")) {
			if(curdir<0)
				curlib=-1;
			else
				curdir=-1;
			sockprintf(sock,"200 CDUP command successful.");
			continue;
		}

		if(!strnicmp(cmd, "CWD ", 4) || !strnicmp(cmd,"XCWD ",5)) {
			p=cmd+4;
			while(*p && *p<=' ') p++;

			if(!strnicmp(p,BBS_FSYS_DIR,strlen(BBS_FSYS_DIR))) 
				p+=strlen(BBS_FSYS_DIR);	/* already mounted */

			if(*p=='/') {
				curlib=-1;
				curdir=-1;
				p++;
			}
			/* Local File System? */
			if(sysop && !(startup->options&FTP_OPT_NO_LOCAL_FSYS) 
				&& !strnicmp(p,LOCAL_FSYS_DIR,strlen(LOCAL_FSYS_DIR))) {	
				p+=strlen(LOCAL_FSYS_DIR);
				if(!direxist(p)) {
					sockprintf(sock,"550 Directory does not exist.");
					lprintf(LOG_WARNING,"%04d !%s attempted to mount invalid directory: %s"
						,sock, user.alias, p);
					continue;
				}
				SAFECOPY(local_dir,p);
				local_fsys=TRUE;
				sockprintf(sock,"250 CWD command successful (local file system mounted).");
				lprintf(LOG_INFO,"%04d %s mounted local file system", sock, user.alias);
				continue;
			}
			success=FALSE;

			/* Directory Alias? */
			if(curlib<0 && ftpalias(p,NULL,&user,&curdir)==TRUE) {
				if(curdir>=0)
					curlib=scfg.dir[curdir]->lib;
				success=TRUE;
			}

			orglib=curlib;
			orgdir=curdir;
			tp=0;
			if(!strncmp(p,"...",3)) {
				curlib=-1;
				curdir=-1;
				p+=3;
			}
			if(!strncmp(p,"./",2))
				p+=2;
			else if(!strncmp(p,"..",2)) {
				if(curdir<0)
					curlib=-1;
				else
					curdir=-1;
				p+=2;
			}
			if(*p==0)
				success=TRUE;
			else if(!strcmp(p,".")) 
				success=TRUE;
			if(!success  && (curlib<0 || *p=='/')) { /* Root dir */
				if(*p=='/') p++;
				tp=strchr(p,'/');
				if(tp) *tp=0;
				for(i=0;i<scfg.total_libs;i++) {
					if(!chk_ar(&scfg,scfg.lib[i]->ar,&user))
						continue;
					if(!stricmp(scfg.lib[i]->sname,p))
						break;
				}
				if(i<scfg.total_libs) {
					curlib=i;
					success=TRUE;
				}
			}
			if((!success && curdir<0) || (success && tp && *(tp+1))) {
				if(tp)
					p=tp+1;
				tp=lastchar(p);
				if(tp && *tp=='/') *tp=0;
				for(i=0;i<scfg.total_dirs;i++) {
					if(scfg.dir[i]->lib!=curlib)
						continue;
					if(i!=scfg.sysop_dir && i!=scfg.upload_dir
						&& !chk_ar(&scfg,scfg.dir[i]->ar,&user))
						continue;
					if(!stricmp(scfg.dir[i]->code_suffix,p))
						break;
				}
				if(i<scfg.total_dirs) {
					curdir=i;
					success=TRUE;
				} else
					success=FALSE;
			}

			if(success)
				sockprintf(sock,"250 CWD command successful.");
			else {
				sockprintf(sock,"550 %s: No such file or directory.",p);
				curlib=orglib;
				curdir=orgdir;
			}
			continue;
		}

		if(!stricmp(cmd, "PWD") || !stricmp(cmd,"XPWD")) {
			if(curlib<0)
				sockprintf(sock,"257 \"/\" is current directory.");
			else if(curdir<0)
				sockprintf(sock,"257 \"/%s\" is current directory."
					,scfg.lib[curlib]->sname);
			else
				sockprintf(sock,"257 \"/%s/%s\" is current directory."
					,scfg.lib[curlib]->sname
					,scfg.dir[curdir]->code_suffix);
			continue;
		}

		if(!strnicmp(cmd, "MKD", 3) || 
			!strnicmp(cmd,"XMKD",4) || 
			!strnicmp(cmd,"SITE EXEC",9)) {
			lprintf(LOG_WARNING,"%04d !SUSPECTED HACK ATTEMPT by %s: '%s'"
				,sock,user.alias,cmd);
			hacklog(&scfg, "FTP", user.alias, cmd, host_name, &ftp.client_addr);
#ifdef _WIN32
			if(startup->hack_sound[0] && !(startup->options&FTP_OPT_MUTE)) 
				PlaySound(startup->hack_sound, NULL, SND_ASYNC|SND_FILENAME);
#endif
		}		
		sockprintf(sock,"500 Syntax error: '%s'",cmd);
		lprintf(LOG_WARNING,"%04d !UNSUPPORTED COMMAND from %s: '%s'"
			,sock,user.alias,cmd);
	} /* while(1) */

#if defined(SOCKET_DEBUG_TERMINATE)
	socket_debug[sock]|=SOCKET_DEBUG_TERMINATE;
#endif

	if(transfer_inprogress==TRUE) {
		lprintf(LOG_DEBUG,"%04d Waiting for transfer to complete...",sock);
		count=0;
		while(transfer_inprogress==TRUE) {
			if(server_socket==INVALID_SOCKET || terminate_server) {
				mswait(2000);	/* allow xfer threads to terminate */
				break;
			}
			if(!transfer_aborted) {
				if(gettimeleft(&scfg,&user,logintime)<1) {
					lprintf(LOG_WARNING,"%04d Out of time, disconnecting",sock);
					sockprintf(sock,"421 Sorry, you've run out of time.");
					ftp_close_socket(&data_sock,__LINE__);
					transfer_aborted=TRUE;
				}
				if((time(NULL)-lastactive)>startup->max_inactivity) {
					lprintf(LOG_WARNING,"%04d Disconnecting due to to inactivity",sock);
					sockprintf(sock,"421 Disconnecting due to inactivity (%u seconds)."
						,startup->max_inactivity);
					ftp_close_socket(&data_sock,__LINE__);
					transfer_aborted=TRUE;
				}
			}
			if(count && (count%60)==0)
				lprintf(LOG_WARNING,"%04d Still waiting for transfer to complete "
					"(count=%lu, aborted=%d, lastactive=%lX) ..."
					,sock,count,transfer_aborted,lastactive);
			count++;
			mswait(1000);
		}
		lprintf(LOG_DEBUG,"%04d Done waiting for transfer to complete",sock);
	}

	if(user.number) {
		/* Update User Statistics */
		logoutuserdat(&scfg, &user, time(NULL), logintime);
		/* Remove QWK-pack semaphore file (if left behind) */
		sprintf(str,"%spack%04u.now",scfg.data_dir,user.number);
		remove(str);
		lprintf(LOG_INFO,"%04d %s logged off",sock,user.alias);
	}

#ifdef _WIN32
	if(startup->hangup_sound[0] && !(startup->options&FTP_OPT_MUTE)) 
		PlaySound(startup->hangup_sound, NULL, SND_ASYNC|SND_FILENAME);
#endif

#ifdef JAVASCRIPT
	if(js_cx!=NULL) {
		lprintf(LOG_DEBUG,"%04d JavaScript: Destroying context",sock);
		JS_DestroyContext(js_cx);	/* Free Context */
	}

	if(js_runtime!=NULL) {
		lprintf(LOG_DEBUG,"%04d JavaScript: Destroying runtime",sock);
		JS_DestroyRuntime(js_runtime);
	}

#endif

/*	status(STATUS_WFC); server thread should control status display */

	if(pasv_sock!=INVALID_SOCKET)
		ftp_close_socket(&pasv_sock,__LINE__);
	if(data_sock!=INVALID_SOCKET)
		ftp_close_socket(&data_sock,__LINE__);

	client_off(sock);

#ifdef SOCKET_DEBUG_CTRL
	socket_debug[sock]&=~SOCKET_DEBUG_CTRL;
#endif

#if defined(SOCKET_DEBUG_TERMINATE)
	socket_debug[sock]&=~SOCKET_DEBUG_TERMINATE;
#endif

	tmp_sock=sock;
	ftp_close_socket(&tmp_sock,__LINE__);

	if(active_clients)
		active_clients--, update_clients();

	thread_down();
	lprintf(LOG_DEBUG,"%04d CTRL thread terminated (%d clients, %u threads remain, %lu served)"
		,sock, active_clients, thread_count, served);
}

static void cleanup(int code, int line)
{
#ifdef _DEBUG
	lprintf(LOG_INFO,"0000 cleanup called from line %d",line);
#endif
	free_cfg(&scfg);
	free_text(text);

	semfile_list_free(&recycle_semfiles);
	semfile_list_free(&shutdown_semfiles);

	if(server_socket!=INVALID_SOCKET)
		ftp_close_socket(&server_socket,__LINE__);

	update_clients();

#ifdef _WINSOCKAPI_
	if(WSAInitialized && WSACleanup()!=0) 
		lprintf(LOG_ERR,"0000 !WSACleanup ERROR %d",ERROR_VALUE);
#endif

	thread_down();
	status("Down");
	if(terminate_server || code)
		lprintf(LOG_INFO,"#### FTP Server thread terminated (%u threads remain, %lu clients served)"
			,thread_count, served);
	if(startup!=NULL && startup->terminated!=NULL)
		startup->terminated(startup->cbdata,code);
}

const char* DLLCALL ftp_ver(void)
{
	static char ver[256];
	char compiler[32];

	DESCRIBE_COMPILER(compiler);

	sscanf("$Revision: 1.313 $", "%*s %s", revision);

	sprintf(ver,"%s %s%s  "
		"Compiled %s %s with %s"
		,FTP_SERVER
		,revision
#ifdef _DEBUG
		," Debug"
#else
		,""
#endif
		,__DATE__, __TIME__, compiler);

	return(ver);
}

void DLLCALL ftp_server(void* arg)
{
	char*			p;
	char			path[MAX_PATH+1];
	char			error[256];
	char			compiler[32];
	char			str[256];
	SOCKADDR_IN		server_addr;
	SOCKADDR_IN		client_addr;
	socklen_t		client_addr_len;
	SOCKET			client_socket;
	int				i;
	int				result;
	time_t			t;
	time_t			start;
	time_t			initialized=0;
	fd_set			socket_set;
	ftp_t*			ftp;
	struct timeval	tv;

	ftp_ver();

	startup=(ftp_startup_t*)arg;

#ifdef _THREAD_SUID_BROKEN
	if(thread_suid_broken)
		startup->seteuid(TRUE);
#endif

    if(startup==NULL) {
    	sbbs_beep(100,500);
    	fprintf(stderr, "No startup structure passed!\n");
    	return;
    }

	if(startup->size!=sizeof(ftp_startup_t)) {	/* verify size */
		sbbs_beep(100,500);
		sbbs_beep(300,500);
		sbbs_beep(100,500);
		fprintf(stderr, "Invalid startup structure!\n");
		return;
	}

	/* Setup intelligent defaults */
	if(startup->port==0)					startup->port=IPPORT_FTP;
	if(startup->qwk_timeout==0)				startup->qwk_timeout=600;		/* seconds */
	if(startup->max_inactivity==0)			startup->max_inactivity=300;	/* seconds */
	if(startup->sem_chk_freq==0)			startup->sem_chk_freq=2;		/* seconds */
	if(startup->index_file_name[0]==0)		SAFECOPY(startup->index_file_name,"00index");
	if(startup->html_index_file[0]==0)		SAFECOPY(startup->html_index_file,"00index.html");
	if(startup->html_index_script[0]==0) {	SAFECOPY(startup->html_index_script,"ftp-html.js");
											startup->options|=FTP_OPT_HTML_INDEX_FILE;
	}
	if(startup->options&FTP_OPT_HTML_INDEX_FILE)
		startup->options&=~FTP_OPT_NO_JAVASCRIPT;
	else
		startup->options|=FTP_OPT_NO_JAVASCRIPT;
#ifdef JAVASCRIPT
	if(startup->js.max_bytes==0)			startup->js.max_bytes=JAVASCRIPT_MAX_BYTES;
	if(startup->js.cx_stack==0)				startup->js.cx_stack=JAVASCRIPT_CONTEXT_STACK;

	ZERO_VAR(js_server_props);
	SAFEPRINTF2(js_server_props.version,"%s %s",FTP_SERVER,revision);
	js_server_props.version_detail=ftp_ver();
	js_server_props.clients=&active_clients;
	js_server_props.options=&startup->options;
	js_server_props.interface_addr=&startup->interface_addr;
#endif


	uptime=0;
	served=0;
	startup->recycle_now=FALSE;
	startup->shutdown_now=FALSE;
	terminate_server=FALSE;

	do {

		thread_up(FALSE /* setuid */);

		status("Initializing");

		memset(&scfg, 0, sizeof(scfg));

		lprintf(LOG_INFO,"Synchronet FTP Server Revision %s%s"
			,revision
#ifdef _DEBUG
			," Debug"
#else
			,""
#endif
			);

		DESCRIBE_COMPILER(compiler);

		lprintf(LOG_INFO,"Compiled %s %s with %s", __DATE__, __TIME__, compiler);

		sbbs_srand();	/* Seed random number generator */

		if(!winsock_startup()) {
			cleanup(1,__LINE__);
			return;
		}

		t=time(NULL);
		lprintf(LOG_INFO,"Initializing on %.24s with options: %lx"
			,CTIME_R(&t,str),startup->options);

		/* Initial configuration and load from CNF files */
		SAFECOPY(scfg.ctrl_dir, startup->ctrl_dir);
		lprintf(LOG_INFO,"Loading configuration files from %s", scfg.ctrl_dir);
		scfg.size=sizeof(scfg);
		SAFECOPY(error,UNKNOWN_LOAD_ERROR);
		if(!load_cfg(&scfg, text, TRUE, error)) {
			lprintf(LOG_ERR,"!ERROR %s",error);
			lprintf(LOG_ERR,"!Failed to load configuration files");
			cleanup(1,__LINE__);
			return;
		}

		if(startup->host_name[0]==0)
			SAFECOPY(startup->host_name,scfg.sys_inetaddr);

		if(!(scfg.sys_misc&SM_LOCAL_TZ) && !(startup->options&FTP_OPT_LOCAL_TIMEZONE)) { 
			if(putenv("TZ=UTC0"))
				lprintf(LOG_ERR,"!putenv() FAILED");
			tzset();

			if((t=checktime())!=0) {   /* Check binary time */
				lprintf(LOG_ERR,"!TIME PROBLEM (%ld)",t);
				cleanup(1,__LINE__);
				return;
			}
		}

		if(uptime==0)
			uptime=time(NULL);	/* this must be done *after* setting the timezone */

		if(startup->temp_dir[0])
			SAFECOPY(scfg.temp_dir,startup->temp_dir);
		else
			SAFECOPY(scfg.temp_dir,"../temp");
	   	prep_dir(scfg.ctrl_dir, scfg.temp_dir, sizeof(scfg.temp_dir));
		MKDIR(scfg.temp_dir);
		lprintf(LOG_DEBUG,"Temporary file directory: %s", scfg.temp_dir);
		if(!isdir(scfg.temp_dir)) {
			lprintf(LOG_ERR,"!Invalid temp directory: %s", scfg.temp_dir);
			cleanup(1,__LINE__);
			return;
		}

		if(!startup->max_clients) {
			startup->max_clients=scfg.sys_nodes;
			if(startup->max_clients<10)
				startup->max_clients=10;
		}
		lprintf(LOG_DEBUG,"Maximum clients: %d",startup->max_clients);

		/* Sanity-check the passive port range */
		if(startup->pasv_port_low || startup->pasv_port_high) {
			if(startup->pasv_port_low > startup->pasv_port_high
				|| startup->pasv_port_high-startup->pasv_port_low < (startup->max_clients-1)) {
				lprintf(LOG_WARNING,"!Correcting Passive Port Range (Low: %u, High: %u)"
					,startup->pasv_port_low,startup->pasv_port_high);
				if(startup->pasv_port_low)
					startup->pasv_port_high = startup->pasv_port_low+(startup->max_clients-1);
				else
					startup->pasv_port_low = startup->pasv_port_high-(startup->max_clients-1);
			}
			lprintf(LOG_DEBUG,"Passive Port Low: %u",startup->pasv_port_low);
			lprintf(LOG_DEBUG,"Passive Port High: %u",startup->pasv_port_high);
		}

		lprintf(LOG_DEBUG,"Maximum inactivity: %d seconds",startup->max_inactivity);

		active_clients=0, update_clients();

		strlwr(scfg.sys_id); /* Use lower-case unix-looking System ID for group name */

		for(i=0;i<scfg.total_libs;i++) {
			strlwr(scfg.lib[i]->sname);
			dotname(scfg.lib[i]->sname,scfg.lib[i]->sname);
		}
#if 0	/* this is now handled by load_cfg()->prep_cfg() */
		for(i=0;i<scfg.total_dirs;i++) 
			strlwr(scfg.dir[i]->code_suffix);
#endif
		/* open a socket and wait for a client */

		if((server_socket=ftp_open_socket(SOCK_STREAM))==INVALID_SOCKET) {
			lprintf(LOG_ERR,"!ERROR %d opening socket", ERROR_VALUE);
			cleanup(1,__LINE__);
			return;
		}

		lprintf(LOG_DEBUG,"%04d FTP Server socket opened",server_socket);

		/*****************************/
		/* Listen for incoming calls */
		/*****************************/
		memset(&server_addr, 0, sizeof(server_addr));

		server_addr.sin_addr.s_addr = htonl(startup->interface_addr);
		server_addr.sin_family = AF_INET;
		server_addr.sin_port   = htons(startup->port);

		if(startup->seteuid!=NULL)
			startup->seteuid(FALSE);
		result=retry_bind(server_socket, (struct sockaddr *) &server_addr,sizeof(server_addr)
			,startup->bind_retry_count,startup->bind_retry_delay,"FTP Server",lprintf);
		if(startup->seteuid!=NULL)
			startup->seteuid(TRUE);
		if(result!=0) {
			lprintf(LOG_ERR,"%04d %s", server_socket, BIND_FAILURE_HELP);
			cleanup(1,__LINE__);
			return;
		}

		if((result=listen(server_socket, 1))!= 0) {
			lprintf(LOG_ERR,"%04d !ERROR %d (%d) listening on socket"
				,server_socket, result, ERROR_VALUE);
			cleanup(1,__LINE__);
			return;
		}

		lprintf(LOG_NOTICE,"%04d FTP Server thread started on port %d",server_socket,startup->port);
		status(STATUS_WFC);

		/* Setup recycle/shutdown semaphore file lists */
		shutdown_semfiles=semfile_list_init(scfg.ctrl_dir,"shutdown","ftp");
		recycle_semfiles=semfile_list_init(scfg.ctrl_dir,"recycle","ftp");
		SAFEPRINTF(path,"%sftpsrvr.rec",scfg.ctrl_dir);	/* legacy */
		semfile_list_add(&recycle_semfiles,path);
		if(!initialized) {
			initialized=time(NULL);
			semfile_list_check(&initialized,recycle_semfiles);
			semfile_list_check(&initialized,shutdown_semfiles);
		}


		/* signal caller that we've started up successfully */
		if(startup->started!=NULL)
    		startup->started(startup->cbdata);

		while(server_socket!=INVALID_SOCKET && !terminate_server) {

			if(active_clients==0) {
				if(!(startup->options&FTP_OPT_NO_RECYCLE)) {
					if((p=semfile_list_check(&initialized,recycle_semfiles))!=NULL) {
						lprintf(LOG_INFO,"0000 Recycle semaphore file (%s) detected",p);
						break;
					}
#if 0	/* unused */
					if(startup->recycle_sem!=NULL && sem_trywait(&startup->recycle_sem)==0)
						startup->recycle_now=TRUE;
#endif
					if(startup->recycle_now==TRUE) {
						lprintf(LOG_NOTICE,"0000 Recycle semaphore signaled");
						startup->recycle_now=FALSE;
						break;
					}
				}
				if(((p=semfile_list_check(&initialized,shutdown_semfiles))!=NULL
						&& lprintf(LOG_INFO,"0000 Shutdown semaphore file (%s) detected",p))
					|| (startup->shutdown_now==TRUE
						&& lprintf(LOG_INFO,"0000 Shutdown semaphore signaled"))) {
					startup->shutdown_now=FALSE;
					terminate_server=TRUE;
					break;
				}
			}
			/* now wait for connection */

			tv.tv_sec=startup->sem_chk_freq;
			tv.tv_usec=0;

			FD_ZERO(&socket_set);
			FD_SET(server_socket,&socket_set);

			if((i=select(server_socket+1,&socket_set,NULL,NULL,&tv))<1) {
				if(i==0)
					continue;
				if(ERROR_VALUE==EINTR)
					lprintf(LOG_DEBUG,"%04d FTP Server listening interrupted", server_socket);
				else if(ERROR_VALUE == ENOTSOCK)
            		lprintf(LOG_NOTICE,"%04d FTP Server socket closed", server_socket);
				else
					lprintf(LOG_WARNING,"%04d !ERROR %d selecting socket",server_socket, ERROR_VALUE);
				continue;
			}

			if(server_socket==INVALID_SOCKET || terminate_server)	/* terminated */
				break;

			client_addr_len = sizeof(client_addr);
			client_socket = accept(server_socket, (struct sockaddr *)&client_addr
        		,&client_addr_len);

			if(client_socket == INVALID_SOCKET)
			{
#if 0	/* is this necessary still? */
				if(ERROR_VALUE == ENOTSOCK || ERROR_VALUE == EINTR || ERROR_VALUE == EINVAL) {
            		lprintf(LOG_NOTICE,"0000 FTP socket closed while listening");
					break;
				}
#endif
				lprintf(LOG_WARNING,"%04d !ERROR %d accepting connection"
					,server_socket, ERROR_VALUE);
#ifdef _WIN32
				if(WSAGetLastError()==WSAENOBUFS)	/* recycle (re-init WinSock) on this error */
					break;
#endif
				continue;
			}
			if(startup->socket_open!=NULL)
				startup->socket_open(startup->cbdata,TRUE);
			sockets++;

			if(trashcan(&scfg,inet_ntoa(client_addr.sin_addr),"ip-silent")) {
				ftp_close_socket(&client_socket,__LINE__);
				continue;
			}

			if(active_clients>=startup->max_clients) {
				lprintf(LOG_WARNING,"%04d !MAXIMUM CLIENTS (%d) reached, access denied"
					,client_socket, startup->max_clients);
				sockprintf(client_socket,"421 Maximum active clients reached, please try again later.");
				mswait(3000);
				ftp_close_socket(&client_socket,__LINE__);
				continue;
			}

			if((ftp=malloc(sizeof(ftp_t)))==NULL) {
				lprintf(LOG_CRIT,"%04d !ERROR allocating %d bytes of memory for ftp_t"
					,client_socket,sizeof(ftp_t));
				sockprintf(client_socket,"421 System error, please try again later.");
				mswait(3000);
				ftp_close_socket(&client_socket,__LINE__);
				continue;
			}

			ftp->socket=client_socket;
			ftp->client_addr=client_addr;

			_beginthread (ctrl_thread, 0, ftp);
			served++;
		}

#ifdef _DEBUG
		lprintf(LOG_DEBUG,"0000 server_socket: %d",server_socket);
		lprintf(LOG_DEBUG,"0000 terminate_server: %d",terminate_server);
#endif
		if(active_clients) {
			lprintf(LOG_DEBUG,"%04d Waiting for %d active clients to disconnect..."
				,server_socket, active_clients);
			start=time(NULL);
			while(active_clients) {
				if(time(NULL)-start>startup->max_inactivity) {
					lprintf(LOG_WARNING,"%04d !TIMEOUT waiting for %d active clients"
						,server_socket, active_clients);
					break;
				}
				mswait(100);
			}
		}

		if(thread_count>1) {
			lprintf(LOG_DEBUG,"%04d Waiting for %d threads to terminate..."
				,server_socket, thread_count-1);
			start=time(NULL);
			while(thread_count>1) {
				if(time(NULL)-start>TIMEOUT_THREAD_WAIT) {
					lprintf(LOG_WARNING,"%04d !TIMEOUT waiting for %d threads"
						,server_socket, thread_count-1);
					break;
				}
				mswait(100);
			}
		}

		cleanup(0,__LINE__);

		if(!terminate_server) {
			lprintf(LOG_INFO,"Recycling server...");
			mswait(2000);
			if(startup->recycle!=NULL)
				startup->recycle(startup->cbdata);
		}

	} while(!terminate_server);
}
